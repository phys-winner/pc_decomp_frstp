"""Validate CFG barriers and compare original/optimized real instruction blocks on ARM64."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from fastbuild import ROOT, CLANG, COMPILE_FLAGS
from leaf_probe import windows_toolchain
from optimize_live_flags64 import body, CALL, PURE


def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def generate():
    programs=[
        'SET64(c->rax,flags_add(c,c->rcx,c->rdx,8));\nflags_sub(c,c->rbx,c->rsi,8);\nreturn;\n',
        'flags_add(c,c->rcx,c->rdx,8);\nif(c->zf) goto A;\nflags_sub(c,c->rbx,1,8);\ngoto B;\nA:\nflags_sub(c,c->rsi,2,8);\nB:\nreturn;\n',
        'flags_add(c,c->rcx,c->rdx,8);\nflags_logicz(c,c->rbx,8);\nreturn;\n',
        'flags_sub(c,0,1,8);\ngoto Test;\nLoop:\nSET64(c->rdi,c->rdi+1);\nflags_add(c,0,1,8);\nTest:\nif(c->cf) goto Loop;\nflags_sub(c,c->rbx,1,8);\nreturn;\n',
    ]
    assert '2u)' in body(programs[1])[0] and '32u)' in body(programs[2])[0]
    for barrier in ['wr64((uint64_t)&c->cf,1);','c->rax=rd64((uint64_t)&c->pf);',
                    'dispatch(c,c->rcx);','goto *unknown;','ES3_EH_LEAVE();',
                    'while(c->zf) { c->zf=0; }']:
        text='flags_add(c,c->rcx,c->rdx,8);\n'+barrier+'\nflags_sub(c,c->rbx,1,8);\nreturn;\n'
        assert body(text)[0].splitlines()[0]==text.splitlines()[0],barrier
    structured='while(c->zf) {\nflags_add(c,c->rcx,c->rdx,8);\nflags_sub(c,c->rbx,1,8);\n}\nreturn;\n'
    assert body(structured)[0]==structured
    assert body(structured.replace('while(c->zf)', 'for(;c->zf;)'))[0]==structured.replace('while(c->zf)', 'for(;c->zf;)')
    conditional='flags_add(c,c->rcx,c->rdx,8);\nif(c->rsi) flags_sub(c,c->rbx,1,8);\nreturn;\n'
    assert body(conditional)[0]==conditional
    # Straight-line regions from actual hot units, terminated before any observation barrier.
    for unit in ('0812','0820','0822','0831'):
        path=ROOT/('build/arm64/generated-overlay/recomp_funcs_'+unit+'.c')
        if not path.exists():path=ROOT/('generated/frostpunk-x64/recomp_funcs_'+unit+'.c')
        block=[]
        for line in path.read_text().splitlines():
            code=line.split('/*',1)[0].strip()
            pure=bool(re.match(r'^(?:SET\w*\(|flags_(?:add|sub|logicz)\(|c->\w+\s*=)',code)) and not set(CALL.findall(code))-PURE
            pure=pure and code.endswith(';') and not any(char in code for char in '{}') and not re.search(r'\b_\w+',code)
            if pure:block.append(code+'\n');continue
            if block:
                program=''.join(block)+'return;\n'
                if body(program)[1]:programs.append(program)
                block=[]
            if len(programs)>=403:break
        if len(programs)>=403:break
    assert len(programs)>100
    source=['#include "live_flags64.h"\n#include <stdlib.h>\n']
    for i,program in enumerate(programs):
        source.append('static void original_%d(CPU*c){\n%s}\n'%(i,program))
        source.append('static void candidate_%d(CPU*c){\n%s}\n'%(i,body(program)[0]))
    source.append('unsigned test_generated_blocks(void){\nunsigned cases=0;\n')
    for i in range(len(programs)):
        source.append('for(unsigned n=0;n<128;++n){CPU a;for(unsigned j=0;j<sizeof(a);++j)((unsigned char*)&a)[j]=(unsigned char)(n*37+j*11);'
                      'a.cf=n&1;a.zf=(n>>1)&1;a.sf=(n>>2)&1;a.of=(n>>3)&1;a.pf=(n>>4)&1;a.af=(n>>5)&1;'
                      'if(n<8){a.rcx=n;a.rdx=(uint64_t)-(int64_t)n;}CPU b=a;original_%d(&a);candidate_%d(&b);'
                      'if(memcmp(&a,&b,sizeof(a)))abort();++cases;}\n'%(i,i))
    source.append('return cases;}\n')
    return ''.join(source),len(programs)


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--serial',required=True);args=ap.parse_args()
    out=ROOT/'build/perf/native-cpu-20261113/flags-proof';out.mkdir(parents=True,exist_ok=True)
    generated,count=generate();copy=out/'blocks.c';copy.write_text(generated,encoding='utf-8',newline='\n')
    sources=[ROOT/'tests/live_flags64_fixture.c',copy]
    pins={str(p):sha(p) for p in [*sources,ROOT/'native/runtime64/live_flags64.h',ROOT/'tools/optimize_live_flags64.py',Path(__file__)]}
    bins,env=windows_toolchain()
    def run(command):
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True,env=env,timeout=180)
        if r.returncode:raise RuntimeError(r.stdout[-1000:]+r.stderr[-4000:])
        return r.stdout
    flags=['-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-I'+str(ROOT/'native/runtime64')]
    objects=[]
    for i,p in enumerate(sources):
        obj=out/('host-%d.obj'%i);run([CLANG,'--target=x86_64-pc-windows-msvc',*flags,'-fms-runtime-lib=static','-c',p,'-o',obj]);objects.append(obj)
    host=out/'flags-host.exe';run([bins/'link.exe','/nologo','/OUT:'+str(host),*objects,'libcmt.lib','libvcruntime.lib','libucrt.lib','oldnames.lib','kernel32.lib'])
    host_result=json.loads(run([host]));assert host_result['success']
    arm=out/'flags-arm64';run([CLANG,*COMPILE_FLAGS,'-I'+str(ROOT/'native/runtime64'),*sources,'-Wl,-z,max-page-size=16384','-lm','-o',arm])
    adb=['adb','-s',args.serial];remote='/data/local/tmp/frostpunk-flags-'+sha(arm)[:12]
    run([*adb,'push',arm,remote]);run([*adb,'shell','chmod','700',remote])
    try:device=json.loads(run([*adb,'shell',remote]))
    finally:run([*adb,'shell','rm','-f',remote])
    assert device==host_result and pins=={p:sha(p) for p in pins}
    result=dict(success=True,host=host_result,device=device,programs=count,sources=pins,binary_sha256=sha(arm))
    (out/'result.json').write_text(json.dumps(result,indent=1)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='sources'}))


if __name__=='__main__':main()
