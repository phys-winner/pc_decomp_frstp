"""Build the production calendar/pause oracle; run only with an explicit device."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args = parser.parse_args()
    config = json.loads((ROOT / 'port.json').read_text())
    sdk = Path(config['android_sdk'])
    clang = sdk / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out = ROOT / 'build/audit-20261108'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'lifecycle-time-arm64'
    subprocess.run([str(clang), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    '-I' + str(ROOT / 'native/runtime64'), str(ROOT / 'tests/android_lifecycle_time_fixture.c'),
                    '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-o', str(binary)], check=True)
    result = {'compiled': True, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'device_executed': False}
    if args.serial:
        adb = [str(sdk / 'platform-tools/adb.exe'), '-s', args.serial]
        remote = '/data/local/tmp/frostpunk-lifecycle-time-oracle'
        subprocess.run(adb + ['push', str(binary), remote], capture_output=True, check=True)
        subprocess.run(adb + ['shell', 'chmod', '700', remote], capture_output=True, check=True)
        run = subprocess.run(adb + ['shell', remote], capture_output=True, text=True, timeout=30, check=True)
        result.update(device_executed=True, oracle=json.loads(run.stdout))
        assert result['oracle']['success']
    (out / 'lifecycle-time-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
