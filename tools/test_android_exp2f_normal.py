"""Bounded normal-result exp2f UCRT/host proof and ARM CLI. No ADB."""
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/android-exp2f-normal-corrected-proof'
ROW=struct.Struct('<IIIIIiII')
BASE_SHA='187070328a532c7105a901a9a74e8157a9e7df4cb743e1e607d91fccb3443982'
def sha(path):
    with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();commands=[];cfg=json.loads((ROOT/'port.json').read_text())
    qualified=ROOT/'build/android-exp2f-provenance-correction/qualified-math-18707032.c'
    names=['android/host/win64_math.c','android/host/win64_math.h','android/host/win64.c','native/runtime64/cpu64.h','tests/win64_angles_oracle.c','tests/win64_angles_fixture.c','tools/test_android_exp2f_normal.py',str(qualified.relative_to(ROOT))]
    pins={name:sha(ROOT/name)for name in names}
    baseline=qualified.read_text();production=(ROOT/'android/host/win64_math.c').read_text()
    if sha(qualified)!=BASE_SHA:raise ValueError('Hardware-qualified math source drift')
    def exp2f_code(text):
        a=text.index('static uint64_t angle_double(uint32_t bits);')
        b=text.index('/* Typed powf preserves the native environment;',a)
        return re.sub(r'/\*.*?\*/','',text[a:b],flags=re.S)
    if exp2f_code(production)!=exp2f_code(baseline):raise ValueError('Hardware-qualified exp2f implementation changed')
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command);r=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if r.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps({'command':command,'expected':expected,'actual':r.returncode,'stdout':r.stdout,'stderr':r.stderr},indent=2)+'\n')
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+r.stdout[-7000:]+r.stderr[-7000:])
        return r
    oracle=(ROOT/'tests/win64_angles_oracle.c').read_text()
    oracle=oracle.replace('float(__cdecl *atan_fn)(float,float)=(float(__cdecl*)(float,float))GetProcAddress(crt,"atan2f");','float(__cdecl *fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"exp2f");').replace('    float(__cdecl *asin_fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"asinf");\n','').replace('!atan_fn||!asin_fn','!fn').replace('value=in.kind?asin_fn(y):atan_fn(y,x);','value=fn(y);')
    oracle_path=OUT/'exp2f_oracle.c';oracle_path.write_text(oracle)
    exe=OUT/'exp2f-oracle.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11',oracle_path,'/Fo'+str(OUT)+'/', '/Fe'+str(exe)])
    bits=lambda value:struct.unpack('<I',struct.pack('<f',value))[0]
    values={0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000}
    values.update(bits((i-4032)/32)for i in range(8097))
    rng=random.Random(0x141cc7d40)
    values.update(bits(rng.uniform(-126,127))for _ in range(4096))
    for index in range(4096):
        negative=bool(index&1);values.add(rng.randrange((0x42fc0000 if negative else 0x42fe0000)+1)|(0x80000000 if negative else 0))
    for e in [-126,-32,-27,-26,-25,-24,-23,-1,0,1,6]:
        raw=bits(2.**e)
        for delta in [-2,-1,0,1,2]:
            for sign in [0,0x80000000]:values.add((raw+delta)|sign)
    for n in range(-126,128):
        raw=bits(n)
        for delta in [-1,0,1]:
            if raw+delta<0:continue
            magnitude=(raw+delta)&0x7fffffff;negative=(raw+delta)>>31
            if magnitude<=(0x42fc0000 if negative else 0x42fe0000):values.add(raw+delta)
    # Representative original expression shapes, rounded to binary32 after
    # each scalar operation. These are input-domain samples, not a claim of
    # complete camera/render-consumer equivalence.
    f32=lambda value:struct.unpack('<f',struct.pack('<f',value))[0]
    rad=f32(math.pi/180);construction=set()
    for left,right in [(20,120),(40,60),(60,90),(90,120)]:
        a=f32(math.tan(f32(f32(left*.5)*rad)));b=f32(math.tan(f32(f32(right*.5)*rad)))
        for tick in range(129):
            alpha=f32(tick/128);construction.add(bits(f32(f32(f32(b-a)*alpha)+a)))
    for left in [.0625,.25,1,2,4,16,128]:
        for right in [.0625,.25,1,2,4,16,128]:
            a=f32(math.log2(left));b=f32(math.log2(right))
            for tick in range(33):
                alpha=f32(tick/32);construction.add(bits(f32(f32(f32(1-alpha)*a)+f32(alpha*b))))
    values.update(construction)
    inputs=OUT/'inputs.bin';input_count=0
    with inputs.open('wb')as f:
        for mode in range(16):
            for index,value in enumerate(sorted(values)):
                csr=0x1f80|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)|(0x15 if index&1 else 0)
                f.write(struct.pack('<IIII',value,0,csr,2));input_count+=1
        # Existing exact powers remain valid even when precision is unmasked.
        for mask in range(64):
            for mode in range(16):
                for value in [0,0x80000000,bits(1),bits(-1),bits(16),bits(-16)]:
                    csr=(mask<<7)|((mode&3)<<13)|(64 if mode&4 else 0)|(32768 if mode&8 else 0)
                    f.write(struct.pack('<IIII',value,0,csr,2));input_count+=1
    data=OUT/'cases.bin';native=json.loads(run([exe,inputs,data]).stdout);rows=list(ROW.iter_unpack(data.read_bytes()))
    if len(rows)!=input_count or native['cases']!=input_count:raise ValueError('Native vector count')
    fractional=0;tiny=0;integral=0
    for raw,_,result,csr,expected_csr,expected_errno,_,calls in rows:
        magnitude=raw&0x7fffffff;value=struct.unpack('<f',struct.pack('<I',raw))[0];integer=value==math.trunc(value)
        flags=0 if integer or magnitude<0x800000 and csr&64 else 32|(2 if magnitude<0x800000 else 0)
        if expected_csr!=csr|flags or expected_errno!=1379 or calls:raise ValueError('Unexpected UCRT exp2f status/error profile '+repr((hex(raw),hex(csr),hex(expected_csr),flags,expected_errno,calls)))
        if integer:
            exact=(int(value)+127)<<23
            if result!=exact:raise ValueError('Existing normal integral result changed')
            integral+=1
        else:fractional+=1
        tiny+=0<magnitude<0x32800000
    fixture=(ROOT/'tests/win64_angles_fixture.c').read_text()
    lo=fixture.index('static unsigned bound(');hi=fixture.index('static int one(',lo)
    fixture=fixture[:lo]+'''static unsigned bound(const row*r){
    uint32_t m=r->y&0x7fffffff;if(!m||m<0x800000&&(r->mxcsr&64u))return 0;
    int e=(int)(m>>23)-127;if(e>=0){unsigned shift=23u-(unsigned)e;if(!(m&((UINT32_C(1)<<shift)-1u)))return 0;}
    return 2;
}
'''+fixture[hi:]
    fixture=fixture.replace('const char*name=r->kind?"asinf":"atan2f";','const char*name="exp2f";').replace('i*37','i*8191').replace('"atan2f_cases\\\":%u,\\\"asinf_cases','"unused_cases\\\":%u,\\\"exp2f_cases')
    lo=fixture.index('    if(!strcmp(argv[1],"--negative")){');hi=fixture.index('    win64_math_self_test();',lo)
    fixture=fixture[:lo]+'''    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.rip=0x141ae3294;c.xmm[0].u32[0]=0x3fa00000;const char*reason=argv[2];
        if(!strcmp(reason,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(reason,"unmasked_precision"))c.mxcsr&=~4096u;
        else if(!strcmp(reason,"unmasked_denormal")){c.xmm[0].u32[0]=1;c.mxcsr&=~256u;}
        else if(!strcmp(reason,"unmasked_subnormal_precision")){c.xmm[0].u32[0]=1;c.mxcsr&=~4096u;}
        else if(!strcmp(reason,"low_range"))c.xmm[0].u32[0]=0xc2fc0001;
        else if(!strcmp(reason,"high_range"))c.xmm[0].u32[0]=0x42fe0001;
        else if(!strcmp(reason,"NaN"))c.xmm[0].u32[0]=0x7fc12345;
        else if(!strcmp(reason,"sNaN"))c.xmm[0].u32[0]=0x7f800001;
        else if(!strcmp(reason,"infinity"))c.xmm[0].u32[0]=0x7f800000;
        else if(!strcmp(reason,"negative_infinity"))c.xmm[0].u32[0]=0xff800000;
        else return 2;win64_math_call(&c,"exp2f");return 99;
    }
'''+fixture[hi:]
    fixture_path=OUT/'exp2f_normal_fixture.c';fixture_path.write_text(fixture)
    route='    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('Existing math route changed')
    (OUT/'angles_route.inc').write_text(route+'\n')
    host=OUT/'exp2f-normal-host.exe';run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/experimental:c11atomics','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(OUT),fixture_path,ROOT/'android/host/win64_math.c','/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    host_result=json.loads(run([host,data]).stdout.strip().splitlines()[-1])
    negative_names=['reserved','unmasked_precision','unmasked_denormal','unmasked_subnormal_precision','low_range','high_range','NaN','sNaN','infinity','negative_infinity']
    negatives=[]
    for name in negative_names:
        r=run([host,'--negative',name],73)
        if 'MATH_INPUT name=exp2f x=0x'not in r.stderr or 'mxcsr='not in r.stderr or 'guest_rip='not in r.stderr or 'rsp='not in r.stderr:raise ValueError('Typed exp2f rejection diagnostic missing')
        negatives.append({'name':name,'exit':73,'stderr':r.stderr})
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';binary=OUT/'exp2f-normal-arm64'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(OUT),fixture_path,ROOT/'android/host/win64_math.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',binary])
    import pefile
    original=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(original)!=cfg['original_exe_sha256']:raise ValueError('Original PE drift')
    pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase;read=lambda va,size:pe.get_data(va-base,size)
    pe.parse_data_directories(directories=[1])
    imports={i.address:(d.dll.decode(),i.name.decode()if i.name else None)for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports}
    if imports[0x141cc7d40][1]!='exp2f'or imports[0x141cc7d58][1]!='atanf':raise ValueError('Math import identity drift')
    for va in [0x140d884b2,0x140ea487d,0x140ea489d,0x140fc1481,0x14101264f]:
        raw=read(va,6)
        if raw[:2]!=b'\xff\x15'or va+6+struct.unpack_from('<i',raw,2)[0]!=0x141cc7d40:raise ValueError('Original exp2f direct-IAT caller gate')
    if read(0x141ae3294,6).hex()!='ff25be4a1e00':raise ValueError('Original atanf thunk identity gate')
    spans=[(0x140fc146a,0x1d),(0x140ea4895,0xe),(0x140d884b2,6),(0x140ea487d,6),(0x14101264f,6)]
    evidence=[{'va':hex(va),'size':size,'bytes':read(va,size).hex(),'sha256':hashlib.sha256(read(va,size)).hexdigest()}for va,size in spans]
    if pins!={name:sha(ROOT/name)for name in names}:raise ValueError('Source drift during proof')
    report={'success':True,'sources':pins,'previous_math_source_sha256':BASE_SHA,'exp2f_code_unchanged_from_hardware_qualified_source':True,'other_math_changes_outside_this_proof_scope':True,'native_UCRT':native,'host':host_result,'negative_stops':negatives,'commands':commands,'original_sha256':cfg['original_exe_sha256'],'original_evidence':evidence,'verified_IAT':{hex(slot):imports[slot]for slot in [0x141cc7d40,0x141cc7d58]},'provenance_correction':'research/android-exp2f-provenance-correction.json','actual_input_evidence':'Prior typed branch was nonintegral STOP, which follows finite1<=abs(x)<=126negative/127positive guard; exactbits/callerreturn were not captured.','domain':'Finite[-126,127] with normal float results. Verified direct-IAT7D40 settings140FC1481 interpolates log2f values,140EA489D reads continuous setting; original initialization140D884B2 and14101264F also call exp2f directly. Tangent/FOV-style samples are additional numerical inputs only: thunk141AE3294 used by1412509D1/14150DAE7 actually imports atanf. No claim that one is the actual exp2f caller.','representative_construction_inputs':len(construction),'construction_scope':'Binary32-RNE log2 interpolation samples from actual exp2f expression shape, plus additional tangent-style numerical samples from original atanf expression shapes. Each is compared directly against UCRT exp2f in16modes; tangent samples do not prove exp2f caller reachability or any full consumer equivalence.','limits':'Integer powers and±0 exact. Fractional results, including tiny/subnormal inputs whenDAZclear, have measured UCRT ULP bound2; IA/range/matherr outsideprofile rejects. GuestCSR status exact, nativeFP/errno restored; nofull exp2f parity orgameplayclaim.','fractional_cases':fractional,'integral_cases':integral,'tiny_nonzero_cases':tiny,'binary':str(binary.relative_to(ROOT)),'binary_sha256':sha(binary),'dataset':str(data.relative_to(ROOT)),'dataset_sha256':sha(data),'expected_ARM':{'cases':input_count,'exp2f_cases':input_count,'max_ULP_at_most':2,'failures':0,'concurrent_cases':8192},'device_executed':False,'original_game_executed':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({key:report[key]for key in ['success','host','fractional_cases','integral_cases','tiny_nonzero_cases','binary','binary_sha256','dataset','dataset_sha256','expected_ARM']},indent=2));print('receipt_sha256='+sha(path));print('math_sha256='+pins['android/host/win64_math.c'])

if __name__=='__main__':main()
