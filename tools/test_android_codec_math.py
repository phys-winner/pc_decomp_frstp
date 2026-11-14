"""Bounded MDCT log/cos/sin UCRT oracle, host fixture and ARM CLI. No ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-codec-math-test'
ROW = struct.Struct('<QQIIIiII')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def bits(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_math.c','android/host/win64_math.h','native/runtime64/cpu64.h',
             'tests/win64_codec_math_oracle.c','tests/win64_codec_math_fixture.c','tools/test_android_codec_math.py']
    before = {name:sha(ROOT / name) for name in files};commands=[]

    def run(args, expected=0):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if result.returncode!=expected:
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+result.stdout[-10000:]+result.stderr[-10000:])
        return result

    import pefile
    pe_path=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(pe_path)!=cfg['original_exe_sha256']:raise ValueError('Original PE changed')
    pe=pefile.PE(str(pe_path),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
    gates={
        0x141C51F24:'8b0683f8407c22443bc07c1d4181f8002000007f14',
        0x141C54069:'488b7a204c8be2',
        0x141C54140:'8b17d3fa498b4d10488b09e8c09800008b57048bcbd3fa498b4d18488b09e8ad980000',
        0x141C5DA84:'66440f6ee64c8bf0450f5be448898424e0000000410f5ac4e82958e8ff0f28f0f20f10056c785900e81958e8fff2440f10152e785900f20f5ef0f2410f58f20f28c6e81158e8ff',
        0x1421F5318:'0000000000000040',0x1421F52E8:'000000000000e03f',0x1421F5354:'db0f4940',
        0x141C697CC:'ff25e6e40500',0x141C697D8:'ff256ae50500',0x141AE32CA:'ff25b04a1e00',
    }
    for va, expected in gates.items():
        actual=pe.get_data(va-base,len(bytes.fromhex(expected)))
        if actual.hex()!=expected:raise ValueError('Original codec proof gate drift at '+hex(va)+': '+actual.hex())
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('Production CRT route changed')
    (OUT/'codec_route.inc').write_text(route+'\n')
    inputs=OUT/'inputs.bin';input_count=0
    with inputs.open('wb') as stream:
        def case(x,kind,mode):
            nonlocal input_count
            if kind&255 and mode&3:
                construction=kind>>8
                exact_zero=(construction==2 and not x>>32) or (not construction and not x&0x7fffffffffffffff)
                if not exact_zero:return
            csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)
            if input_count&16:csr|=0x15
            stream.write(struct.pack('<QII',x,kind,csr));input_count+=1
        for n in [64,128,256,512,1024,2048,4096,8192]:
            for mode in range(16):
                case(n,0x100,mode)
                for i in range(n//4):
                    for construction in (2,3):
                        for kind in (1,2):case(n|(i<<32),(construction<<8)|kind,mode)
                for i in range(n//8):
                    for kind in (1,2):case(n|(i<<32),0x400|kind,mode)
        rng=random.Random(0x141C5DA10)
        for n in [1,2,3,4,5,63,65,127,129,255,257,511,513,1023,1025,4095,4097,8191,8193,16777215,2147483647]+[rng.randrange(1,2147483648) for _ in range(256)]:
            for mode in range(16):case(n,0x100,mode)
        angles={0,0x80000000,0x39000000,0x39000001,0x397fffff,0x3f000000,0x3f800000,0x3fc90fda,0x3fc90fdb,0x3fc90fdc,0x40490fda,0x40490fdb,0x40490fdc,0x40800000}
        angles.update(rng.randrange(0x39000000,0x40800001) for _ in range(256))
        for raw in sorted(angles):
            x=bits(struct.unpack('<f',struct.pack('<I',raw))[0])
            for kind in (1,2):
                for mode in range(16):case(x,kind,mode)
    oracle=OUT/'codec-oracle.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_codec_math_oracle.c',
         '/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    dataset=OUT/'cases.bin';oracle_result=json.loads(run([oracle,inputs,dataset]).stdout)
    counts=[0,0,0]
    for x,result,kind,csr,expected_csr,expected_errno,_,_ in ROW.iter_unpack(dataset.read_bytes()):
        counts[kind]+=1
        flags=0 if (not kind and x==0x3ff0000000000000) or (kind and not x&0x7fffffffffffffff) else 32
        if expected_csr!=csr|flags or expected_errno!=1379:
            raise ValueError('UCRT codec status/errno profile differs: '+repr((kind,hex(x),hex(csr),hex(expected_csr),expected_errno)))
    common=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT)]
    host=OUT/'codec-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',*[x.replace('-I','/I',1) for x in common],
         ROOT/'tests/win64_codec_math_fixture.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    result=run([host,dataset]);host_result=json.loads(result.stdout);(OUT/'host.log').write_text(result.stdout+result.stderr)
    negatives=[]
    for name in ('log','cos','sin'):
        reasons=['precision','reserved','negative','NaN','infinity','fraction','large','tiny']
        if name!='log':reasons+=['round_down','round_up','round_zero']
        for reason in reasons:
            result=run([host,'--negative',name,reason],73)
            if 'MATH_INPUT name='+name+' x=0x' not in result.stderr:raise ValueError('Missing rejection input bits')
            negatives.append({'name':name,'reason':reason,'exit_code':73,'stderr':result.stderr})
    binary=OUT/'codec-math-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing',
         '-ffunction-sections','-fdata-sections',*common,ROOT/'tests/win64_codec_math_fixture.c',ROOT/'android/host/win64_math.c',
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    if before!={name:sha(ROOT/name) for name in files}:raise ValueError('Codec math sources changed during test')
    report={'success':True,'phase':'nearest-trig-host-oracle-arm-build','sources':before,'commands':commands,
            'original':{'sha256':cfg['original_exe_sha256'],'byte_gates':{hex(va):raw for va,raw in gates.items()},
                        'scope':'validated blocksize64..8192, logN/log2 consumer, all three exact MDCT table angle loops'},
            'oracle':oracle_result,'host':host_result,'counts_by_kind':counts,'negative_cases':negatives,
            'dataset':{'path':str(dataset.relative_to(ROOT)),'sha256':sha(dataset),'row_bytes':40,'count':input_count},
            'binary':{'path':str(binary.relative_to(ROOT)),'sha256':sha(binary),'args':['cases.bin']},
            'bounds':{'log_double_ULP':4,'nearest_trig_double_ULP':4,'nonzero_trig_directed_rounding':'rejected',
                      'exact_zero_trig_rounding':'all four modes','float_consumer':'exact bits','log2N_consumer':'exact integer'},
            'device_executed':False,'general_math_parity':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'host':host_result,'binary':report['binary'],'dataset':report['dataset']},indent=2))


if __name__=='__main__':main()
