"""Measure/test exact import routers with real stdio/string; no device actions."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
from leaf_probe import ROOT, windows_toolchain

OUT=ROOT/'build/android-crt-hotpath-test'
DECLINE='''    /* These exact CRT names are declined after the required first-call FILE
     * registration, without walking unrelated stream/format APIs. */
    if(!strcmp(name,"memcpy")||!strcmp(name,"memmove")||!strcmp(name,"strcat_s")||
       !strcmp(name,"strcpy_s")||!strcmp(name,"strncmp"))return 0;
'''
OLD='''    if (crt && (!strcmp(name,"memcpy") || !strcmp(name,"memmove"))) {
        /* Preserve stdio's first-call FILE registration even when it declines
         * this name. Other earlier providers reject these CRT copies purely. */
        if (!win64_stdio_call(c,name)) {
            uint64_t a=c->rcx,b=c->rdx,d=c->r8;
            memmove(win64_pointer(a,(size_t)d,1),win64_pointer(b,(size_t)d,0),(size_t)d); c->rax=a;
        }
        c->rsp+=8;return 1;
    }
'''
NEW_PREFIX='    size_t prefix=append?strnlen((const char*)out,capacity):0;'
OLD_PREFIX='    size_t prefix=append?bounded_length(destination,capacity,width):0;'


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    core=ROOT/'android/host/win64.c';stdio=ROOT/'android/host/win64_stdio.c';string=ROOT/'android/host/win64_string.c'
    fixture=ROOT/'tests/android_crt_hotpath_fixture.c'
    pins=[core,stdio,string,fixture,Path(__file__).resolve(),ROOT/'android/host/win64.h',
          ROOT/'android/host/win64_stdio.h',ROOT/'android/host/win64_string.h']+sorted((ROOT/'native/runtime64').glob('*.h'))
    before={str(p.relative_to(ROOT)):sha(p) for p in pins}
    text=core.read_text();router=text[text.index('int win64_dispatch_import('):text.index('int win64_dispatch_dynamic(')]
    start=router.index('    if (crt &&');end=router.index('    const int kernel=')
    current=router[start:end]
    for name in ('memcpy','memmove','strcat_s','strcpy_s','strncmp'):
        if '"'+name+'"' not in current:raise ValueError('Hot route source drift: '+name)
    baseline=router[:start]+OLD+router[end:]
    is_crt=text[text.index('static int is_crt('):text.index('static uint64_t crt_allocate(')]
    declarations='''int baseline_string_call(CPU*,const char*);
static int old_string(CPU*c,const char*n){++string_calls;return baseline_string_call(c,n);}
'''+is_crt
    for variant,body in [('baseline',baseline),('candidate',router)]:
        api=set(re.findall(r'\b(win64_[a-z0-9_]+_call)\(',body))
        imports=set(re.findall(r'\b(win64_[a-z0-9_]+_import)\(',body))-{'win64_dispatch_import'}
        declarations+='\n#define win64_stdio_call '+('old_stdio' if variant=='baseline' else 'new_stdio')+'\n#define win64_string_call '+('old_string' if variant=='baseline' else 'real_string')+'\n'
        for symbol in sorted(api-{'win64_stdio_call','win64_string_call'}):declarations+='#define '+symbol+' unrelated_api\n'
        for symbol in sorted(imports):declarations+='#define '+symbol+' unrelated_import\n'
        declarations+='#define win64_threads_raise_exception unrelated_notification\n#define execute unrelated_import\n'
        declarations+=body.replace('int win64_dispatch_import(', 'static int '+variant+'_route(')
        for symbol in sorted(api|imports|{'win64_threads_raise_exception','execute'}):declarations+='#undef '+symbol+'\n'
    routers=OUT/'crt_routers.inc';routers.write_text(declarations,newline='\n')
    raw=stdio.read_text()
    if raw.count(DECLINE)!=1:raise ValueError('Expected exact stdio decline guard')
    original_stdio=OUT/'baseline_stdio.c';original_stdio.write_text(raw.replace(DECLINE,''),newline='\n')
    string_text=string.read_text()
    if string_text.count(NEW_PREFIX)!=1:raise ValueError('Expected exact validated-destination prefix line')
    original_string=OUT/'baseline_string.c';original_string.write_text(string_text.replace(NEW_PREFIX,OLD_PREFIX),newline='\n')
    cfg=json.loads((ROOT/'port.json').read_text());llvm=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler=llvm/'clang.exe';bins,env=windows_toolchain();commands=[]
    def run(command,expected=0):
        commands.append(list(map(str,command)));p=subprocess.run(commands[-1],env=env,capture_output=True,text=True,timeout=30)
        if p.returncode!=expected:raise RuntimeError(subprocess.list2cmdline(commands[-1])+'\n'+p.stdout+p.stderr)
        return p
    include=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT)]
    sources=[fixture,stdio,string,original_stdio,original_string];objects=[]
    def definitions(source):
        if source==original_stdio:return ['-Dwin64_stdio_call=baseline_stdio_call','-Dwin64_stdio_self_test=baseline_stdio_self_test']
        if source==original_string:return ['-Dwin64_string_call=baseline_string_call','-Dwin64_string_self_test=baseline_string_self_test']
        return []
    for source in sources:
        obj=OUT/(source.stem+'.obj');objects.append(obj)
        defs=definitions(source)
        run([compiler,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-D_CRT_SECURE_NO_WARNINGS','-Wno-deprecated-declarations',*include,*defs,'-c',source,'-o',obj])
    binary=OUT/'crt-hotpath-host.exe';run([bins/'link.exe','/nologo','/OUT:'+str(binary),*objects,'libcmt.lib','oldnames.lib','kernel32.lib'])
    host=json.loads(run([binary]).stdout);cold=[];negative=[]
    for name in ('strcat_s','strcpy_s','strncmp','memcpy','memmove'):cold.append(json.loads(run([binary,'--cold',name]).stdout))
    for name in ('strcat_s','strcpy_s','strncmp'):
        for variant in ('baseline','candidate'):
            p=run([binary,'--negative',variant,name],73)
            if 'CRT_HOTPATH_POINTER_REJECT' not in p.stderr and 'STOP Win64 string' not in p.stderr:raise ValueError('Wrong negative boundary')
            if 'CRT_HOTPATH_POINTER_REJECT' in p.stderr and 'registrations=4 RSP_unpopped=1' not in p.stderr:raise ValueError('Fault bypassed first FILE registration or popped RSP')
            negative.append(dict(variant=variant,name=name,exit_code=73,stderr=p.stderr))
        if negative[-2]['stderr']!=negative[-1]['stderr']:raise ValueError('Baseline/candidate first fault differs: '+name)
    arm_objects=[]
    for source in sources:
        obj=OUT/(source.stem+'.arm.o');arm_objects.append(obj)
        defs=definitions(source)
        run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing',*include,*defs,'-c',source,'-o',obj])
    arm=OUT/'crt-hotpath-arm64';run([compiler,'--target=aarch64-linux-android30',*arm_objects,'-lm','-pthread','-Wl,-z,max-page-size=16384','-o',arm])
    if before!={str(p.relative_to(ROOT)):sha(p) for p in pins}:raise ValueError('Source drift during hotpath fixture')
    result=dict(success=True,host=host,cold_first_calls=cold,negative=negative,
        arm=dict(compiled=True,executed=False,binary=str(arm.relative_to(ROOT)),binary_sha256=sha(arm)),
        source_hashes=before,routers_sha256=sha(routers),baseline_stdio_sha256=sha(original_stdio),baseline_string_sha256=sha(original_string),
        prior_byte_prefix_differential=True,
        host_binary_sha256=sha(binary),compiler_sha256=sha(compiler),game_executed=False,commands=commands)
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(success=True,host=host,cold_cases=len(cold),negative_cases=len(negative),arm=result['arm'])))


if __name__=='__main__':main()
