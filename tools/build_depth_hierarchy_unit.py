"""Use the allocated scene size in both depth hierarchy passes, then register unit 0836."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import generate_android_direct_dispatch as direct
from build_port_settings_unit import FLAGS
from fastbuild import CLANG, INPUTS, ROOT

SOURCE = ROOT / 'build/arm64/generated-overlay/recomp_funcs_0836.c'
ORIGINAL = ROOT / 'build/arm64/eh-overlay-objects/recomp_funcs_0836.o'
SOURCE_SHA = '1e5a731bbf8c1fbecaec48c9314d99ec860a8d24f953fe1101447f00d9528817'
ORIGINAL_SHA = '8d74d3a94e458977ded9d0eed9b5b795898abd1b6593acf22511700786b10155'
OUT = ROOT / 'build/fast/depth-hierarchy'
FUNCTIONS = ('L_000141086940', 'L_000141086C30')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def body(text, name):
    start = text.index('void ' + name + '(CPU *c)\n{')
    return text[start:text.index('\n}\n', start) + 3]


def repair(text):
    for name, expected in zip(FUNCTIONS, (4, 1)):
        old = body(text, name)
        assert old.count('GVA(0x142983064)') + old.count('GVA(0x142983068)') == expected
        # 140F7AE10 allocates the hierarchy from Renderer+0xb4; the output viewport can be larger.
        new = old.replace('GVA(0x142983064)', 'rd64(GVA(0x142982A70))+0xb4')
        new = new.replace('GVA(0x142983068)', 'rd64(GVA(0x142982A70))+0xb8')
        text = text.replace(old, new, 1)
    return text


def compile_unit(text, folder):
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / SOURCE.name
    source.write_text(text, encoding='utf-8', newline='\n')
    target = source.with_suffix('.o')
    subprocess.run([str(CLANG), *FLAGS, '-c', str(source), '-o', str(target)], check=True)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    assert sha(SOURCE) == SOURCE_SHA and sha(ORIGINAL) == ORIGINAL_SHA, 'Depth hierarchy input drift'
    table = ROOT / 'generated/frostpunk-x64/recomp_dispatch.c'
    header = ROOT / 'generated/frostpunk-x64/recomp_funcs.h'
    production, _ = direct.rewrite_unit(SOURCE.read_text(), direct.binding_table(table.read_text(), header.read_text()))
    assert sha(compile_unit(production, OUT / 'baseline')) == ORIGINAL_SHA, 'Baseline object differs'
    fixed = repair(production)
    # Exercise the actual translated bodies and contrast them with the original failing bodies.
    for label, text in [('baseline', production), ('fixed', fixed)]:
        folder = OUT / label
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / 'depth-bodies.inc'
        bodies = '\n'.join(body(text, name) for name in FUNCTIONS)
        bodies = direct.NEW_CALL.sub(lambda match: 'dispatch(c, ' + match.group(1) + ')', bodies)
        path.write_text(bodies, encoding='utf-8', newline='\n')
        binary = OUT / label / 'depth-test'
        subprocess.run([str(CLANG), '--target=aarch64-linux-android30', '-O2', '-std=c11',
                        '-I' + str(ROOT / 'native/runtime64'), '-I' + str(path.parent),
                        str(ROOT / 'tests/depth_hierarchy_fixture.c'), '-lm',
                        '-Wl,-z,max-page-size=16384', '-o', str(binary)], check=True)
        adb = ['adb', '-s', args.serial]
        remote = '/data/local/tmp/fp-depth-hierarchy-test'
        subprocess.run(adb + ['push', str(binary), remote], check=True, capture_output=True)
        try:
            subprocess.run(adb + ['shell', 'chmod', '755', remote], check=True)
            result = json.loads(subprocess.check_output(adb + ['shell', remote, label]))
            assert result['success']
            (path.parent / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        finally:
            subprocess.run(adb + ['shell', 'rm', '-f', remote], check=True)
    obj = compile_unit(fixed, OUT)
    before = INPUTS.read_bytes()
    data = json.loads(before)
    old, new = ORIGINAL.relative_to(ROOT).as_posix(), obj.relative_to(ROOT).as_posix()
    assert sum(row in (old, new) for row in data['objects']) == 1
    data['objects'] = [new if row == old else row for row in data['objects']]
    assert INPUTS.read_bytes() == before, 'Runtime inputs changed concurrently'
    INPUTS.write_text(json.dumps(data, indent=1) + '\n')
    result = dict(success=True, device=args.serial, source_sha256=SOURCE_SHA,
                  baseline_object_identical=True, object_sha256=sha(obj), changed_extent_reads=5,
                  fixture_sha256=sha(ROOT / 'tests/depth_hierarchy_fixture.c'))
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
