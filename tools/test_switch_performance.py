"""Exercise every derived texture cap and the existing Mobile Rendering toggle on host/ARM."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from fastbuild import ROOT,CLANG
from leaf_probe import windows_toolchain
from build_port_settings_unit import EDITS
from test_android_touch_controls import extract
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--serial',required=True);args=p.parse_args()
    out=ROOT/'build/switch-performance-test';out.mkdir(exist_ok=True)
    report=ROOT/'research/switch-performance-textures.json';rows=json.loads(report.read_text())['entries']
    cases=out/'switch-cases.inc';cases.write_text('\n'.join('{%s,%u,%u,%u,%u},'%(json.dumps(r['name']),r['width'],r['height'],r['mips'],r['skip']) for r in rows)+'\n')
    fixture=ROOT/'tests/switch_performance_fixture.c';source=ROOT/'android/host/switch_performance.c'
    transaction=out/'mobile-transaction.inc';transaction.write_text(extract(EDITS[1][1],'if(R32(c->rbx)>=20u&&R32(c->rbx)<=31u)'))
    pins={str(x):sha(x) for x in [report,cases,transaction,fixture,source,ROOT/'android/host/switch_texture_caps.inc',ROOT/'android/host/switch_performance.h',ROOT/'tools/build_port_settings_unit.py']}
    bins,env=windows_toolchain()
    def run(cmd):
        r=subprocess.run(list(map(str,cmd)),env=env,capture_output=True,text=True,timeout=60)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        return r.stdout
    includes=[ROOT/'android/host',ROOT/'native/runtime64',out];host=out/'switch-performance.exe'
    run([bins/'cl.exe','/nologo','/std:c11','/O2',*['/I'+str(x) for x in includes],source,fixture,'/Fo'+str(out)+'\\','/Fe'+str(host)])
    expected=json.loads(run([host]));assert expected['success']
    arm=out/'switch-performance-arm64';run([CLANG,'--target=aarch64-linux-android30','-std=c11','-O2',*['-I'+str(x) for x in includes],source,fixture,'-lm','-Wl,-z,max-page-size=16384','-o',arm])
    adb=['adb','-s',args.serial];remote='/data/local/tmp/fp-switch-perf-'+sha(arm)[:12]
    run([*adb,'push',arm,remote]);run([*adb,'shell','chmod','700',remote])
    try:actual=json.loads(run([*adb,'shell',remote]))
    finally:run([*adb,'shell','rm','-f',remote])
    assert actual==expected and pins=={p:sha(p) for p in pins}
    (out/'result.json').write_text(json.dumps(dict(success=True,serial=args.serial,host=expected,device=actual,sources=pins),indent=1)+'\n');print(json.dumps(actual))
if __name__=='__main__':main()
