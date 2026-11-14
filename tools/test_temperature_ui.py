"""Exercise the named temperature-only filter correction on host and ARM64."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from fastbuild import ROOT,CLANG
from leaf_probe import windows_toolchain

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--serial',required=True);args=p.parse_args()
    out=ROOT/'build/temperature-ui-test';out.mkdir(exist_ok=True)
    source=ROOT/'android/host/temperature_ui.c';fixture=ROOT/'tests/temperature_ui_fixture.c'
    sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
    pins={str(p):sha(p) for p in [source,fixture,ROOT/'android/host/temperature_ui.h',ROOT/'tools/build_mobile_controls.py',Path(__file__)]}
    bins,env=windows_toolchain()
    def run(cmd):
        r=subprocess.run(list(map(str,cmd)),env=env,capture_output=True,text=True,timeout=60)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        return r.stdout
    includes=[ROOT/'native/runtime64',ROOT/'android/host'];host=out/'temperature-ui.exe'
    run([bins/'cl.exe','/nologo','/std:c11','/O2',*['/I'+str(p) for p in includes],source,fixture,'/Fo'+str(out)+'\\','/Fe'+str(host)])
    expected=json.loads(run([host]));assert expected['success']
    arm=out/'temperature-ui-arm64';run([CLANG,'--target=aarch64-linux-android30','-std=c11','-O2',*['-I'+str(p) for p in includes],source,fixture,'-Wl,-z,max-page-size=16384','-o',arm])
    adb=['adb','-s',args.serial];remote='/data/local/tmp/fp-temperature-ui-'+sha(arm)[:12]
    run([*adb,'push',arm,remote]);run([*adb,'shell','chmod','700',remote])
    try:actual=json.loads(run([*adb,'shell',remote]))
    finally:run([*adb,'shell','rm','-f',remote])
    assert actual==expected and pins=={p:sha(p) for p in pins}
    (out/'result.json').write_text(json.dumps(dict(success=True,serial=args.serial,host=expected,device=actual,sources=pins),indent=1)+'\n');print(json.dumps(actual))
if __name__=='__main__':main()
