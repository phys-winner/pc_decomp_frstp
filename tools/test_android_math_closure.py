"""Check every imported math name and compare new providers with system UCRT on the selected device."""
import argparse
import hashlib
import json
import math
import random
import re
import struct
import subprocess
from pathlib import Path
from leaf_probe import ROOT,windows_toolchain
from fastbuild import CLANG

NAMES=['exp','atan','coshf','sinhf','tanhf','frexp','modf','lroundf','lrintf','_fdclass','_dclass','_dsign','_fdsign','copysign','lgamma','lgammaf','lgammal']
NAMES+=['expf','exp2f','logf','log10f','atanf','fmodf','powf','atan2f','log','sin','cos','pow','ldexp']
WIDE={0,1,5,6,10,11,13,14,16,25,26,27,28,29}
ROW=struct.Struct('<IIQQQQIi')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    parser.add_argument('--oracle-only',action='store_true')
    args=parser.parse_args()
    if not args.oracle_only and not args.serial:parser.error('--serial is required for device checks')
    out=ROOT/'build/qa-20261112-ui/math';out.mkdir(parents=True,exist_ok=True)
    bins,env=windows_toolchain()
    def run(command,**kwargs):
        result=subprocess.run(list(map(str,command)),capture_output=True,**kwargs)
        if result.returncode:
            (out/'failure.stdout').write_bytes(result.stdout);(out/'failure.stderr').write_bytes(result.stderr)
            raise RuntimeError(str(command)+' exit='+str(result.returncode)+'\n'+result.stdout.decode(errors='replace')[-4000:]+result.stderr.decode(errors='replace')[-4000:])
        return result.stdout
    oracle=out/'oracle.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_math_closure_oracle.c','/Fo'+str(out/'oracle.obj'),'/Fe'+str(oracle)],env=env)
    rng=random.Random(0x141cc7d50)
    fbits=lambda x:struct.unpack('<I',struct.pack('<f',x))[0]
    dbits=lambda x:struct.unpack('<Q',struct.pack('<d',x))[0]
    fvalues={0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc01234,0xffc01234,0x7f801234,0xff801234}
    for x in [2**-14,2**-12,.1,.25,.5,.75,1,1.5,2,5,6,9,10,20,88,89,90,100,2**23,2**31]:
        for delta in [-1,0,1]:
            b=fbits(x)+delta;fvalues.update([b,b|0x80000000])
    fvalues.update(rng.getrandbits(32) for _ in range(256))
    fvalues.update(fbits(rng.uniform(-100,100)) for _ in range(256))
    dvalues={0,0x8000000000000000,1,0x8000000000000001,0x000fffffffffffff,0x0010000000000000,0x7fefffffffffffff,0x7ff0000000000000,0xfff0000000000000,0x7ff8000012345678,0x7ff0000012345678,0xfff8000012345678,0xfff0000012345678}
    for x in [2**-54,2**-27,.1,.25,.5,.75,1,1.5,2,5,6,10,20,88,100,709,710,745,746,2**52]:
        for delta in [-1,0,1]:
            b=dbits(x)+delta;dvalues.update([b,b|0x8000000000000000])
    dvalues.update(rng.getrandbits(64) for _ in range(256))
    dvalues.update(dbits(rng.uniform(-750,750)) for _ in range(256))
    inputs=out/'inputs.bin'
    with inputs.open('wb') as stream:
        for kind in range(17):
            for mode in range(16):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)
                for value in sorted(dvalues if kind in WIDE else fvalues):stream.write(struct.pack('<IIQQ',kind,csr,value,0x8000000000000000))
        for kind in range(17,len(NAMES)):
            values=[.125,.5,1.,2.,5.,50.]
            if kind in [19,20,25]:values+=[-1.,2**-120]
            if kind in [21,22,23,24,26,27,28]:values+=[-2.,-5.]
            for mode in range(16):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)
                for value in values:
                    aux=3 if kind==29 else dbits(3) if kind in WIDE else fbits(-3 if kind==22 else 3)
                    stream.write(struct.pack('<IIQQ',kind,csr,dbits(value) if kind in WIDE else fbits(value),aux))
    cases=out/'cases.bin';run([oracle,inputs,cases])
    rows=list(ROW.iter_unpack(cases.read_bytes()))
    if args.oracle_only:
        for kind,name in enumerate(NAMES):
            patterns={}
            for k,csr,x,_,result,aux,after,error in rows:
                if k!=kind:continue
                sign=1<<(63 if k in WIDE else 31);m=x&~sign;inf=0x7ff0000000000000 if sign==1<<63 else 0x7f800000
                category='zero' if m==0 else 'nan' if m>inf else 'inf' if m==inf else 'subnormal' if m<(0x10000000000000 if sign==1<<63 else 0x800000) else 'finite'
                key=str((category,after&~csr,error));patterns.setdefault(key,[hex(x),hex(csr),hex(result),hex(aux)])
            print(name,json.dumps(patterns))
        return
    # UCRT's lgamma reflection loses accuracy next to negative integer poles.
    # CPython's independent Lanczos implementation supplies the real-valued reference there.
    for index,row in enumerate(rows):
        kind,csr,bits,aux,result,output,after,error=row
        if kind in [26,27]:
            value=struct.unpack('<d',struct.pack('<Q',bits))[0]
            rows[index]=(kind,csr,bits,aux,dbits(math.sin(value) if kind==26 else math.cos(value)),output,after,error)
            continue
        if kind<14 or kind>16:continue
        wide=kind in WIDE;fmt='<d' if wide else '<f';integer='<Q' if wide else '<I'
        value=struct.unpack(fmt,struct.pack(integer,bits))[0]
        if math.isnan(value):continue
        if csr&64 and abs(value)<(2**-1022 if wide else 2**-126):value=math.copysign(0,value)
        error=1379
        try:reference=math.lgamma(value)
        except (ValueError,OverflowError):reference=math.inf;error=34
        if not wide:
            try:result=fbits(reference)
            except OverflowError:result=0x7f800000;error=34
        else:result=dbits(reference)
        if math.isinf(reference) and math.isfinite(value):error=34
        rows[index]=(kind,csr,bits,aux,result,output,after,error)
    reference_cases=out/'reference-cases.bin';reference_cases.write_bytes(b''.join(ROW.pack(*row) for row in rows))
    imports=set()
    for path in [ROOT/'generated/startup/recomp_imports.c',*(ROOT/'generated/dependencies').rglob('recomp_imports.c')]:
        imports.update(re.findall(r'"api-ms-win-crt-math-l1-1-0.dll",\s*"([^"]+)"',path.read_text()))
    (out/'math_import_names.inc').write_text(','.join(json.dumps(x) for x in sorted(imports-{'__setusermatherr'})))
    flags=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(out)]
    binary=out/'math-closure'
    run([CLANG,'--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing',*flags,ROOT/'tests/win64_math_closure_fixture.c',ROOT/'android/host/win64_math.c','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    adb=['adb','-s',args.serial];remote='/data/local/tmp/fp-math-closure'
    run([*adb,'push',binary,remote]);run([*adb,'push',reference_cases,remote+'.bin'])
    try:
        run([*adb,'shell','chmod','755',remote]);data=run([*adb,'shell',remote,remote+'.bin',remote+'.actual'])
        (out/'device.log').write_bytes(data);result=json.loads(data)
        assert result['success'] and result['imported_math_functions']==len(imports)-1
        result['serial']=args.serial
        result['imports']=sorted(imports)
        sources=[ROOT/'android/host/win64_math.c',*(ROOT/'android/host').glob('win64_math*.inc'),ROOT/'tests/win64_math_closure_fixture.c',ROOT/'tests/win64_math_closure_oracle.c',Path(__file__).resolve()]
        result['source_sha256']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
        result['reference']='Windows UCRT; CPython mathematical reference for gamma and double sin/cos. Transcendentals: 4 ULP; gamma: 4e-14 double / 4e-7 float relative-or-absolute. Incidental status-flag differences are recorded.'
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(data.decode())
    finally:
        run([*adb,'pull',remote+'.actual',out/'actual.bin'])
        run([*adb,'shell','rm','-f',remote,remote+'.bin',remote+'.actual'])

if __name__=='__main__':main()
