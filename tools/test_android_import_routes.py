"""Current import router versus an explicit sealed baseline, using real stdio."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--before',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--serial',required=True);args=parser.parse_args()
    out=args.output.resolve();out.mkdir(exist_ok=True,parents=True);before=args.before.resolve()
    current=ROOT/'android/host/win64.c';stdio=ROOT/'android/host/win64_stdio.c';string=ROOT/'android/host/win64_string.c'
    files=[current,stdio,string,ROOT/'tests/android_import_routes_fixture.c',ROOT/'tests/android_crt_hotpath_fixture.c',Path(__file__).resolve()]
    pins={str(p.relative_to(ROOT)):sha(p) for p in files}
    old=(before/'android/host/win64.c').read_text();new=current.read_text()
    def predicate(text):return text[text.index('static int is_crt('):text.index('static uint64_t crt_allocate(')]
    declarations=predicate(old).replace('is_crt(','before_is_crt(')+predicate(new).replace('is_crt(','after_is_crt(')
    fragment=old[old.index('        if (!strcmp(name,"strcmp")) {'):];fragment=fragment[:fragment.index('\n')]
    declarations+='static int primitive_execute(CPU*c,const char*dll,const char*name){uint64_t a=c->rcx,b=c->rdx;if(before_is_crt(dll)){\n'+fragment+'\n}return 0;}\n'
    for label,text in [('baseline',old),('candidate',new)]:
        router=text[text.index('int win64_dispatch_import('):text.index('int win64_dispatch_dynamic(')]
        apis=set(re.findall(r'\b(win64_[a-z0-9_]+_call)\(',router));imports=set(re.findall(r'\b(win64_[a-z0-9_]+_import)\(',router))-{'win64_dispatch_import'}
        mapping={symbol:'unrelated_api' for symbol in apis};mapping.update({symbol:'unrelated_import' for symbol in imports})
        mapping.update(win64_stdio_call='old_stdio' if label=='baseline' else 'new_stdio',win64_string_call='real_string',win64_threads_raise_exception='unrelated_notification',execute='primitive_execute',is_crt='before_is_crt' if label=='baseline' else 'after_is_crt')
        declarations+=''.join('#define '+a+' '+b+'\n' for a,b in mapping.items())
        declarations+=router.replace('int win64_dispatch_import(','static int '+label+'_route(')
        declarations+=''.join('#undef '+a+'\n' for a in mapping)
    (out/'crt_routers.inc').write_text(declarations)
    support=(ROOT/'tests/android_crt_hotpath_fixture.c').read_text();support=support[:support.index('static CPU arguments(')]
    (out/'route_support.inc').write_text(support)
    original=out/'before_stdio.c';original.write_bytes((before/'android/host/win64_stdio.c').read_bytes())
    cfg=json.loads((ROOT/'port.json').read_text());clang=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    command=[str(clang),'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',*['-I'+str(ROOT/p) for p in ['native/runtime64','android/host']],'-I'+str(out)]
    objects=[]
    for i,source in enumerate([files[3],stdio,string,original]):
        obj=out/('part%d.o'%i);objects.append(obj)
        defines=['-Dwin64_stdio_call=baseline_stdio_call','-Dwin64_stdio_self_test=baseline_stdio_self_test'] if source==original else []
        p=subprocess.run(command+defines+['-c',str(source),'-o',str(obj)],capture_output=True,text=True,timeout=60);assert p.returncode==0,p.stderr
    binary=out/'import-routes-arm64';p=subprocess.run([str(clang),'--target=aarch64-linux-android30',*map(str,objects),'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-lm','-pthread','-o',str(binary)],capture_output=True,text=True,timeout=60);assert p.returncode==0,p.stderr
    adb=['adb','-s',args.serial];remote='/data/local/tmp/frostpunk-import-routes-'+sha(binary)[:12]
    for tail in [['push',str(binary),remote],['shell','chmod','700',remote]]:subprocess.run(adb+tail,capture_output=True,check=True,timeout=30)
    p=subprocess.run(adb+['shell',remote],capture_output=True,text=True,timeout=60);assert p.returncode==0,(p.stdout,p.stderr)
    row=json.loads(p.stdout);assert row['success'] and row['paired_calls']==4160 and row['classifications']>40000
    negatives=[]
    for variant in ['baseline','candidate']:
        p=subprocess.run(adb+['shell',remote,'--fault',variant],capture_output=True,text=True,timeout=30)
        assert p.returncode==73 and 'registrations=4 RSP_unpopped=1' in p.stderr,(p.returncode,p.stdout,p.stderr)
        negatives.append(p.stderr)
    assert negatives[0]==negatives[1]
    assert pins=={str(p.relative_to(ROOT)):sha(p) for p in files}
    result=dict(success=True,device_executed=True,serial=args.serial,sources=pins,device=row,negative=negatives,binary_sha256=sha(binary),baseline_sources={str(p.relative_to(ROOT)):sha(p) for p in [before/'android/host/win64.c',before/'android/host/win64_stdio.c']})
    (out/'qualification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(row,indent=2))
if __name__=='__main__':main()
