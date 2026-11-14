"""Run tests/sync_condition_cs_fixture.c (SleepConditionVariableCS of android/host/win64_sync.c) on an ARM64 device.

    python -X utf8 tools/test_sync_condition_cs.py --serial SERIAL

Builds a static-free native executable from the real synchronization source with stand-in host
services, pushes it under its own name to /data/local/tmp, runs it and removes it again.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fastbuild as fb  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    args = ap.parse_args()
    out = ROOT / 'build/fastlift/test-sync'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'sync-condition-cs-arm64'
    command = [str(fb.CLANG), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing', '-pthread', '-w',
               *['-I' + str(ROOT / d) for d in ('native/runtime64', 'android/host', 'generated/frostpunk-x64', 'generated/win64')],
               str(ROOT / 'tests/sync_condition_cs_fixture.c'), '-Wl,-z,max-page-size=16384', '-o', str(binary)]
    done = subprocess.run(command, capture_output=True, text=True, timeout=180)
    if done.returncode:
        print(done.stderr[-4000:])
        raise SystemExit(1)
    remote = '/data/local/tmp/frostpunk-sync-cs-' + hashlib.sha256(binary.read_bytes()).hexdigest()[:12]
    adb = ['adb', '-s', args.serial]
    try:
        subprocess.run(adb + ['push', str(binary), remote], capture_output=True, check=True, timeout=60)
        subprocess.run(adb + ['shell', 'chmod', '700', remote], capture_output=True, check=True, timeout=30)
        run = subprocess.run(adb + ['shell', remote], capture_output=True, text=True, timeout=200)
    finally:
        subprocess.run(adb + ['shell', 'rm', '-f', remote], capture_output=True, timeout=30)
    if run.returncode:
        print('FAILED exit %d' % run.returncode, run.stdout[-500:], run.stderr[-1500:])
        raise SystemExit(1)
    row = json.loads(run.stdout.strip().splitlines()[-1])
    assert row['success'] and row['handoffs'] == 400000 and row['protected_increments'] == 80000, row
    print(json.dumps(row))


if __name__ == '__main__':
    main()
