"""Exact packed-codec ldexp UCRT oracle and portable ARM CLI; never uses ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT=ROOT/'build/android-ldexp-test'
ROW=struct.Struct('<QQQIIiI')


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();cfg=json.loads((ROOT/'port.json').read_text())
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files=['android/host/win64_math.c','android/host/win64_math.h','native/runtime64/cpu64.h',
           'tests/win64_ldexp_oracle.c','tests/win64_ldexp_fixture.c','tools/test_android_ldexp.py']
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
    gates={0x141C56274:'25ffff1f00',0x141C56288:'c1fa15',0x141C5628B:'81e2ff030000',
           0x141C56296:'660f6ec0f30fe6c0',0x141C562A5:'81c2ecfcffff',0x141C562AB:'ff15f71a0700',
           0x141C562DC:'81c2ecfcffff',0x141C562E2:'ff15c01a0700',0x1421F5560:'0000000000000080'}
    for va,expected in gates.items():
        actual=pe.get_data(va-base,len(bytes.fromhex(expected)))
        if actual.hex()!=expected:raise ValueError('Original ldexp caller gate drift '+hex(va)+': '+actual.hex())
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('CRT import route changed')
    (OUT/'ldexp_route.inc').write_text(route+'\n')
    rng=random.Random(0x141C562B1);inputs=OUT/'inputs.bin';input_count=0
    mantissas=[0,1,2,3,7,8,15,16,31,32,1023,1024,1048575,1048576,2097150,2097151]
    with inputs.open('wb') as stream:
        def case(mantissa,exponent,sign,csr):
            nonlocal input_count
            word=mantissa|(exponent<<21)|(sign<<31)
            stream.write(struct.pack('<IIQ',word,csr,rng.getrandbits(32)<<32));input_count+=1
        for exponent in range(1024):
            for mantissa in mantissas:
                for sign in (0,1):
                    for rc in range(4):case(mantissa,exponent,sign,0x1f80|(rc<<13)|(0x15 if exponent&1 else 0))
        for exponent in [0,1,767,768,787,788,789,1022,1023]:
            for mantissa in mantissas:
                for sign in (0,1):
                    for mode in range(16):case(mantissa,exponent,sign,0x1f95|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0))
        for mask in range(64):
            for mode in range(16):
                for mantissa in [0,1,3,2097151]:
                    for sign in (0,1):case(mantissa,[0,788,1023][mask%3],sign,(mask<<7)|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if mask&1 else 0))
        for _ in range(2048):
            mode=rng.randrange(16);case(rng.randrange(2097152),rng.randrange(1024),rng.randrange(2),0x1f95|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0))
    oracle=OUT/'ldexp-oracle.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_ldexp_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    dataset=OUT/'cases.bin';oracle_result=json.loads(run([oracle,inputs,dataset]).stdout)
    for x,result,rdx,csr,expected_csr,expected_errno,word in ROW.iter_unpack(dataset.read_bytes()):
        if expected_csr!=csr or expected_errno!=1379:raise ValueError('UCRT exact scaling status differs')
        mantissa=word&0x1fffff;scale=((word>>21)&1023)-788
        if not mantissa:expected=(word>>31)<<63
        else:
            shift=mantissa.bit_length()-1
            expected=((word>>31)<<63)|((1023+shift+scale)<<52)|((mantissa-(1<<shift))<<(52-shift))
        if result!=expected or (rdx&0xffffffff)!=(scale&0xffffffff):raise ValueError('UCRT codec scale disagrees with exact integer normalization')
    common=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT)]
    host=OUT/'ldexp-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',*[x.replace('-I','/I',1) for x in common],
         ROOT/'tests/win64_ldexp_fixture.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    result=run([host,dataset]);host_result=json.loads(result.stdout);(OUT/'host.log').write_text(result.stdout+result.stderr)
    negatives=[]
    for name in ['scale_low','scale_high','reserved','fraction','subnormal','NaN','infinity','large']:
        result=run([host,'--negative',name],73)
        if 'MATH_INPUT name=ldexp x=0x' not in result.stderr or 'exponent=' not in result.stderr:raise ValueError('Missing typed rejection bits')
        negatives.append({'case':name,'exit_code':73,'stderr':result.stderr})
    binary=OUT/'ldexp-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing',
         '-ffunction-sections','-fdata-sections',*common,ROOT/'tests/win64_ldexp_fixture.c',ROOT/'android/host/win64_math.c',
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    if before!={name:sha(ROOT/name) for name in files}:raise ValueError('Ldexp sources changed during fixture')
    report={'success':True,'phase':'host-UCRT-exact-arm-build','sources':before,'commands':commands,
            'original':{'sha256':cfg['original_exe_sha256'],'byte_gates':{hex(va):raw for va,raw in gates.items()},
                        'mantissa_bits':21,'packed_exponent_bits':10,'exponent_bias':-788,'scale_range':[-788,235],
                        'nonzero_result_unbiased_exponent_range':[-788,255]},
            'oracle':oracle_result,'host':host_result,'negative_cases':negatives,
            'dataset':{'path':str(dataset.relative_to(ROOT)),'sha256':sha(dataset),'row_bytes':40,'count':input_count},
            'binary':{'path':str(binary.relative_to(ROOT)),'sha256':sha(binary),'args':['cases.bin']},
            'device_executed':False,'scope':'exact normal-result codec scaling plus signed zero, all rounding/DAZ/FTZ and all64 exception-mask patterns; no generic ldexp parity'}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'host':host_result,'binary':report['binary'],'dataset':report['dataset']},indent=2))


if __name__=='__main__':main()
