"""Compile the exact production app report() with bounded mock IO; never ADB."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from leaf_probe import ROOT,windows_toolchain


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    output=ROOT/'build/app-game-report-test';output.mkdir(exist_ok=True)
    app=ROOT/'android/app/app.c';fixture=ROOT/'tests/app_game_report_fixture.c'
    pins=[app,fixture,Path(__file__).resolve()];before={str(p.relative_to(ROOT)):sha(p) for p in pins}
    text=app.read_text()
    if len(re.findall(r'char reported_game_state\[512\];',text))!=1:
        raise ValueError('Production report cache capacity changed')
    start=text.index('static void report(app_context *c) {')
    end=text.index('\n#ifdef FP_GAME_RUNTIME\nstatic int verify_game_executable(',start)
    extracted=output/'app_report.inc';extracted.write_text(text[start:end],newline='\n')
    bins,env=windows_toolchain();config=json.loads((ROOT/'port.json').read_text())
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    commands=[]
    def run(args):
        command=list(map(str,args));commands.append(command)
        p=subprocess.run(command,env=env,cwd=output,capture_output=True,text=True,timeout=45)
        if p.returncode:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr)
        return p.stdout
    exe=output/'app-report-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/std:c11','/W4','/DFP_REPORT_CACHE_BYTES=512','/I'+str(output),
         fixture,'/Fo'+str(output)+'/', '/Fe'+str(exe)])
    host=json.loads(run([exe]))
    if (host['success'] is not True or host['duplicate_reports']!=10000 or host['duplicate_new_writes']!=0
            or host['IO_failures_then_retry']!=4 or host['format_failures_then_retry']!=2
            or host['changed_fields']!=8 or not host['maximum_JSON_bytes']<512):
        raise ValueError('Incomplete report fixture')
    state=host['maximum_state']
    expected={'kind':'static-recompiled-original-game-worker','host_abi':1,'runtime_loaded':False,
              'worker_started':False,'worker_finished':False,'worker_result':-2147483648,'real_window':False,
              'window_generation':18446744073709551615,'queue_overflow':False,'game_boot_proven':False}
    if state!=expected:raise ValueError('Serialized production JSON differs at maximum field values')
    arm=output/'app-report-arm64'
    run([clang,'--target=aarch64-linux-android30','-O2','-std=c11','-fPIE','-pie','-DFP_REPORT_CACHE_BYTES=512',
         '-I'+str(output),fixture,'-Wl,-z,max-page-size=16384','-o',arm])
    if before!={str(p.relative_to(ROOT)):sha(p) for p in pins}:raise ValueError('Report source drift during test')
    report={'success':True,'source_sha256':before,'exact_report_extracted_sha256':sha(extracted),
            'host':host,'host_binary_sha256':sha(exe),'ARM':{'path':str(arm.relative_to(ROOT)),
            'sha256':sha(arm),'args':[],'executed':False},'commands':commands,'device_actions':False}
    path=output/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'host':host,'ARM':report['ARM'],'receipt':str(path.relative_to(ROOT)),
                      'receipt_sha256':sha(path)},indent=2))


if __name__=='__main__':main()
