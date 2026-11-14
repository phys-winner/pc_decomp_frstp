"""Bounded Gaussian expf UCRT oracle, paired-consumer proof and ARM CLI; no ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT=ROOT/'build/android-expf-test'
ROW=struct.Struct('<IIIIIiII')


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();cfg=json.loads((ROOT/'port.json').read_text())
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    names=['android/host/win64_math.c','android/host/win64_math.h','native/runtime64/cpu64.h',
           'tests/win64_expf_oracle.c','tests/win64_expf_fixture.c','tools/test_android_expf.py',
           'generated/frostpunk-x64/recomp_funcs_0793.c','generated/frostpunk-x64/recomp_meta.json']
    canonical=ROOT/'generated/frostpunk-x64/recomp_funcs_0793.c'
    meta=json.loads((ROOT/'generated/frostpunk-x64/recomp_meta.json').read_text())
    if sha(canonical)!=meta['generated_files'][canonical.name] or meta['original_exe_sha256']!=cfg['original_exe_sha256']:
        raise ValueError('Sealed matherr canonical changed')
    source=canonical.read_text();start=source.index('void L_000140D871C0(CPU *c)\n{')
    end=source.index('c->rsp += 8; return;',start);end=source.index('\n',end)
    body=source[start:end].replace('void L_000140D871C0(', 'static void baseline_matherr(',1)+'\n}\n'
    (OUT/'expf_matherr.inc').write_text(body)
    before={name:sha(ROOT/name) for name in names};commands=[]

    def run(args,expected=0):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if result.returncode!=expected:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+result.stdout[-7000:]+result.stderr[-7000:])
        return result

    import pefile
    import capstone
    pe_path=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(pe_path)!=cfg['original_exe_sha256']:raise ValueError('Original PE changed')
    pe=pefile.PE(str(pe_path),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
    if pe.get_data(0x140D871C0-base,3)!=bytes.fromhex('33c0c3'):raise ValueError('Default matherr original changed')
    registration=pe.get_data(0x141AE11E7-base,0x15)
    decoded_registration=list(capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64).disasm(registration,0x141AE11E7))
    if not any(i.address==0x141AE11E7 and i.mnemonic=='call' and i.op_str=='0x141ae20f0' for i in decoded_registration):
        raise ValueError('Original matherr registration predicate call changed')
    if pe.get_data(0x141AE11F0-base,3)!=bytes.fromhex('488d0d') or 0x141AE11F7+struct.unpack('<i',pe.get_data(0x141AE11F3-base,4))[0]!=0x140D871C0:
        raise ValueError('Original matherr registration target changed')
    if pe.get_data(0x141AE11F7-base,5)!=bytes.fromhex('e828210000'):raise ValueError('Original matherr setter call changed')
    constants={0x141D96DD8:-2.0,0x141DBDD78:9.0,0x1421F5380:16.0,0x141DBDD7C:36.0,0x1421F5388:25.0,0x1421F5368:4.0,0x1421F52B8:1.0}
    gates={hex(va):pe.get_data(va-base,4).hex() for va in constants}
    for va,value in constants.items():
        if pe.get_data(va-base,4)!=struct.pack('<f',value):raise ValueError('Gaussian constant changed '+hex(va))
    address=0x1410129FA;raw=pe.get_data(address-base,0xAA)
    instructions=list(capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64).disasm(raw,address))
    calls=[(i.address,i.op_str) for i in instructions if i.mnemonic=='call']
    if calls!=[(va,'0x141ae32b2') for va in [0x141012A27,0x141012A3C,0x141012A51,0x141012A66,0x141012A93,0x141012A9F]] or raw[:8].hex()!='f30f109174050000':
        raise ValueError('Original Gaussian argument/call sequence changed')
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('CRT import route changed')
    (OUT/'expf_route.inc').write_text(route+'\n')
    rng=random.Random(0x141012970);xs={0,0x80000000,0xff800000,0xc2a00000,0xb3000000,0xb3000001,0xb2ffffff}
    for exponent in range(134):
        for mantissa in [0,1,2,0x3fffff,0x400000,0x7ffffe,0x7fffff]:
            value=(exponent<<23)|mantissa
            if value<=0x42a00000:xs.add(0x80000000|value)
    xs.update(0x80000000|rng.randrange(0x7f800000) for _ in range(8192))
    for exponent in range(134,255):
        for mantissa in [0,1,2,0x3fffff,0x400000,0x7ffffe,0x7fffff]:xs.add(0x80000000|(exponent<<23)|mantissa)
    xs.update(range(0xc2cff0b5,0xc2cff2b5))
    xs.update(range(0xc2aeac10,0xc2aeac90))
    xs.add(0xc4e0ffff)
    inputs=OUT/'inputs.bin';input_count=0
    with inputs.open('wb') as stream:
        for index,x in enumerate(sorted(xs)):
            for mode in range(16):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                stream.write(struct.pack('<IIII',x,0x3f800000,csr,index));input_count+=1
        stream.write(struct.pack('<IIII',0xc4e0ffff,0x3f800000,0x1fa4,0));input_count+=1
        sigmas=[.05,.5,.625,.75,1.,1.25,1.5,2.,3.,4.,6.,8.,16.]+[rng.uniform(.5,16.) for _ in range(256)]
        for sigma in sigmas:
            sigma_bits=struct.unpack('<I',struct.pack('<f',sigma))[0]
            for mode in range(16):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)
                for coefficient in range(6):
                    stream.write(struct.pack('<IIII',0,sigma_bits,csr,0x80000000|coefficient));input_count+=1
    oracle=OUT/'expf-oracle.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_expf_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    dataset=OUT/'cases.bin';oracle_result=json.loads(run([oracle,inputs,dataset]).stdout)
    for x,_,result,csr,expected_csr,expected_errno,tag,hook_calls in ROW.iter_unpack(dataset.read_bytes()):
        magnitude=x&0x7fffffff;flags=0
        if magnitude and x!=0xff800000:
            if not(magnitude<0x800000 and csr&64):
                flags=32|(2 if magnitude<0x800000 else 0)|(16 if result<0x800000 else 0)
        if expected_csr!=csr|flags or expected_errno!=1379:raise ValueError('UCRT Gaussian expf exception/errno profile differs: '+repr((hex(x),hex(csr),hex(expected_csr),flags)))
        expected_hooks=int(bool(tag&4) and x!=0xff800000 and magnitude>=0x42cff1b5)
        if hook_calls!=expected_hooks:raise ValueError('UCRT return0 matherr boundary differs: '+repr((hex(x),hex(csr),hook_calls,expected_hooks)))
    common=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT)]
    host=OUT/'expf-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',*[x.replace('-I','/I',1) for x in common],
         ROOT/'tests/win64_expf_fixture.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    result=run([host,dataset]);host_result=json.loads(result.stdout);(OUT/'host.log').write_text(result.stdout+result.stderr)
    negatives=[]
    for name in ['positive','NaN','sNaN','infinity','reserved','unmasked_precision','unmasked_denormal',
                 'unmasked_underflow','unmasked_tail_underflow','unmasked_tail_precision',
                 'handler_before_init','handler_bad_init','handler_unknown']:
        result=run([host,'--negative',name],73)
        if 'MATH_INPUT name=expf x=0x' not in result.stderr:raise ValueError('Missing rejection bits')
        negatives.append({'case':name,'exit_code':73,'stderr':result.stderr})
    binary=OUT/'expf-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing',
         '-ffunction-sections','-fdata-sections',*common,ROOT/'tests/win64_expf_fixture.c',ROOT/'android/host/win64_math.c',
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    if before!={name:sha(ROOT/name) for name in names}:raise ValueError('Expf sources changed during fixture')
    report={'success':True,'phase':'host-UCRT-negative-underflow-arm-build','sources':before,'commands':commands,
            'original':{'sha256':cfg['original_exe_sha256'],'argument_block_va':hex(address),'argument_block_bytes':raw.hex(),
                        'constants':gates,'call_sites':calls,'sigma_default':1,'setter_unclamped':True,
                        'default_args':[-4.5,-8,-18,-12.5,-2,-.5]},
            'oracle':oracle_result,'host':host_result,'negative_cases':negatives,
            'matherr':{'handler_va':'0x140d871c0','bytes':'33c0c3','canonical':str(canonical.relative_to(ROOT)),
                       'canonical_body_sha256':sha(OUT/'expf_matherr.inc'),'registration_va':'0x141ae11e7','registration_bytes':registration.hex(),
                       'registration_disassembly':[(hex(i.address),i.mnemonic,i.op_str) for i in decoded_registration],
                       'native_return0_callback_boundary_checked':True,'unknown_or_unverified_handler_rejected':True},
            'dataset':{'path':str(dataset.relative_to(ROOT)),'sha256':sha(dataset),'row_bytes':32,'count':input_count},
            'binary':{'path':str(binary.relative_to(ROOT)),'sha256':sha(binary),'args':['cases.bin']},
            'bounds':{'result_ULP':4,'paired_numerator_ULP':8},
            'consumer_scope':'three exact source ADDSS/MULSS Gaussian pair numerators, reference UCRT exp outputs; no complete render-state/normalization parity claim',
            'underflow':{'actual_input':'0xc4e0ffff','actual_mxcsr':'0x1fa4','UCRT_zero_tail_min_magnitude':'0x42cff1b5',
                         'tail_guest_flags':48,'errno_unchanged':True,'previous_proof_archive':'build/android-expf-test/normal-profile-hardware-pass/archive.json'},
            'device_executed':False,'scope':'all nonpositive finite floats plus -Inf; normal/gradual/zero outputs, allRC/DAZ/FTZ; positive/NaN/unmaskedraisedexceptions rejected'}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'host':host_result,'binary':report['binary'],'dataset':report['dataset']},indent=2))


if __name__=='__main__':main()
