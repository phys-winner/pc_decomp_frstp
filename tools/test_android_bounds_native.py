"""Compare the native bounding transform with the untouched shipped ARM object."""
import argparse,hashlib,json,statistics,subprocess,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import fastbuild as fb
OUT=ROOT/'build/test-native-bounds'
ORIGINAL=ROOT/'build/perf/native-cpu-20261113/runtime-all/0804/candidate-renamed.o'
ORIGINAL_SHA='aa108647559ddbddf26cebdb8f859b99f2d36b3232a40032fb1649839c2e1382'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(command):
 result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=180)
 if result.returncode:raise RuntimeError(result.stdout[-2000:]+result.stderr[-4000:])
 return result.stdout
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--serial',required=True);args=parser.parse_args()
 OUT.mkdir(parents=True,exist_ok=True);old=ORIGINAL;assert sha(old)==ORIGINAL_SHA
 native=ROOT/'android/host/bounds_native.c';fixture=ROOT/'tests/android_bounds_native_fixture.c'
 with old.open('rb') as f:
  e=ELFFile(f);original_text=e.get_section_by_name('.text').data();undefined=[s.name for s in e.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']=='SHN_UNDEF' and s.name]
 renamed=OUT/'production-original.o';run([fb.BIN/'llvm-objcopy.exe','--redefine-sym=L_000140E2C200=fp_bounds_reference',old,renamed])
 with renamed.open('rb') as f:assert ELFFile(f).get_section_by_name('.text').data()==original_text
 stubs=OUT/'unreached.S';allowed={'g_image_delta','_setjmp','memcpy','memset','memmove','memcmp','__stack_chk_guard','__stack_chk_fail','__errno'}
 stubs.write_text('.text\n'+''.join('.p2align 2\n.global '+name+'\n.type '+name+',%function\n'+name+':\n brk #0\n' for name in undefined if name not in allowed)+'\n.section .note.GNU-stack,"",%progbits\n')
 flags=[*fb.COMPILE_FLAGS,*['-I'+str(ROOT/d) for d in fb.INCLUDES]]
 objects=[]
 for source in [native,fixture,stubs]:
  obj=OUT/(source.stem+'.o');run([fb.CLANG,*flags,'-c',source,'-o',obj]);objects.append(obj)
 binary=OUT/'bounds-arm64';run([fb.CLANG,'--target=aarch64-linux-android30',*objects,renamed,'-Wl,-z,max-page-size=16384','-lm','-o',binary])
 adb=['adb','-s',args.serial];assert not run([*adb,'shell','pidof com.frostpunk.staticrecomp || true']).strip();assert run([*adb,'shell','getprop ro.serialno']).strip()==args.serial
 remote='/data/local/tmp/frostpunk-bounds-'+sha(binary)[:16]
 try:
  run([*adb,'push',binary,remote]);run([*adb,'shell','chmod','700',remote]);test=json.loads(run([*adb,'shell',remote]));assert test['success'] and test['fast_cases']>1000 and test['fallback_cases']>1000
  bench=json.loads(run([*adb,'shell',remote,'--benchmark']));assert bench['success']
 finally:run([*adb,'shell','rm','-f',remote])
 sources=[Path(__file__),native,fixture,stubs,old,ROOT/'native/runtime64/cpu64.h',ROOT/'native/runtime64/sse_scalar.h']
 medians={k:statistics.median(r[k] for r in bench['pairs']) for k in ['original_ns','native_ns']}
 result=dict(success=True,device=test,benchmark=bench,medians=medians,reduction_pct=100*(1-medians['native_ns']/medians['original_ns']),original_text_unchanged=True,sources={str(p):sha(p) for p in sources},native_object_sha256=sha(OUT/'bounds_native.o'),serial=args.serial,original_object_sha256=sha(old))
 (OUT/'result.json').write_text(json.dumps(result,indent=1)+'\n');print(json.dumps(result,indent=1),flush=True)

if __name__=='__main__':main()
