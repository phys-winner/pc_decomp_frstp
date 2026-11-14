"""Exact production type_info::name provider vs native VCRUNTIME data, no ADB."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from leaf_probe import ROOT, windows_toolchain
from test_android_rtti import ROUTE, PRODUCTION_ROUTE

OUT=ROOT/'build/audit-20261108/type-info-name'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    cfg=json.loads((ROOT/'port.json').read_text());OUT.mkdir(parents=True,exist_ok=True)
    core=(ROOT/'android/host/win64.c').read_text()
    assert core.count(PRODUCTION_ROUTE)==1 and '#define IMPORT_DONE do { *handled=step; c->rsp+=8; return 1; } while (0)' in core
    subprocess.run([sys.executable,str(ROOT/'tools/oracle_type_info_names.py')],check=True,capture_output=True,text=True)
    oracle=json.loads((OUT/'oracle.json').read_text());assert oracle['success'] and oracle['oracle']['cases']==11
    assert oracle['oracle']['ucrt_errno_preserved'] and oracle['oracle']['OOM_errno12_no_cache_or_list']
    assert sha(ROOT/'android/host/win64_rtti_names.inc')==oracle['generated_include_sha256']
    files=['android/host/win64_rtti.c','android/host/win64_rtti.h','android/host/win64_rtti_names.inc',
           'android/host/win64.h','android/host/win64_stdio.h','native/runtime64/cpu64.h','tests/type_info_name_fixture.c',
           'tests/type_info_name_oracle.c','tools/oracle_type_info_names.py','tools/test_type_info_names.py']
    pins={p:sha(ROOT/p) for p in files}
    (OUT/'rtti_route.inc').write_text(ROUTE+'\n')
    bins,env=windows_toolchain();commands=[]
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if result.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps(dict(command=command,returncode=result.returncode,stdout=result.stdout,stderr=result.stderr),indent=2)+'\n')
            raise RuntimeError(result.stdout+result.stderr)
        return result
    include=[ROOT/'native/runtime64',ROOT/'android/host',OUT];host=OUT/'type-info-name-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/experimental:c11atomics',
         *['/I'+str(p) for p in include],ROOT/'tests/type_info_name_fixture.c',ROOT/'android/host/win64_rtti.c',
         '/Fo'+str(OUT)+'/', '/Fe'+str(host)])
    host_run=run([host,OUT/'cases.bin']);host_proof=json.loads(host_run.stdout);assert host_proof['success']
    (OUT/'host.log').write_text(host_run.stdout+host_run.stderr)
    negatives=[]
    for name in ['unknown','unterminated','null_data','null_root','unaligned_root','reserved_root','readonly_cache','readonly_root']:
        result=run([host,OUT/'cases.bin',name],73);assert 'STOP Win64 RTTI' in result.stderr
        negatives.append(dict(arg=name,exit_code=73,stderr=result.stderr))
    ndk=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin';arm=OUT/'type-info-name-arm64'
    run([ndk/'clang.exe','--target=aarch64-linux-android30','-O2','-std=c11','-ffp-contract=off','-fno-strict-aliasing','-fwrapv',
         *['-I'+str(p) for p in include],ROOT/'tests/type_info_name_fixture.c',ROOT/'android/host/win64_rtti.c',
         '-Wl,-z,max-page-size=16384','-pthread','-o',arm])
    assert pins=={p:sha(ROOT/p) for p in files},'Owned source changed during proof'
    report=dict(success=True,sources=pins,native_oracle=oracle['oracle'],oracle_receipt_sha256=sha(OUT/'oracle.json'),
                original_exe_sha256=oracle['original_exe_sha256'],host=host_proof,negative_cases=negatives,
                data=dict(path=str((OUT/'cases.bin').relative_to(ROOT)),sha256=sha(OUT/'cases.bin')),
                arm=dict(path=str(arm.relative_to(ROOT)),sha256=sha(arm),args=['cases.bin']),commands=commands,
                integration=dict(production_route=PRODUCTION_ROUTE,single_pop_macro_verified=True),
                scope='All11 original uncached diagnostic-call types, oracle-derived names; native cache and SLIST16 chain/depth/sequence, cached invalid unused root, same-name distinct cache, concurrent shared cache publication. OOM behavior from shipped MSVC source; no generic demangler claim, unknown uncached types fail closed73.',
                original_bytes_executed=False,device_actions=False,arm_executed=False)
    (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(success=True,host=host_proof,arm=report['arm'],receipt_sha256=sha(OUT/'result.json')),indent=2))
if __name__=='__main__':main()
