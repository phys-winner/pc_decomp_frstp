"""Native UCRT tiny tanf contract vs exact production provider; no ADB."""
import difflib
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/audit-20261108/tiny-tanf'
INPUT=struct.Struct('<II');OUTPUT=struct.Struct('<IIiII');FIXTURE=struct.Struct('<QIIiIQIiII')
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    OUT.mkdir(parents=True,exist_ok=True);cfg=json.loads((ROOT/'port.json').read_text());bins,env=windows_toolchain();commands=[]
    sources=['android/host/win64_math.c','android/host/win64_math.h','android/host/win64_stdio.h',
             'native/runtime64/cpu64.h','tests/win64_trig_adapter.c','tests/tiny_tanf_oracle.c',
             'tools/test_android_tiny_tanf.py']
    pins={p:sha(ROOT/p) for p in sources}
    core=(ROOT/'android/host/win64.c').read_text()
    assert core.count('    if (crt && win64_math_call(c,name)) IMPORT_DONE;')==1
    assert '#define IMPORT_DONE do { *handled=step; c->rsp+=8; return 1; } while (0)' in core
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if result.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps(dict(command=command,returncode=result.returncode,expected=expected,stdout=result.stdout,stderr=result.stderr),indent=2)+'\n')
            raise RuntimeError(result.stdout[-7000:]+result.stderr[-7000:])
        return result
    oracle=OUT/'oracle.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/tiny_tanf_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    magnitudes=set(range(257))
    for boundary in [0x00800000,0x32000000,0x39000000]:
        magnitudes.update(range(max(0,boundary-1024),boundary+1025))
    magnitudes.update(range(0x371a25c1-128,0x371a25c1+129))
    for exponent in range(1,115):
        magnitudes.update((exponent<<23)|fraction for fraction in [0,1,2,0x3fffff,0x400000,0x7ffffe,0x7fffff])
    rng=random.Random(0x371a25c1);magnitudes.update(rng.randrange(1,0x39000000) for _ in range(4096))
    values=sorted(x|sign for x in magnitudes for sign in [0,0x80000000])
    rows=[(x,0x1f80|(rc<<13)|(daz<<6)|(ftz<<15)|sticky) for x in values for rc in range(4) for daz in range(2) for ftz in range(2) for sticky in [0,0x26]]
    inputs=OUT/'input.bin';outputs=OUT/'native.bin';fixture=OUT/'cases.bin'
    inputs.write_bytes(b''.join(INPUT.pack(*r) for r in rows));native=json.loads(run([oracle,inputs,outputs]).stdout)
    data=outputs.read_bytes();assert native['success'] and native['cases']==len(rows) and len(data)==len(rows)*OUTPUT.size
    exact=0;boundary=0;statuses={};observed=[]
    with fixture.open('wb') as stream:
        for i,(bits,csr) in enumerate(rows):
            result,actual,error,last_error,calls=OUTPUT.unpack_from(data,i*OUTPUT.size)
            magnitude=bits&0x7fffffff;flags=0;predicted=bits
            if magnitude<0x39000000:
                if magnitude and (not(csr&64) or magnitude>=0x800000):
                    flags=32
                    if magnitude<0x800000:flags|=18
                    elif magnitude==0x800000:
                        flags|=16;rc=(csr>>13)&3
                        smaller=rc==3 or (rc==1 and not(bits>>31)) or (rc==2 and bits>>31)
                        if smaller and not(csr&(64|32768)):flags|=2
                if magnitude>=0x32000000 and ((csr>>13)&3)==(1 if bits>>31 else 2):predicted+=1
                assert (result,actual,error,last_error,calls)==(predicted,csr|flags,1379,0x5a17,0),(hex(bits),hex(csr),hex(result),hex(actual),hex(predicted),flags,error,last_error,calls)
                exact+=1;limit=0
            else:
                assert error==1379 and last_error==0x5a17 and not calls
                boundary+=1;limit=4
            statuses[str(actual&63)]=statuses.get(str(actual&63),0)+1
            if bits==0x371a25c1 and csr==0x1fa6:observed.append(dict(bits=hex(bits),csr=hex(csr),result=hex(result),result_csr=hex(actual)))
            stream.write(FIXTURE.pack(bits,2,csr,1379,0,result,actual,error,limit,i&1))
    assert observed and FIXTURE.size==48
    route='''static int trig_import(CPU*c,const char*dll,const char*name){
    if(strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll"))return 0;
    if(!win64_math_call(c,name))return 0;
    c->rsp+=8;return 1;
}
'''
    (OUT/'trig_route.inc').write_text(route)
    include=[ROOT/'native/runtime64',ROOT/'android/host',OUT];host=OUT/'tiny-tanf-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/experimental:c11atomics',*['/I'+str(p) for p in include],ROOT/'tests/win64_trig_adapter.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    host_run=run([host,fixture]);host_result=json.loads(host_run.stdout.strip().splitlines()[-1]);assert host_result['success']
    (OUT/'host.log').write_text(host_run.stdout+host_run.stderr)
    negative=[]
    for name in ['matherr_tanf','tanf_unmasked_invalid','tanf_unmasked_precision','tanf_tiny_precision','tanf_min_normal_underflow','tanf_subnormal_denormal']:
        result=run([host,'--negative',name],73);assert 'STOP Win64 math' in result.stderr;negative.append(dict(arg=name,exit_code=73,stderr=result.stderr))
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';arm=OUT/'tiny-tanf-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',*['-I'+str(p) for p in include],ROOT/'tests/win64_trig_adapter.c',ROOT/'android/host/win64_math.c','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',arm])
    assert pins=={p:sha(ROOT/p) for p in sources},'Math source changed during proof'
    before=(OUT/'win64_math.before.c').read_text();after=(ROOT/'android/host/win64_math.c').read_text()
    (OUT/'math-change.diff').write_text(''.join(difflib.unified_diff(before.splitlines(keepends=True),after.splitlines(keepends=True),fromfile='before',tofile='after')))
    report=dict(success=True,sources=pins,native_UCRT=native,exact_tiny_cases=exact,unchanged_boundary_cases=boundary,
                host=host_result,negative_cases=negative,observed_surface_input=observed,status_distribution=statuses,
                native_ucrt_sha256=sha('C:/Windows/System32/ucrtbase.dll'),dataset=dict(path=str(fixture.relative_to(ROOT)),sha256=sha(fixture),bytes=fixture.stat().st_size),
                binary=dict(path=str(arm.relative_to(ROOT)),sha256=sha(arm),bytes=arm.stat().st_size,args=['cases.bin']),
                commands=commands,scope='Exact measured UCRT bit rule for every exponent of finite |x|<2^-13 including normal/subnormal/zero, both signs and RC4/DAZ2/FTZ2, initial sticky flags0/0x26. Branch boundaries and nearby >=2^-13 use unchanged existing measured ULP4 profile. Native UCRT errno/LastError/matherr and exact guest CSR validated; full production CPU/nativeFP/hosterrno/guesterrno/importpop fixture. No Bionic approximation in the added tiny path.',
                original_game_code_executed=False,device_actions=False,arm_executed=False)
    (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(success=True,exact_tiny_cases=exact,boundary_cases=boundary,host=host_result,arm=report['binary'],receipt_sha256=sha(OUT/'result.json')),indent=2))
if __name__=='__main__':main()
