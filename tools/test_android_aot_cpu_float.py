"""Host-native differential proof and AArch64 CLI compilation; never uses adb."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT,windows_toolchain

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=ROOT/'build/android-aot-cpu-float-test';out.mkdir(parents=True,exist_ok=True)
    config=json.loads((ROOT/'port.json').read_text());contract=json.loads((ROOT/'research/android-aot-cpu-sites.json').read_text())
    sites=[r for r in contract['sites'] if r['operation']>=6];assert len(sites)==19
    cases=out/'aot_cpu_float_cases.inc'
    cases.write_text('static const float_site float_cases[]={\n'+''.join('    {UINT64_C(%s),"%s",%d,%d,%d},\n'%(r['va'],r['instruction'],r['operation'],r['destination'],r['source']) for r in sites)+'};\n',encoding='utf-8',newline='\n')
    files=[ROOT/'android/host/aot_cpu_float.c',ROOT/'android/host/aot_cpu.h',ROOT/'tests/android_aot_cpu_float_fixture.c',ROOT/'tests/aot_cpu_sha256.h',ROOT/'native/runtime64/cpu64.h',ROOT/'native/windows/simd_float_ops.asm',ROOT/'research/android-aot-cpu-sites.json',Path(__file__).resolve(),cases]
    before={str(p.relative_to(ROOT)).replace('\\','/'):digest(p) for p in files}
    binaries,env=windows_toolchain();clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';commands=[]
    def run(command):
        commands.append(list(map(str,command)));p=subprocess.run(commands[-1],env=env,cwd=out,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180)
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        return p.stdout
    asm=out/'oracle.obj';run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(asm),ROOT/'native/windows/simd_float_ops.asm'])
    flags=['-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-Wall','-Wextra','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(out)]
    obj=out/'fixture.obj';run([clang,'--target=x86_64-pc-windows-msvc',*flags,'-c',ROOT/'tests/android_aot_cpu_float_fixture.c','-o',obj])
    host=out/'host-oracle.exe';run([binaries/'link.exe','/nologo','/OUT:'+str(host),obj,asm,'libcmt.lib','oldnames.lib','kernel32.lib'])
    result=json.loads(run([host]));assert result['success'] and result['native_full_CPU_cases']==291737
    golden=out/'aot_cpu_float_golden.h';golden.write_text('#define FP_AOT_FLOAT_GOLDEN "'+result['float_sha256']+'"\n',encoding='utf-8',newline='\n')
    arm=out/'aot-cpu-float-arm64';run([clang,'--target=aarch64-linux-android30',*flags,'-fPIE','-pie','-Wl,-z,max-page-size=16384',ROOT/'tests/android_aot_cpu_float_fixture.c','-o',arm])
    assert before=={p:digest(ROOT/p) for p in before};before[str(golden.relative_to(ROOT)).replace('\\','/')]=digest(golden)
    report=dict(success=True,phase='floating',source_hashes=before,host=dict(**result,executable_sha256=digest(host)),arm=dict(compiled=True,binary=str(arm.relative_to(ROOT)).replace('\\','/'),binary_sha256=digest(arm),executed=False),commands=commands,original_bytes_executed=False)
    target=ROOT/'research/android-aot-cpu-float-validation.json';target.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report['host'],indent=2));print(report['arm'])

if __name__=='__main__':main()
