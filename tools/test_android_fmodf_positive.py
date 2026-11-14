"""Finite float dividend/positive normal divisor UCRT proof; no ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/android-fmodf-positive-test'
ROW=struct.Struct('<IIIIIiII')

def sha(path):
    with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def exact_remainder(x,y,csr):
    """Independent unbounded integer modulo in units of2^-149."""
    def units(value):
        e=value>>23
        return((value&0x7fffff)|(0x800000 if e else 0))<<max(e-1,0)
    def float_bits(value):
        if not value:return 0
        if value<0x800000:return value
        shift=value.bit_length()-24
        if value&((1<<shift)-1):raise ValueError('Nonrepresentable exact dyadic remainder')
        return((shift+1)<<23)|((value>>shift)&0x7fffff)
    magnitude=x&0x7fffffff;sign=x&0x80000000;result=x;flags=0
    if 0<magnitude<0x800000:
        if not csr&64:
            flags=2
            if csr&32768:result=sign;flags|=48
    elif magnitude>=y:
        result=sign|float_bits(units(magnitude)%units(y))
        if not result&0x7fffffff and magnitude>y and (csr>>13)&3==1:result^=0x80000000
        elif 0<result&0x7fffffff<0x800000 and csr&32768:result=sign;flags|=48
    return result,csr|flags

def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();cfg=json.loads((ROOT/'port.json').read_text());commands=[]
    names=['android/host/win64_math.c','android/host/win64_math.h','android/host/win64.c','native/runtime64/cpu64.h','tests/win64_fmodf_oracle.c','tests/win64_fmodf_fixture.c','tools/test_android_fmodf_positive.py']
    acos_dir=ROOT/'build/android-acosf-candidate'
    names+=['build/android-acosf-candidate/acosf_fragment.inc','build/android-acosf-candidate/acosf_fixture.c','build/android-acosf-candidate/cases.bin','build/android-acosf-candidate/result.json']
    pins={name:sha(ROOT/name)for name in names}
    if (ROOT/'android/host/win64_math.c').read_text().count((acos_dir/'acosf_fragment.inc').read_text())!=1:raise ValueError('Qualified acosf fragment is not exact')
    old=ROOT/'build/android-fmodf-test'
    historical={str(p.relative_to(ROOT)):sha(p)for p in old.iterdir()if p.is_file()and(p.suffix in ['.json','.exe']or p.name in ['cases.bin','fmodf-arm64'])}
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command);r=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if r.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps({'command':command,'expected':expected,'actual':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+r.stdout[-7000:]+r.stderr[-7000:])
        return r
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('Existing math route changed')
    (OUT/'fmodf_route.inc').write_text(route+'\n')
    fixture=(ROOT/'tests/win64_fmodf_fixture.c').read_text()
    fixture=fixture.replace('else if(!strcmp(reason,"other_divisor"))c.xmm[1].u32[0]=0x40000000;','else if(!strcmp(reason,"subnormal_divisor"))c.xmm[1].u32[0]=1;\n        else if(!strcmp(reason,"infinite_divisor"))c.xmm[1].u32[0]=0x7f800000;\n        else if(!strcmp(reason,"negative_zero_divisor"))c.xmm[1].u32[0]=0x80000000;\n        else if(!strcmp(reason,"unmasked_nonunit_intermediate")){c.xmm[0].u32[0]=0x41200000;c.xmm[1].u32[0]=0x41100000;c.mxcsr&=~4096u;}\n        else if(!strcmp(reason,"unmasked_nonunit_underflow")){c.xmm[0].u32[0]=0x01000000;c.xmm[1].u32[0]=0x00800001;c.mxcsr=(c.mxcsr|32768u)&~2048u;}')
    fixture=fixture.replace('i*37','i*8191').replace('unit_divisor_only','positive_normal_divisor')
    fixture_path=OUT/'fmodf_positive_fixture.c';fixture_path.write_text(fixture)
    raw_old=(old/'cases.bin').read_bytes();old_rows=list(ROW.iter_unpack(raw_old))
    pairs={(x,y)for x in [0,0x80000000,1,0x80000001,0x7fffff,0x807fffff,0x800000,0x80800000,0x3f800000,0xbf800000,0x41100000,0xc1100000,0x7f7fffff,0xff7fffff]for y in [0x3f800000,0x40000000,0x41100000,0x40a00000,0x40e00000,0x43b40000,0x3c888889,0x00800000,0x00800001,0x00ffffff,0x3f800001,0x7f7fffff]}
    rng=random.Random(0x41100000)
    pairs.update((rng.randrange(0x7f800000)|(rng.getrandbits(1)<<31),rng.randrange(0x800000,0x7f800000))for _ in range(8192))
    fbits=lambda value:struct.unpack('<I',struct.pack('<f',value))[0]
    # Advancing animation time, exact multiples, adjacent ULPs and the actual0%9.
    for y in [0x3f800000,0x40000000,0x41100000,0x40a00000,0x40e00000,0x43b40000,0x3c888889,0x00800000,0x00800001,0x00ffffff,0x3f800001,0x7f7fffff]:
        divisor=struct.unpack('<f',struct.pack('<I',y))[0]
        for k in [1,2,3,9,16,256,65536]:
            if divisor*k>3.4028234663852886e38:continue
            raw=fbits(divisor*k)
            for delta in [-1,0,1]:
                if 0<=raw+delta<0x7f800000:
                    for sign in [0,0x80000000]:pairs.add(((raw+delta)|sign,y))
    pairs.update((fbits(i/64),0x41100000)for i in range(4097))
    inputs=OUT/'inputs.bin';input_count=len(old_rows)
    with inputs.open('wb')as stream:
        for x,y,_,csr,_,_,tag,_ in old_rows:stream.write(struct.pack('<IIII',x,y,csr,tag))
        for index,(x,y)in enumerate(sorted(pairs)):
            for mode in range(16):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                stream.write(struct.pack('<IIII',x,y,csr,input_count));input_count+=1
    oracle=OUT/'fmodf-oracle.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT/'tests/win64_fmodf_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    dataset=OUT/'cases.bin';native=json.loads(run([oracle,inputs,dataset]).stdout)
    if native['cases']!=input_count:raise ValueError('Native vector count')
    raw=dataset.read_bytes()
    if raw[:len(raw_old)]!=raw_old:raise ValueError('Accepted unit-divisor oracle prefix changed')
    subnormal_results=0;nonunit=0
    for x,y,value,csr,expected_csr,expected_errno,_,_ in ROW.iter_unpack(raw):
        exact,flags=exact_remainder(x,y,csr)
        if(value,expected_csr,expected_errno)!=(exact,flags,1379):raise ValueError('Independent integer modulo vs UCRT '+repr((hex(x),hex(y),hex(csr),hex(value),hex(exact),hex(expected_csr),hex(flags))))
        nonunit+=y!=0x3f800000;subnormal_results+=0<value&0x7fffffff<0x800000
    includes=['/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(OUT)]
    host=OUT/'fmodf-positive-host.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',*includes,fixture_path,ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    result=run([host,dataset]);host_result=json.loads(result.stdout);(OUT/'host.log').write_text(result.stdout+result.stderr)
    negatives=['zero_divisor','negative_divisor','subnormal_divisor','infinite_divisor','negative_zero_divisor','NaN_divisor','reserved','NaN','sNaN','infinity','unmasked_denormal','unmasked_underflow','unmasked_precision','unmasked_intermediate','unmasked_nonunit_intermediate','unmasked_nonunit_underflow']
    negative_results=[]
    for name in negatives:
        r=run([host,'--negative',name],73)
        if 'MATH_INPUT name=fmodf x=0x'not in r.stderr:raise ValueError('Missing typed stop')
        negative_results.append({'case':name,'exit_code':73,'stderr':r.stderr})
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';binary=OUT/'fmodf-positive-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',*[p.replace('/I','-I',1)for p in includes],fixture_path,ROOT/'android/host/win64_math.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    # Re-link the unchanged hardware-qualified acosf fixture with CURRENT
    # production math, rather than copying its whole old candidate source.
    acos_host=OUT/'acosf-integrated-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/experimental:c11atomics','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(acos_dir),acos_dir/'acosf_fixture.c',ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(acos_host)])
    acos_host_result=json.loads(run([acos_host,acos_dir/'cases.bin']).stdout.strip().splitlines()[-1])
    acos_negatives=['reserved','unmasked_precision','unmasked_invalid','custom_handler','known_before_init','mutated_init']
    for name in acos_negatives:run([acos_host,'--negative',name],73)
    acos_binary=OUT/'acosf-integrated-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(acos_dir),acos_dir/'acosf_fixture.c',ROOT/'android/host/win64_math.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',acos_binary])
    acos_integrated={'host':acos_host_result,'binary':{'path':str(acos_binary.relative_to(ROOT)),'sha256':sha(acos_binary),'args':['acosf-cases.bin']},'dataset':{'source_path':str((acos_dir/'cases.bin').relative_to(ROOT)),'sha256':sha(acos_dir/'cases.bin')},'expected_ARM':{'cases':266512,'acosf_cases':266512,'max_ULP_at_most':2,'failures':0,'concurrent_cases':8192},'negative_names':acos_negatives}
    import pefile,capstone
    original=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(original)!=cfg['original_exe_sha256']:raise ValueError('Original PE drift')
    pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase;read=lambda va,size:pe.get_data(va-base,size)
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    for va in [0x1410e712b,0x1410b7744]:
        instructions=list(md.disasm(read(va,5),va))
        if len(instructions)!=1 or instructions[0].mnemonic!='call'or instructions[0].op_str!='0x141ae329a':raise ValueError('Relevant original fmodf call drift')
    spans=[(0x1410e710d,0x25),(0x1410b7731,0x18),(0x141ae329a,6),(0x141d5a2a4,4)]
    evidence=[{'va':hex(va),'size':size,'bytes':read(va,size).hex(),'sha256':hashlib.sha256(read(va,size)).hexdigest()}for va,size in spans]
    if pins!={name:sha(ROOT/name)for name in names}or historical!={name:sha(ROOT/name)for name in historical}:raise ValueError('Proof/historical source drift')
    report={'success':True,'sources':pins,'historical_unit_proof_unchanged':historical,'commands':commands,'oracle':native,'host':host_result,'negative_cases':negative_results,'acosf_integrated':acos_integrated,'original_sha256':cfg['original_exe_sha256'],'original_evidence':evidence,'actual_boundary':{'x':'0x00000000','y':'0x41100000','mxcsr':'0x1fa4','thunk':'0x141ae329a','caller_return_not_captured':True},'original_scope':'Original animation call1410E712B uses max(time,0) modulo objectduration+20; angle calls1410B7744/7762 use360. Actual y9 is grounded by runtime input, not attributed to a particular original direct caller.','scope':'Finite binary32 dividend and positive normal finite divisor. Exact integer dyadic remainder; native UCRT early-subnormal DAZ/FTZ and cancellation-zero policy. Zero/negative/subnormal/nonfinite divisors, nonfinite dividends, reserved CSR or raised/unmasked intermediate exceptions reject.','unit_oracle_prefix_exact':True,'legacy_unit_cases':len(old_rows),'nonunit_cases':nonunit,'subnormal_results':subnormal_results,'dataset':{'path':str(dataset.relative_to(ROOT)),'sha256':sha(dataset),'row_bytes':32,'count':input_count},'binary':{'path':str(binary.relative_to(ROOT)),'sha256':sha(binary),'args':['cases.bin']},'expected_ARM':{'cases':input_count,'bit_exact':input_count,'failures':0,'concurrent_cases':8192,'positive_normal_divisor':True},'device_executed':False,'original_game_executed':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'math_source_sha256':pins['android/host/win64_math.c'],'host':host_result,'binary':report['binary'],'dataset':report['dataset'],'negative_count':len(negatives),'acosf_integrated':acos_integrated,'receipt_sha256':sha(path)},indent=2))

if __name__=='__main__':main()
