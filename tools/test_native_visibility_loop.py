"""Compare complete original/new ARM visibility queries, including backedges and CPU aliases."""
import argparse,hashlib,json,statistics,subprocess
from pathlib import Path
import pefile
from elftools.elf.elffile import ELFFile
import fastbuild as fb
from build_mobile_quality import detail_object

ROOT=fb.ROOT
OUT=ROOT/'build/test-native-visibility-loop'
ORIGINAL=ROOT/'build/perf/native-cpu-20261113/compile-cache/obj/recomp_funcs_0822-17c74f17f12fefdfa529.o'
ORIGINAL_SHA='3ad58044314b7b96085727118bb72fc59bd97d9eb1411265d047ea8274543225'

def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(command):
    result=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=180)
    if result.returncode:raise RuntimeError(result.stdout[-2000:]+result.stderr[-4000:])
    return result.stdout

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--serial',required=True);args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True);assert sha(ORIGINAL)==ORIGINAL_SHA
    _,replacement,routing=detail_object(OUT);candidate=ROOT/replacement
    exe=Path(fb.CONFIG['game_directory'])/'Frostpunk.exe';assert sha(exe)==fb.CONFIG['original_exe_sha256']
    with pefile.PE(str(exe),fast_load=True) as pe:
        values=[(a-0x140000000,pe.get_data(a-0x140000000,4)) for a in [0x141d5a300,0x1421f52b8,0x141d83e94]]
    assert all(len(v)==4 for a,v in values)
    constants=OUT/'constants.h'
    constants.write_text('static const struct {unsigned offset;unsigned char value[4];} constants[]={'+','.join('{'+str(a)+',{'+','.join(str(b) for b in value)+'}}' for a,value in values)+'};\n')
    undefined=[];prefixed=[]
    for prefix,path in [('old_',ORIGINAL),('new_',candidate)]:
        target=OUT/(prefix+'whole.o');run([fb.BIN/'llvm-objcopy.exe','--prefix-symbols='+prefix,path,target]);prefixed.append(target)
        with path.open('rb') as a,target.open('rb') as b:
            ea,eb=ELFFile(a),ELFFile(b);assert ea.get_section_by_name('.text').data()==eb.get_section_by_name('.text').data()
            undefined.extend(s.name for s in eb.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']=='SHN_UNDEF' and s.name)
    allowed={prefix+name for prefix in ['old_','new_'] for name in ['g_image_delta','fp_dispatch_main','dispatch','fp_test_detail_mode','fp_test_detail_threshold']}
    stubs=OUT/'unreached.S';stubs.write_text('.text\n'+''.join('.p2align 2\n.global '+s+'\n.type '+s+',%function\n'+s+':\n brk #0\n' for s in sorted(set(undefined)-allowed))+'\n.section .note.GNU-stack,"",%progbits\n')
    flags=[*fb.COMPILE_FLAGS,*['-I'+str(ROOT/d) for d in fb.INCLUDES],'-I'+str(OUT)]
    fixture=ROOT/'tests/visibility_loop_fixture.c';objects=[]
    for source in [fixture,stubs]:
        obj=OUT/(source.stem+'.o');run([fb.CLANG,*flags,'-c',source,'-o',obj]);objects.append(obj)
    binary=OUT/'visibility-loop-arm64';run([fb.CLANG,'--target=aarch64-linux-android30',*objects,*prefixed,'-Wl,-z,max-page-size=16384','-lm','-o',binary])
    adb=['adb','-s',args.serial];assert run([*adb,'shell','getprop ro.serialno']).strip()==args.serial
    assert not run([*adb,'shell','pidof com.frostpunk.staticrecomp || true']).strip()
    remote='/data/local/tmp/frostpunk-visibility-loop-'+sha(binary)[:16]
    try:
        run([*adb,'push',binary,remote]);run([*adb,'shell','chmod','700',remote]);test=json.loads(run([*adb,'shell',remote]))
        assert test['success'] and test['queries']==2868 and test['full_cpu_memory_callbacks_fp_errno']
        bench=json.loads(run([*adb,'shell',remote,'--benchmark']));assert bench['success']
    finally:run([*adb,'shell','rm','-f',remote])
    paths=[Path(__file__),fixture,ORIGINAL,candidate,constants,stubs,ROOT/'tools/build_mobile_quality.py',ROOT/'tools/native_visibility_overlay.py',ROOT/'tools/mobile_detail_overlay.py',ROOT/'tools/optimize_live_flags64.py',ROOT/'android/host/visibility_loop_native.h',ROOT/'android/host/visibility_filter_native.h',ROOT/'native/runtime64/cpu64.h',ROOT/'native/runtime64/live_flags64.h',ROOT/'native/runtime64/sse_scalar.h',ROOT/'build/arm64/generated-overlay/recomp_funcs_0822.c']
    medians={str(k):{name:statistics.median(p[name] for p in bench['pairs'] if p['kind']==k) for name in ['original_ns','native_ns']} for k in range(7)}
    result=dict(success=True,device=test,benchmark=bench,medians=medians,sources={str(p):sha(p) for p in paths},original_object_sha256=sha(ORIGINAL),native_object_sha256=sha(candidate),whole_original_and_candidate_text_unchanged=True,routing=routing,binary_sha256=sha(binary),header_digest=fb.header_digest())
    (OUT/'result.json').write_text(json.dumps(result,indent=1)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ['benchmark','sources']},indent=1),flush=True)

if __name__=='__main__':main()
