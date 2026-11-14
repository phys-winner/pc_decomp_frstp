"""Run the actual ZIP spans, Win64 kernel, CRT streams and input-size parser on ARM64."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from fastbuild import ROOT, CLANG, INCLUDES
from test_android_touch_controls import extract


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    out = ROOT / 'build/zip-files-test'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'android/app/app.c').read_text()
    (out / 'game-input-check.inc').write_text(extract(source, 'static int game_inputs_check_text('), encoding='utf-8')
    binary = out / 'fixture'
    inputs = ['tests/zip_files_fixture.c', 'android/host/win64_kernel.c', 'android/host/zip_files.c', 'reference/launcher-zipmount/zipmount.c']
    command = [str(CLANG), '--target=aarch64-linux-android30', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
               *['-I' + str(ROOT / name) for name in INCLUDES], '-I' + str(out),
               *[str(ROOT / name) for name in inputs], '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-lm', '-o', str(binary)]
    subprocess.run(command, check=True)
    adb = ['adb', '-s', args.serial]
    remote = '/data/local/tmp/fp-zip-files-' + hashlib.sha256(binary.read_bytes()).hexdigest()[:12]
    subprocess.run(adb + ['push', str(binary), remote], check=True, capture_output=True)
    try:
        subprocess.run(adb + ['shell', 'chmod', '700', remote], check=True)
        result = subprocess.run(adb + ['shell', remote], check=True, capture_output=True, text=True, timeout=90)
        report = json.loads(result.stdout)
        assert report['success']
        report['sources'] = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in inputs}
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(result.stdout)
    finally:
        subprocess.run(adb + ['shell', 'rm', '-f', remote], check=True)


if __name__ == '__main__':
    main()
