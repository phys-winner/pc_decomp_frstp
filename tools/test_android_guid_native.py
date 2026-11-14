"""Compare native reverse GUID comparison with the exact linked ARM64 object."""
import argparse
import hashlib
import json
import statistics
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from fastbuild import ROOT, BIN, CLANG, COMPILE_FLAGS, INCLUDES
from leaf_probe import windows_toolchain
from test_android_render_scale import function

OUT = ROOT / 'build/perf/fps45-20261112/native-guid'
ORIGINAL = ROOT / 'build/fast/mobile-objects/recomp_funcs_0803.o'
ORIGINAL_SHA = '2141510886adf095b220450778cda978b9c900ef64ef13c2b6f0500048b264ec'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    assert sha(ORIGINAL) == ORIGINAL_SHA
    pins = {p.relative_to(ROOT).as_posix(): sha(p) for p in
            (ROOT / 'android/host/guid_native.c', ROOT / 'tests/android_guid_native_fixture.c',
             ROOT / 'native/runtime64/cpu64.h', Path(__file__).resolve())}
    bins, env = windows_toolchain()

    def run(command, timeout=120):
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, env=env, timeout=timeout)
        if result.returncode:
            raise RuntimeError(result.stdout[-1500:] + result.stderr[-5000:])
        return result.stdout

    renamed = OUT / 'production-original.o'
    run([BIN / 'llvm-objcopy.exe', '--redefine-sym=L_000140E1E6C0=fp_original_L_000140E1E6C0', ORIGINAL, renamed])
    with ORIGINAL.open('rb') as f, renamed.open('rb') as g:
        old, new = ELFFile(f), ELFFile(g)
        assert old.get_section_by_name('.text').data() == new.get_section_by_name('.text').data()
        undefined = [s.name for s in old.get_section_by_name('.symtab').iter_symbols()
                     if s['st_shndx'] == 'SHN_UNDEF' and s.name]
    stubs = OUT / 'unreachable.c'
    stubs.write_text('#include <stdint.h>\n#include <stdlib.h>\nint64_t g_image_delta;\n' +
                     '\n'.join('void ' + name + '(void){abort();}' for name in undefined
                               if name not in ('g_image_delta', '_setjmp')) + '\n')
    generated = ROOT / 'generated/frostpunk-x64/recomp_funcs_0803.c'
    oracle = OUT / 'host-original.c'
    oracle.write_text('#include "cpu64.h"\n' + function(generated.read_text(), '140E1E6C0').replace(
        'void L_000140E1E6C0(', 'void fp_original_L_000140E1E6C0(', 1))
    flags = ['-O2', '-std=c11', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-fno-math-errno',
             '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host')]
    sources = [ROOT / 'tests/android_guid_native_fixture.c', ROOT / 'android/host/guid_native.c']
    host_objects = []
    for i, source in enumerate([*sources, oracle]):
        obj = OUT / ('host-%d.obj' % i)
        run([CLANG, '--target=x86_64-pc-windows-msvc', *flags, '-fms-runtime-lib=static', '-c', source, '-o', obj])
        host_objects.append(obj)
    host = OUT / 'guid-host.exe'
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(host), *host_objects,
         'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_result = json.loads(run([host]))
    assert host_result['success']
    (OUT / 'host-result.json').write_text(json.dumps(host_result, indent=1) + '\n')
    objects = []
    for i, source in enumerate([*sources, stubs]):
        obj = OUT / ('arm-%d.o' % i)
        run([CLANG, *COMPILE_FLAGS, *['-I' + str(ROOT / p) for p in INCLUDES], '-c', source, '-o', obj])
        objects.append(obj)
    arm = OUT / 'guid-arm64'
    run([CLANG, '--target=aarch64-linux-android30', *objects, renamed,
         '-Wl,-z,max-page-size=16384', '-lm', '-o', arm])
    if args.compile_only:
        print(json.dumps(dict(host=host_result, arm_binary_sha256=sha(arm)), indent=1))
        return
    adb = ['adb', '-s', args.serial]
    native_override = run([*adb, 'shell', 'getprop', 'debug.fp.native_guid']).strip()
    assert not native_override.startswith('0'), 'Native GUID comparison is disabled on the test device'
    remote = '/data/local/tmp/frostpunk-guid-' + sha(arm)[:12]
    run([*adb, 'push', arm, remote])
    run([*adb, 'shell', 'chmod', '700', remote])
    try:
        device_result = json.loads(run([*adb, 'shell', remote]))
        benchmark = json.loads(run([*adb, 'shell', remote, '--benchmark']))
    finally:
        run([*adb, 'shell', 'rm', '-f', remote])
    assert device_result['success']
    assert pins == {name: sha(ROOT / name) for name in pins}
    medians = {str(kind): {name: statistics.median(p[name] for p in benchmark['pairs'] if p['kind'] == kind)
                          for name in ('original_ns', 'native_ns')} for kind in range(3)}
    result = dict(success=True, host=host_result, device=device_result, benchmark=benchmark, medians=medians,
                  native_override=native_override, sources=pins, original_object_sha256=ORIGINAL_SHA,
                  original_machine_code_unchanged=True, arm_binary_sha256=sha(arm),
                  reduction_pct={kind: 100 * (1 - row['native_ns'] / row['original_ns']) for kind, row in medians.items()})
    (OUT / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
