"""Actual matrix Euler atan2f/asinf boundary; system UCRT and isolated ARM proof, no ADB."""
import hashlib
import json
from pathlib import Path
import math
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain
OUT=ROOT/'build/android-angles-test'
ROW=struct.Struct('<IIIIIiII')

def sha(path):
    with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();commands=[];cfg=json.loads((ROOT/'port.json').read_text())
    names=['android/host/win64_math.c','android/host/win64_math.h','android/host/win64.c','native/runtime64/cpu64.h','tests/win64_angles_oracle.c','tests/win64_angles_fixture.c','tools/test_android_angles.py','generated/frostpunk-x64/recomp_funcs_0804.c']
    pins={name:sha(ROOT/name)for name in names};production=(ROOT/'android/host/win64_math.c').read_text()
    a=production.index('static void angles_stop(');b=production.index('static uint64_t integer_round(',a)
    baseline=production[:a]+production[b:];baseline=baseline.replace('||angles_call(c,name);',';')
    # read_text normalizes CRLF; accepted math source is LF.
    if hashlib.sha256(baseline.encode()).hexdigest()!='cedc5577fa27f40e8410681a97aac6b476a7c433cc3b01074650cd9b2e54a6c1':raise ValueError('Existing math implementation changed outside angle closure')
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command);r=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if r.returncode!=expected:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+r.stdout[-6000:]+r.stderr[-6000:])
        return r
    exe=OUT/'angles-oracle.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_angles_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(exe)])
    values=[0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,0x3e800000,0xbe800000,0x3f800000,0xbf800000,0x40000000,0xc0000000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00001,0xffc12345,0x7f800001,0xff800001]
    fbits=lambda value:struct.unpack('<I',struct.pack('<f',value))[0]
    exponent=lambda value:(value>>23)-127 if value>>23 else value.bit_length()-150
    rng=random.Random(0x140e33af8);pairs={(y,x)for y in values for x in values}
    pairs.update((rng.getrandbits(32),rng.getrandbits(32))for _ in range(8192))
    pairs.update((fbits(math.sin(i*2*math.pi/4096)),fbits(math.cos(i*2*math.pi/4096)))for i in range(4096))
    for e in [-149,-127,-126,-125,-27,-26,-14,-13,-12,0,26,27,126,127]:
        y=fbits(2.**e)
        for dy in [-1,0,1]:
            for signy in [0,0x80000000]:
                for signx in [0,0x80000000]:pairs.add((max(1,y+dy)|signy,0x3f800000|signx))
    asins=set(values)|{fbits((i-2048)/2048)for i in range(4097)}|{rng.randrange(0x3f800001)|(0x80000000 if i&1 else 0)for i in range(2048)}
    asins.update([0x387fffff,0x38800000,0x38800001,0xb87fffff,0xb8800000,0xb8800001,0x3f7ffffe,0x3f7fffff,0xbf7fffff,0x3f800001,0xbf800001])
    inputs=OUT/'inputs.bin';rejected=0;input_count=0
    with inputs.open('wb')as f:
        for mode in range(16):
            for index,(y,x)in enumerate(sorted(pairs)):
                ay,ax=y&0x7fffffff,x&0x7fffffff
                if mode&4:
                    if ay<0x800000:ay=0
                    if ax<0x800000:ax=0
                if 0<ay<0x7f800000 and 0<ax<0x7f800000 and not x>>31 and exponent(ay)-exponent(ax)<-125:rejected+=1;continue
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                f.write(struct.pack('<IIII',y,x,csr,0));input_count+=1
            for index,y in enumerate(sorted(asins)):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                f.write(struct.pack('<IIII',y,0,csr,1));input_count+=1
    data=OUT/'cases.bin';native=json.loads(run([exe,inputs,data]).stdout);rows=list(ROW.iter_unpack(data.read_bytes()))
    if len(rows)!=input_count or native['cases']!=input_count:raise ValueError('Native vector count')
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('Existing math route changed')
    (OUT/'angles_route.inc').write_text(route+'\n')
    host=OUT/'angles-host.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/experimental:c11atomics','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(OUT),ROOT/'tests/win64_angles_fixture.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    result=json.loads(run([host,data]).stdout.strip().splitlines()[-1])
    negative=['reserved','tiny_ratio','unmasked_precision','unmasked_invalid','unmasked_denormal','unmasked_underflow','asin_reserved','asin_unmasked_precision','asin_unmasked_invalid','asin_handler']
    stops=[dict(name=name,exit=run([host,'--negative',name],73).returncode)for name in negative]
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';binary=OUT/'angles-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT),ROOT/'tests/win64_angles_fixture.c',ROOT/'android/host/win64_math.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',binary])
    import pefile,capstone
    original=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(original)!=cfg['original_exe_sha256']:raise ValueError('Original PE drift')
    pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase;read=lambda va,size:pe.get_data(va-base,size);md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    calls=[(0x140e33af8,0x141ae3276),(0x140e33b0b,0x141ae3270),(0x140e33b1f,0x141ae3276)]
    for va,target in calls:
        decoded=list(md.disasm(read(va,5),va))
        if len(decoded)!=1 or decoded[0].mnemonic!='call'or decoded[0].op_str!=hex(target):raise ValueError('Actual matrix call bytes changed')
    spans=[(0x140e33ad7,0x54),(0x141ae3270,12)]
    evidence=[{'va':hex(va),'bytes':read(va,size).hex(),'sha256':hashlib.sha256(read(va,size)).hexdigest()}for va,size in spans]
    if pins!={name:sha(ROOT/name)for name in names}:raise ValueError('Source drift during validation')
    report={'success':True,'sources':pins,'old_math_source_reconstructed_sha256':'cedc5577fa27f40e8410681a97aac6b476a7c433cc3b01074650cd9b2e54a6c1','native_UCRT':native,'host':result,'negative_stops':stops,'commands':commands,'original_sha256':cfg['original_exe_sha256'],'original_evidence':evidence,'actual_domain':'atan2f(y,x) from normalized rotation matrix entries, asinf(-entry) guarded between approximately+-0.999999; original Euler conversion then multiplies angles by degrees constant.','limits':'Bounded matrix-angle path; atan2f positive-x finite exponent ratio below2^-125 rejects; unmasked raised exceptions reject; custom asinf domain matherr rejects; masked finite polynomial results measured<=2ULP, not general libm bitparity.','binary':str(binary.relative_to(ROOT)),'binary_sha256':sha(binary),'dataset':str(data.relative_to(ROOT)),'dataset_sha256':sha(data),'native_probe_excluded_tiny_ratio_rows':rejected,'expected_ARM':{'cases':input_count,'atan2f_cases':sum(not r[6]for r in rows),'asinf_cases':sum(bool(r[6])for r in rows),'max_ULP_at_most':2,'failures':0,'concurrent_cases':8192},'device_executed':False,'original_game_executed':False,'gameplay_proven':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k]for k in ['success','native_UCRT','host','binary','binary_sha256','dataset','dataset_sha256','expected_ARM']},indent=2));print('receipt_sha256='+sha(path))

if __name__=='__main__':main()
