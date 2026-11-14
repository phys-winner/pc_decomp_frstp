"""Check native matrix math against the linked ARM64 object and the lifted host oracle."""
import argparse
import hashlib
import json
import re
import statistics
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from fastbuild import ROOT, BIN, CLANG, COMPILE_FLAGS, INCLUDES
from leaf_probe import windows_toolchain
from test_android_render_scale import function

OUT = ROOT / 'build/perf/cpu-gpu-20261112/native-matrix'
ORIGINAL = ROOT / 'build/arm64/eh-overlay-objects/recomp_funcs_0800.o'
ORIGINAL_SHA = '481ee5e180b54ad519a27834a6b75e09527b0756e92cfc11b79dd6a8b5c22585'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    assert sha(ORIGINAL) == ORIGINAL_SHA
    pins = {p.relative_to(ROOT).as_posix(): sha(p) for p in
            (ROOT / 'android/host/matrix_native.c', ROOT / 'tests/android_matrix_native_fixture.c',
             ROOT / 'native/runtime64/cpu64.h', Path(__file__).resolve())}
    commands = []
    bins, env = windows_toolchain()

    def run(command, timeout=120):
        command = list(map(str, command))
        commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
        if result.returncode:
            raise RuntimeError(result.stdout[-1500:] + result.stderr[-5000:])
        return result.stdout

    renamed = OUT / 'production-original.o'
    run([BIN / 'llvm-objcopy.exe', '--redefine-sym=L_000140DFA630=fp_original_L_000140DFA630', ORIGINAL, renamed])
    with ORIGINAL.open('rb') as f, renamed.open('rb') as g:
        old, new = ELFFile(f), ELFFile(g)
        assert old.get_section_by_name('.text').data() == new.get_section_by_name('.text').data()
        undefined = [s.name for s in old.get_section_by_name('.symtab').iter_symbols()
                     if s['st_shndx'] == 'SHN_UNDEF' and s.name]
    stubs = OUT / 'unreachable.c'
    stubs.write_text('#include <stdint.h>\n#include <stdlib.h>\nint64_t g_image_delta;\n' +
                     '\n'.join('void ' + name + '(void){abort();}' for name in undefined
                               if name not in ('g_image_delta', '_setjmp')) + '\n')
    generated = ROOT / 'build/arm64/generated-overlay/recomp_funcs_0800.c'
    oracle = OUT / 'host-original.c'
    oracle.write_text('#include "cpu64.h"\n' + function(generated.read_text(), '140DFA630').replace(
        'void L_000140DFA630(', 'void fp_original_L_000140DFA630(', 1))
    flags = ['-O2', '-std=c11', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-fno-math-errno',
             '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host')]
    sources = [ROOT / 'tests/android_matrix_native_fixture.c', ROOT / 'android/host/matrix_native.c']
    host_objects = []
    for i, source in enumerate([*sources, oracle]):
        obj = OUT / ('host-%d.obj' % i)
        run([CLANG, '--target=x86_64-pc-windows-msvc', *flags, '-fms-runtime-lib=static', '-c', source, '-o', obj])
        host_objects.append(obj)
    host = OUT / 'matrix-host.exe'
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(host), *host_objects,
         'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_result = json.loads(run([host]))
    objects = []
    for i, source in enumerate([*sources, stubs]):
        obj = OUT / ('arm-%d.o' % i)
        run([CLANG, *COMPILE_FLAGS, *['-I' + str(ROOT / p) for p in INCLUDES], '-c', source, '-o', obj])
        objects.append(obj)
    arm = OUT / 'matrix-arm64'
    run([CLANG, '--target=aarch64-linux-android30', *objects, renamed,
         '-Wl,-z,max-page-size=16384', '-lm', '-o', arm])
    adb = ['adb', '-s', args.serial]
    native_override = run([*adb, 'shell', 'getprop', 'debug.fp.native_matrix']).strip()
    assert not native_override.startswith('0'), 'Native matrix is disabled on the test device'
    remote = '/data/local/tmp/frostpunk-matrix-' + sha(arm)[:12]
    run([*adb, 'push', arm, remote])
    run([*adb, 'shell', 'chmod', '700', remote])
    device_result = json.loads(run([*adb, 'shell', remote]))
    benchmark = json.loads(run([*adb, 'shell', remote, '--benchmark']))
    run([*adb, 'shell', 'rm', '-f', remote])
    assert host_result['success'] and device_result['success']
    assert pins == {name: sha(ROOT / name) for name in pins}
    medians = {name: statistics.median(p[name] for p in benchmark['pairs']) for name in ('original_ns', 'native_ns')}
    result = dict(success=True, host=host_result, device=device_result, benchmark=benchmark, medians=medians,
                  native_override=native_override,
                  sources=pins, original_object_sha256=ORIGINAL_SHA, original_machine_code_unchanged=True,
                  arm_binary_sha256=sha(arm), reduction_pct=100 * (1 - medians['native_ns'] / medians['original_ns']))
    (OUT / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
