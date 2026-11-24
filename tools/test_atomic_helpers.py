"""Device stress test of cpu64.h's locked read-modify-write helpers (tests/atomic_helpers_stress.c).

    python -X utf8 tools/test_atomic_helpers.py --serial SERIAL

Builds the test twice for arm64 Android: with the helpers (must print ATOMIC_STRESS PASS) and with the
old plain lowering of lock add (negative control, must FAIL: the harness sees lost updates).
"""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    args = ap.parse_args()
    config = json.loads((ROOT / 'port.json').read_text())
    clang = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out = ROOT / 'build/test-atomic-helpers'
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for name, extra in (('atomic', []), ('plain', ['-DSTRESS_PLAIN=1'])):
        binary = out / ('atomic_stress_' + name)
        subprocess.run([str(clang), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fPIE', '-pie', '-pthread', '-w',
                        '-I' + str(ROOT / 'native/runtime64'), *extra, str(ROOT / 'tests/atomic_helpers_stress.c'), '-o', str(binary)],
                       check=True)
        remote = '/data/local/tmp/fp_atomic_stress_' + name
        subprocess.run(['adb', '-s', args.serial, 'push', str(binary), remote], check=True, capture_output=True)
        done = subprocess.run(['adb', '-s', args.serial, 'shell', 'chmod 755 %s && %s; echo EXIT=$?; rm -f %s' % (remote, remote, remote)],
                              capture_output=True, text=True, timeout=600)
        print('--- %s\n%s' % (name, done.stdout.strip()))
        results[name] = 'ATOMIC_STRESS PASS' in done.stdout
    ok = results['atomic'] and not results['plain']
    print('test_atomic_helpers: helpers %s, plain control %s -> %s' % (
        'PASS' if results['atomic'] else 'FAIL', 'FAILED as expected' if not results['plain'] else 'PASSED (harness too weak)',
        'OK' if ok else 'NOT OK'))
    raise SystemExit(0 if ok else 1)


if __name__ == '__main__':
    main()
