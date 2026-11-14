"""Run the law/photo geometry regression on one explicitly selected Android device."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    config = json.loads((ROOT / 'port.json').read_text())
    clang = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    output = ROOT / 'build/mobile-20261108/law-photo-fixture'
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(clang), '--target=aarch64-linux-android30', '-std=c11', '-O2',
                    '-Wl,-z,max-page-size=16384', str(ROOT / 'tests/law_photo_layout_fixture.c'),
                    '-lm', '-o', str(output)], check=True)
    adb = ['adb', '-s', args.serial]
    remote = '/data/local/tmp/fp-law-photo-fixture'
    subprocess.run(adb + ['push', str(output), remote], check=True, capture_output=True)
    subprocess.run(adb + ['shell', 'chmod', '755', remote], check=True)
    result = subprocess.run(adb + ['shell', remote], check=True, capture_output=True, text=True)
    receipt = dict(success=True, fixture='tests/law_photo_layout_fixture.c', output=result.stdout.strip())
    output.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
