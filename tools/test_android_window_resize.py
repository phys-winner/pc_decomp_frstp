"""Verify the actual app window/resize functions on host and an explicitly selected ARM device."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from fastbuild import ROOT,CLANG
from leaf_probe import windows_toolchain

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--serial',required=True);args=parser.parse_args()
    out=ROOT/'build/android-window-resize-test';out.mkdir(exist_ok=True)
    source=ROOT/'android/app/app.c';fixture=ROOT/'tests/android_window_resize_fixture.c'
    text=source.read_text();begin=text.index('static void set_window(');end=text.index('\nstatic int write_all(',begin)
    assert text.count('resize_window(c,app->window);')==1
    include=out/'window_resize.inc';include.write_text(text[begin:end]+'\n',newline='\n')
    sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
    pins={str(p):sha(p) for p in [source,fixture,include,Path(__file__)]}
    bins,env=windows_toolchain()
    def run(command):
        result=subprocess.run(list(map(str,command)),env=env,capture_output=True,text=True,timeout=60)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        return result.stdout
    host=out/'window-resize.exe'
    run([bins/'cl.exe','/nologo','/std:c11','/O2','/DFP_GAME_RUNTIME=1','/I'+str(out),fixture,'/Fo'+str(out/'host.obj'),'/Fe'+str(host)])
    expected=json.loads(run([host]));assert expected['success']
    arm=out/'window-resize-arm64'
    run([CLANG,'--target=aarch64-linux-android30','-std=c11','-O2','-DFP_GAME_RUNTIME=1','-I'+str(out),fixture,'-Wl,-z,max-page-size=16384','-o',arm])
    adb=['adb','-s',args.serial];remote='/data/local/tmp/fp-window-resize-'+sha(arm)[:12]
    run([*adb,'push',arm,remote]);run([*adb,'shell','chmod','700',remote])
    try:device=json.loads(run([*adb,'shell',remote]))
    finally:run([*adb,'shell','rm','-f',remote])
    assert device==expected and pins=={p:sha(p) for p in pins}
    result=dict(success=True,host=expected,device=device,serial=args.serial,sources=pins)
    (out/'result.json').write_text(json.dumps(result,indent=1)+'\n');print(json.dumps(device))

if __name__=='__main__':main()
