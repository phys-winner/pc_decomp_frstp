"""Run tests/openal_reconnect_fixture.c (output reconnection of android/host/win64_openal.c) on an ARM64 device.

    python -X utf8 tools/test_openal_reconnect.py --serial SERIAL [--source PATH]

Builds the real bridge source with the production warning flags, the fixture and a stand-in OpenAL
library whose connection state the fixture controls, pushes them into their own directory under
/data/local/tmp, runs the fixture and removes the directory again.  --source tests another bridge
source (the pinned original must fail: it never reopens a disconnected output).
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

INCLUDES = ('native/runtime64', 'android/host', 'android/audio', 'reference/openal-soft/source/include', 'build/openal-bridge')


def compile_c(arguments):
    done = subprocess.run([str(fb.CLANG), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                           '-Wno-misleading-indentation', '-Wno-sign-compare', *['-I' + str(ROOT / d) for d in INCLUDES], *arguments],
                          capture_output=True, text=True, timeout=180)
    if done.returncode:
        print(done.stderr[-4000:])
        raise SystemExit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--source', default=str(ROOT / 'android/host/win64_openal.c'), help='bridge source under test')
    args = ap.parse_args()
    out = ROOT / 'build/fastlift/test-openal-reconnect'
    out.mkdir(parents=True, exist_ok=True)
    fixture, fake = out / 'openal-reconnect-arm64', out / 'libopenal-reconnect-fake.so'
    compile_c(['-fPIC', '-shared', str(ROOT / 'tests/openal_reconnect_fake.c'), '-Wl,-z,max-page-size=16384', '-o', str(fake)])
    compile_c([args.source, str(ROOT / 'tests/openal_reconnect_fixture.c'), '-ldl', '-pthread', '-Wl,-z,max-page-size=16384', '-o', str(fixture)])
    digest = hashlib.sha256(fixture.read_bytes() + fake.read_bytes()).hexdigest()[:12]
    remote = '/data/local/tmp/frostpunk-openal-reconnect-' + digest
    adb = ['adb', '-s', args.serial]
    try:
        subprocess.run(adb + ['shell', 'mkdir', '-p', remote + '/profile'], capture_output=True, check=True, timeout=30)
        for path in (fixture, fake):
            subprocess.run(adb + ['push', str(path), remote + '/' + path.name], capture_output=True, check=True, timeout=60)
        subprocess.run(adb + ['shell', 'chmod', '700', remote + '/' + fixture.name], capture_output=True, check=True, timeout=30)
        run = subprocess.run(adb + ['shell', '%s/%s %s/%s %s/profile' % (remote, fixture.name, remote, fake.name, remote)],
                             capture_output=True, text=True, timeout=120)
    finally:
        subprocess.run(adb + ['shell', 'rm', '-rf', remote], capture_output=True, timeout=30)
    lines = run.stdout.strip().splitlines()
    row = json.loads(lines[-1]) if lines and lines[-1].startswith('{') else {}
    reconnect_lines = [line for line in (run.stdout + run.stderr).splitlines() if line.startswith('OPENAL_RECONNECT')]
    if run.returncode or not row.get('success'):
        print('FAILED exit %d' % run.returncode, json.dumps(row), run.stderr[-1500:])
        raise SystemExit(1)
    assert row['reopens'] == 5 and row['hold_requests'] == 1 and len(reconnect_lines) == 5, (row, reconnect_lines)
    print(json.dumps(dict(row, reconnect_log_lines=len(reconnect_lines))))


if __name__ == '__main__':
    main()
