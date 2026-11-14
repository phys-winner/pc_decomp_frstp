"""Run native control/state checks on an explicitly selected device."""
import argparse
import json
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial',required=True)
args=parser.parse_args()
config=json.loads((ROOT/'port.json').read_text())
clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
out=ROOT/'build/mobile-20261103';out.mkdir(exist_ok=True)
binary=out/'mobile-controls-test'
subprocess.run([str(clang),'--target=aarch64-linux-android30','-std=c11','-O2','-Wl,-z,max-page-size=16384',
    '-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),
    str(ROOT/'tests/mobile_controls_fixture.c'),str(ROOT/'android/host/mobile_controls.c'),'-lm','-o',str(binary)],check=True)
adb=['adb','-s',args.serial];remote='/data/local/tmp/fp-mobile-controls-test'
subprocess.run(adb+['push',str(binary),remote],check=True,capture_output=True)
try:
    subprocess.run(adb+['shell','chmod','755',remote],check=True)
    result=json.loads(subprocess.check_output(adb+['shell',remote]).decode())
    assert result['success']
    (out/'native-controls-test.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
finally:
    subprocess.run(adb+['shell','rm','-f',remote],check=True)
