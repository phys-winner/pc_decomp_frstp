"""Actual automaton reciprocal-radius atanf boundary; UCRT/ARM proof, no ADB."""
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/android-atanf-test'
ROW=struct.Struct('<IIIIIiII')
BASE_SHA='187070328a532c7105a901a9a74e8157a9e7df4cb743e1e607d91fccb3443982'
OLD_COMMENT=' * guard. Original settings/camera callers also interpolate scalar/log2\n * inputs. Support finite[-126,127], whose float results remain normal;'
NEW_COMMENT=' * guard. Original direct-IAT settings callers interpolate log2 inputs or\n * read continuous scalars. Support finite[-126,127], whose results stay normal;'
def sha(path):
    with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();commands=[];cfg=json.loads((ROOT/'port.json').read_text())
    names=['android/host/win64_math.c','android/host/win64_math.h','android/host/win64.c','native/runtime64/cpu64.h','tests/win64_angles_oracle.c','tests/win64_angles_fixture.c','tools/test_android_atanf.py','build/android-atanf-test/pre-extension-win64_math.c']
    pins={name:sha(ROOT/name)for name in names};production=(ROOT/'android/host/win64_math.c').read_text()
    if sha(OUT/'pre-extension-win64_math.c')!=BASE_SHA:raise ValueError('Accepted pre-extension math drift')
    baseline=(OUT/'pre-extension-win64_math.c').read_text()
    a=production.index('/* Actual14160C9A5 reaches this through1416089D0:');b=production.index('static uint64_t integer_round(',a)
    reconstructed=(production[:a]+'\n'+production[b:]).replace('||atanf_call(c,name);',';')
    if baseline.count(OLD_COMMENT)!=1 or reconstructed!=baseline.replace(OLD_COMMENT,NEW_COMMENT):raise ValueError('Changes outside atanf closure/comment correction')
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command);r=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if r.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps({'command':command,'expected':expected,'actual':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+r.stdout[-6500:]+r.stderr[-6500:])
        return r
    oracle=(ROOT/'tests/win64_angles_oracle.c').read_text()
    oracle=oracle.replace('float(__cdecl *atan_fn)(float,float)=(float(__cdecl*)(float,float))GetProcAddress(crt,"atan2f");','float(__cdecl *fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"atanf");').replace('    float(__cdecl *asin_fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"asinf");\n','').replace('!atan_fn||!asin_fn','!fn').replace('value=in.kind?asin_fn(y):atan_fn(y,x);','value=fn(y);')
    oracle_path=OUT/'atanf_oracle.c';oracle_path.write_text(oracle)
    exe=OUT/'atanf-oracle.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',oracle_path,'/Fo'+str(OUT)+'/', '/Fe'+str(exe)])
    bits=lambda x:struct.unpack('<I',struct.pack('<f',x))[0]
    values={0,0x80000000,0x7f800000,0xff800000,0x7fc12345,0xffc54321,0x7f800001,0xff800001}
    for exponent in range(2,255):
        for fraction in [0,1,2,0x3fffff,0x400000,0x7ffffe,0x7fffff]:
            for sign in [0,0x80000000]:values.add((exponent<<23)|fraction|sign)
    rng=random.Random(0x14160c9a5)
    values.update(rng.randrange(0x01000000,0x7f800000)|(rng.getrandbits(1)<<31)for _ in range(8192))
    values.update(bits((i-2048)/256)for i in range(4097))
    # Original argument construction1/radius, plus the empty getter fallback.
    reciprocal_inputs={bits(1/r)for r in [.01,.05,.1,.25,.5,1,2,4,8,16,32,64,128,256,1024,4096,65536]}
    values.update(reciprocal_inputs)
    for raw in [0x01000000,0x38000000,0x3f000000,0x3f800000,0x40000000,0x4c800000,0x7f7fffff]:
        for delta in [-2,-1,0,1,2]:
            if 0x01000000<=raw+delta<0x7f800000:
                for sign in [0,0x80000000]:values.add((raw+delta)|sign)
    inputs=OUT/'inputs.bin';input_count=0
    with inputs.open('wb')as f:
        for mode in range(16):
            for index,value in enumerate(sorted(values)):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                f.write(struct.pack('<IIII',value,0,csr,2));input_count+=1
        for mask in range(64):
            for mode in range(16):
                for value in [0,0x80000000]:
                    f.write(struct.pack('<IIII',value,0,(mask<<7)|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0),2));input_count+=1
    data=OUT/'cases.bin';native=json.loads(run([exe,inputs,data]).stdout);rows=list(ROW.iter_unpack(data.read_bytes()))
    if len(rows)!=input_count or native['cases']!=input_count:raise ValueError('Native vector count')
    for x,_,value,csr,expected_csr,expected_errno,_,calls in rows:
        m=x&0x7fffffff
        flags=(1 if not x&0x00400000 else 0)if m>0x7f800000 else(32 if m else 0)
        if expected_csr!=csr|flags or expected_errno!=1379 or calls:raise ValueError('Unexpected UCRT normal-result atanf status '+repr((hex(x),hex(csr),hex(expected_csr),flags,expected_errno,calls)))
    fixture=(ROOT/'tests/win64_angles_fixture.c').read_text();a=fixture.index('static unsigned bound(');b=fixture.index('static int one(',a)
    fixture=fixture[:a]+'''static unsigned bound(const row*r){uint32_t m=r->y&0x7fffffff;return !m||m>=0x7f800000?0:2;}
'''+fixture[b:]
    fixture=fixture.replace('const char*name=r->kind?"asinf":"atan2f";','const char*name="atanf";').replace('i*37','i*8191').replace('"atan2f_cases\\\":%u,\\\"asinf_cases','"unused_cases\\\":%u,\\\"atanf_cases')
    a=fixture.index('    if(!strcmp(argv[1],"--negative")){');b=fixture.index('    win64_math_self_test();',a)
    fixture=fixture[:a]+'''    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.rip=0x141ae3294;c.xmm[0].u32[0]=0x3f800000;const char*reason=argv[2];
        if(!strcmp(reason,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(reason,"unmasked_precision"))c.mxcsr&=~4096u;
        else if(!strcmp(reason,"unmasked_invalid")){c.xmm[0].u32[0]=0x7f800001;c.mxcsr&=~128u;}
        else if(!strcmp(reason,"subnormal"))c.xmm[0].u32[0]=1;
        else if(!strcmp(reason,"tiny_normal"))c.xmm[0].u32[0]=0x00800000;
        else if(!strcmp(reason,"below_cutoff"))c.xmm[0].u32[0]=0x00ffffff;
        else if(!strcmp(reason,"infinity_unmasked_precision")){c.xmm[0].u32[0]=0x7f800000;c.mxcsr&=~4096u;}
        else return 2;win64_math_call(&c,"atanf");return 99;
    }
'''+fixture[b:]
    fixture_path=OUT/'atanf_fixture.c';fixture_path.write_text(fixture)
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('Existing math import route drift')
    (OUT/'angles_route.inc').write_text(route+'\n')
    host=OUT/'atanf-host.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/experimental:c11atomics','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(OUT),fixture_path,ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    host_result=json.loads(run([host,data]).stdout.strip().splitlines()[-1]);negative_names=['reserved','unmasked_precision','unmasked_invalid','subnormal','tiny_normal','below_cutoff','infinity_unmasked_precision'];negative=[]
    for name in negative_names:
        r=run([host,'--negative',name],73)
        if 'MATH_INPUT name=atanf x=0x'not in r.stderr or 'rsp='not in r.stderr:raise ValueError('Missing typed atanf stop')
        negative.append({'name':name,'exit':73,'stderr':r.stderr})
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';binary=OUT/'atanf-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT),fixture_path,ROOT/'android/host/win64_math.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',binary])
    import pefile,capstone
    original=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(original)!=cfg['original_exe_sha256']:raise ValueError('Original PE drift')
    pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase;read=lambda va,size:pe.get_data(va-base,size);pe.parse_data_directories(directories=[1])
    imports={i.address:(d.dll.decode(),i.name.decode()if i.name else None)for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports}
    if imports[0x141cc7d58][1]!='atanf'or imports[0x141cc7d40][1]!='exp2f':raise ValueError('Original math IAT names differ')
    md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    for va,target,mnemonic,size in [(0x14160c9a5,0x1416089d0,'call',5),(0x141608a3c,0x141ae3294,'jmp',5)]:
        instructions=list(md.disasm(read(va,size),va))
        if len(instructions)!=1 or instructions[0].mnemonic!=mnemonic or instructions[0].op_str!=hex(target):raise ValueError('Actual atanf caller byte gate')
    if read(0x141ae3294,6).hex()!='ff25be4a1e00':raise ValueError('atanf tail IAT gate')
    spans=[(0x14160c990,0x28),(0x1416089d0,0x71),(0x141233b20,0x6d),(0x141ae3294,6)]
    evidence=[{'va':hex(va),'size':size,'bytes':read(va,size).hex(),'sha256':hashlib.sha256(read(va,size)).hexdigest()}for va,size in spans]
    strings={}
    for va in [0x141fbed00,0x141fbe5d0,0x141fbed30]:
        raw=read(va,256).split(b'\0')[0]+b'\0';strings[hex(va)]={'text':raw[:-1].decode(),'bytes':raw.hex(),'sha256':hashlib.sha256(raw).hexdigest()}
    if pins!={name:sha(ROOT/name)for name in names}:raise ValueError('Proof source drift')
    report={'success':True,'sources':pins,'previous_math_source_sha256':BASE_SHA,'other_math_semantics_unchanged':True,'outside_atanf_change':'Comment-only correction: original direct-IAT exp2f settings callers; FOV/tangent thunk141AE3294 is atanf.','native_UCRT':native,'host':host_result,'negative_stops':negative,'commands':commands,'original_sha256':cfg['original_exe_sha256'],'original_evidence':evidence,'original_strings':strings,'verified_IAT':{hex(slot):imports[slot]for slot in [0x141cc7d58,0x141cc7d40]},'actual_caller':'14160C990 calls getter141233B20, then wrapper1416089D0 at14160C9A5. Wrapper asserts radius>0, computes1/radius and tailjumps verifiedatanf thunk141AE3294. Getter reads firstactiveentity+48; emptyfallback+0 permits+Inf argument.','profile':'±0 andInf/NaN exact measured nativepolicy; finite |x|>=2^-125. Smaller nonzero inputs reject because native directed/subnormalUFbehavior is outside normal-result qualification. No radiusupperbound presumed. Finite normal results measured<=2ULP; guestCSRflags exact, nativeFP/errno restored.','representative_reciprocal_inputs':len(reciprocal_inputs),'binary':str(binary.relative_to(ROOT)),'binary_sha256':sha(binary),'dataset':str(data.relative_to(ROOT)),'dataset_sha256':sha(data),'expected_ARM':{'cases':input_count,'atanf_cases':input_count,'max_ULP_at_most':2,'failures':0,'concurrent_cases':8192},'device_executed':False,'original_game_executed':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k]for k in ['success','host','binary','binary_sha256','dataset','dataset_sha256','expected_ARM']},indent=2));print('receipt_sha256='+sha(path));print('math_sha256='+pins['android/host/win64_math.c'])

if __name__=='__main__':main()
