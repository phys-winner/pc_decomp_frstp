"""Check the complete ContainsEntity body and its bitmap boundary on a selected ARM64 device."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import fastlift

ROOT = Path(__file__).resolve().parents[1]


def body(text):
    start = text.index('void L_000140DABBF0(CPU *c)')
    end = text.index('\nvoid ', start + 1)
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    out = ROOT / 'build/android-entity-membership-test'
    out.mkdir(exist_ok=True)
    source = fastlift.unit_path(795)
    original = source.read_text(encoding='utf-8')
    patched = fastlift.entity_membership_patch(original)
    assert patched == fastlift.unit_text(795)
    _, chunks = fastlift.parse_unit(patched, 795)
    assert not next(c for c in chunks if c.va == 0x140DABBF0).foreign
    for text in (original.replace('c->r8 + c->rax*4', 'c->r8 + c->rax*8'), patched):
        try:
            fastlift.entity_membership_patch(text)
        except ValueError:
            pass
        else:
            raise AssertionError('Changed or duplicate overlay input accepted')
    for name, text in [('baseline', original), ('candidate', patched)]:
        result = body(text).replace('L_000140DABBF0(CPU', 'membership_' + name + '(CPU', 1)
        (out / ('membership_' + name + '.inc')).write_text(result, encoding='utf-8')
    binary = out / 'entity-membership-arm64'
    subprocess.run([str(fastlift.toolchain() / 'clang.exe'), '--target=aarch64-linux-android30',
                    '-std=c11', '-O2', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off',
                    '-Wl,-z,max-page-size=16384', '-I' + str(ROOT / 'native/runtime64'), '-I' + str(out),
                    str(ROOT / 'tests/android_entity_membership_fixture.c'), '-o', str(binary)], check=True)
    adb = ['adb', '-s', args.serial]
    remote = '/data/local/tmp/fp-entity-membership-test'
    subprocess.run(adb + ['push', str(binary), remote], check=True, capture_output=True)
    try:
        subprocess.run(adb + ['shell', 'chmod', '700', remote], check=True)
        result = json.loads(subprocess.check_output(adb + ['shell', remote], text=True, timeout=60))
        assert result['success'] and result['checks'] == 262159
        result['inputs'] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in [source, ROOT / 'tools/fastlift.py', ROOT / 'tests/android_entity_membership_fixture.c']}
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    finally:
        subprocess.run(adb + ['shell', 'rm', '-f', remote], check=True, capture_output=True)


if __name__ == '__main__':
    main()
