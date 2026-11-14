"""Run the fastlift flag-helper differential fixture on a device.

    python tools/test_fastlift_flags.py --serial SERIAL [--rounds N]

Compares every helper of build/fastlift/include (derived from native/runtime64/cpu64.h) with the
canonical helper on random and edge operands, the signed-compare shortcut, the fast ADDSS/MULSS
forms and the volatile guest-memory accessors.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fastlift  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--rounds', type=int, default=2000000)
    args = ap.parse_args()
    out = ROOT / 'build/fastlift/test-flags'
    out.mkdir(parents=True, exist_ok=True)
    include = out / 'include'
    fastlift.write_headers(include)
    binary = out / 'fastlift-flags-arm64'
    clang = fastlift.toolchain() / 'clang.exe'
    command = [str(clang), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing', '-fwrapv',
               '-ffp-contract=off', '-fno-math-errno', '-Wl,-z,max-page-size=16384',
               '-I' + str(include), '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'),
               str(ROOT / 'tests/fastlift_flags_fixture.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    adb = ['adb', '-s', args.serial]
    remote = '/data/local/tmp/fastlift-flags-arm64'
    subprocess.run(adb + ['push', str(binary), remote], check=True, capture_output=True)
    try:
        p = subprocess.run(adb + ['shell', 'chmod 755 %s && %s %d' % (remote, remote, args.rounds)], capture_output=True, text=True)
        print(p.stdout.strip() + p.stderr.strip())
        raise SystemExit(0 if p.returncode == 0 and 'passed' in p.stdout else 1)
    finally:
        subprocess.run(adb + ['shell', 'rm -f ' + remote], capture_output=True)


if __name__ == '__main__':
    main()
