"""Unit-divisor fmodf UCRT differential, full CPU/FP fixture and ARM CLI. No ADB."""
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT=ROOT/'build/android-fmodf-test'
ROW=struct.Struct('<IIIIIiII')


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def expected_unit(x,csr):
    """Independent exact binary64 subtraction; each binary32 fraction is exact."""
    magnitude=x&0x7fffffff;sign=x&0x80000000;flags=0
    if 0<magnitude<0x800000:
        if csr&64:return x,0
        if csr&32768:return sign,50
        return x,2
    value=struct.unpack('<f',struct.pack('<I',x))[0]
    fraction=value-math.trunc(value)
    result=struct.unpack('<I',struct.pack('<f',fraction))[0]
    if not result&0x7fffffff:
        result=sign
        if magnitude>0x3f800000 and (csr>>13)&3==1:result^=0x80000000
    return result,flags


def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();cfg=json.loads((ROOT/'port.json').read_text())
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files=['android/host/win64_math.c','android/host/win64_math.h','native/runtime64/cpu64.h',
           'tests/win64_fmodf_oracle.c','tests/win64_fmodf_fixture.c','tools/test_android_fmodf.py']
    before={name:sha(ROOT/name) for name in files};commands=[]

    def run(args,expected=0):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if result.returncode!=expected:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+result.stdout[-7000:]+result.stderr[-7000:])
        return result

    import pefile
    pe_path=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(pe_path)!=cfg['original_exe_sha256']:raise ValueError('Original PE changed')
    pe=pefile.PE(str(pe_path),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
    gates={0x141AE1400:'4883ec48f30f7f442420f30f7f4c2430e8851e0000660f7e442420660f6e442424660f6e4c2434e86e1e0000660f7e442424660f6e442428660f6e4c2438e8571e0000660f7e442428660f6e44242c660f6e4c243ce8401e0000660f7e44242cf30f6f4424204883c448c3',
           0x141AE329A:'ff25c04a1e00',0x140ED8E45:'0f280d84c63101e8af85c000',0x140ED8ECB:'0f280dfec53101',
           0x140ED8EDF:'e81c85c000',0x1421F54D0:'0000803f0000803f0000803f0000803f'}
    for va,expected in gates.items():
        actual=pe.get_data(va-base,len(bytes.fromhex(expected)))
        if actual.hex()!=expected:raise ValueError('Original fmodf caller gate drift '+hex(va)+': '+actual.hex())
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('CRT import route changed')
    (OUT/'fmodf_route.inc').write_text(route+'\n')
    rng=random.Random(0x141AE1400);xs={0,0x80000000}
    for exponent in range(255):
        for mantissa in [0,1,2,0x3fffff,0x400000,0x7ffffe,0x7fffff]:
            for sign in [0,0x80000000]:xs.add(sign|(exponent<<23)|mantissa)
    for _ in range(8192):
        x=rng.getrandbits(32)
        if x&0x7f800000!=0x7f800000:xs.add(x)
    inputs=OUT/'inputs.bin';input_count=0
    with inputs.open('wb') as stream:
        for index,x in enumerate(sorted(xs)):
            for mode in range(16):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                stream.write(struct.pack('<IIII',x,0x3f800000,csr,index));input_count+=1
        for mask in range(64):
            for mode in range(16):
                for x in [0,0x80000000,0x3f800000,0x40400000,0xc0b00000,0x7f7fffff]:
                    if x&0x7fffffff>0x3f800000 and not mask&32:continue
                    csr=(mask<<7)|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if mask&1 else 0)
                    stream.write(struct.pack('<IIII',x,0x3f800000,csr,input_count));input_count+=1
    oracle=OUT/'fmodf-oracle.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_fmodf_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    dataset=OUT/'cases.bin';oracle_result=json.loads(run([oracle,inputs,dataset]).stdout)
    for x,y,result,csr,expected_csr,expected_errno,_,_ in ROW.iter_unpack(dataset.read_bytes()):
        reference,flags=expected_unit(x,csr)
        if result!=reference or expected_csr!=csr|flags or expected_errno!=1379:
            raise ValueError('UCRT unit remainder differs from independent subtraction/reference policy: '+repr((hex(x),hex(csr),hex(result),hex(reference),expected_csr,flags)))
    common=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT)]
    host=OUT/'fmodf-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',*[x.replace('-I','/I',1) for x in common],
         ROOT/'tests/win64_fmodf_fixture.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    result=run([host,dataset]);host_result=json.loads(result.stdout);(OUT/'host.log').write_text(result.stdout+result.stderr)
    negatives=[]
    for name in ['zero_divisor','negative_divisor','other_divisor','NaN_divisor','reserved','NaN','sNaN','infinity',
                 'unmasked_denormal','unmasked_underflow','unmasked_precision','unmasked_intermediate']:
        result=run([host,'--negative',name],73)
        if 'MATH_INPUT name=fmodf x=0x' not in result.stderr or 'y=0x' not in result.stderr:raise ValueError('Missing typed rejection bits')
        negatives.append({'case':name,'exit_code':73,'stderr':result.stderr})
    binary=OUT/'fmodf-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing',
         '-ffunction-sections','-fdata-sections',*common,ROOT/'tests/win64_fmodf_fixture.c',ROOT/'android/host/win64_math.c',
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    if before!={name:sha(ROOT/name) for name in files}:raise ValueError('Fmodf sources changed during fixture')
    report={'success':True,'phase':'host-UCRT-exact-arm-build','sources':before,'commands':commands,
            'original':{'sha256':cfg['original_exe_sha256'],'byte_gates':{hex(va):raw for va,raw in gates.items()},
                        'callers':['0x140ed8e4c','0x140ed8edf'],'wrapper':'0x141ae1400','divisor_all_lanes':'0x3f800000'},
            'oracle':oracle_result,'host':host_result,'negative_cases':negatives,
            'dataset':{'path':str(dataset.relative_to(ROOT)),'sha256':sha(dataset),'row_bytes':32,'count':input_count},
            'binary':{'path':str(binary.relative_to(ROOT)),'sha256':sha(binary),'args':['cases.bin']},
            'device_executed':False,'scope':'all finite binary32 dividends modulo+1, exact UCRT result/status/errno, DAZ/FTZ and downward-zero quirks; otherdivisors/NaN/Inf/unmasked-raisedexceptions rejected'}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'host':host_result,'binary':report['binary'],'dataset':report['dataset']},indent=2))


if __name__=='__main__':main()
