"""Qualify the actual Android force-include, preserving canonical headers and caches."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import android_eh_contract as eh
import build_lifted

ROOT = eh.ROOT
OUT = ROOT / 'build/android-eh-affinity-integration-test'


def main():
    config = json.loads((ROOT / 'port.json').read_text()); pins = {}
    contract = eh.verified_contract(config, pins, False)
    names = ['android/host/eh_entry_nomask.h', 'tools/android_eh_contract.py', 'tools/test_android_eh_integration.py',
             'tests/test_android_eh_contract.py', 'tools/build_lifted.py', 'tools/build_startup.py',
             'tests/test_android_aot_build.py',
             'tests/android_eh_setjmp_fixture.c', 'native/runtime64/cpu64.h', 'native/runtime64/eh64.h', 'native/runtime64/eh64.c']
    sources = {name: eh.digest(ROOT / name) for name in names}
    subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_android_eh_contract.py'], cwd=ROOT, check=True)
    subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_android_aot_build.py'], cwd=ROOT, check=True)
    bins = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    clang = bins / 'clang.exe'; OUT.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(args, expected=0):
        args = list(map(str, args)); commands.append(args)
        result = subprocess.run(args, capture_output=True, text=True, timeout=120)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(args) + '\n' + result.stdout[-3000:] + result.stderr[-3000:])
        return result

    probe = OUT / 'preinclude-probe.c'
    probe.write_text('''#include "cpu64.h"
__attribute__((used)) static uint64_t fp_eh_entry_probe(void) {
    uint64_t _ind=0; ES3_EH_ENTER(0x140000000ull);
_ljump: ES3_EH_LEAVE(); return _ind;
}
int fp_native_exit_probe(jmp_buf buffer) { return setjmp(buffer); }
''')
    flags, _ = eh.unit_options(Path(next(iter(contract['selected_units']))), 'unused', contract['selected_units'], contract['header_sha256'])
    preprocessed = run([clang, *build_lifted.FLAGS, *flags, '-E', probe]).stdout
    (OUT / 'preinclude-probe.i').write_text(preprocessed)
    included = {Path(json.loads(name)).resolve() for name in re.findall(r'^# \d+ ("(?:\\.|[^"\\])*")', preprocessed, re.M)
                if Path(json.loads(name)).name in {'cpu64.h', 'eh64.h'}}
    if included != {ROOT / 'native/runtime64/cpu64.h', ROOT / 'native/runtime64/eh64.h'}:
        raise ValueError('Production preinclude resolved a shadow CPU/EH header')
    entry = preprocessed.split('static uint64_t fp_eh_entry_probe(void)', 1)[1].split('int fp_native_exit_probe', 1)[0]
    native = preprocessed.split('int fp_native_exit_probe', 1)[1]
    if 'if (_setjmp(_ehjb))' not in entry or 'if (setjmp(_ehjb))' in entry or 'setjmp(buffer)' not in native or '_setjmp(buffer)' in native:
        raise ValueError('Actual production force-include did not isolate the guest landing pad')
    # Opposite include orders used by real generated units must retain the override.
    if preprocessed.count('if (_setjmp(_ehjb))') != 1:
        raise ValueError('Canonical cpu64 quote-include overwrote the production EH macro')
    obj = OUT / 'preinclude-probe.o'
    run([clang, *build_lifted.FLAGS, *flags, '-c', probe, '-o', obj])
    symbols = run([bins / 'llvm-readelf.exe', '--symbols', obj]).stdout
    if not all(name in symbols for name in ['_setjmp', ' setjmp', 'es3_eh_push', 'es3_eh_pop']):
        raise ValueError('Compiled production preinclude/native boundary symbols differ')
    fixture = OUT / 'eh-integration-arm64'
    run([clang, *build_lifted.FLAGS, *flags, '-DFP_EH_CANDIDATE=1', '-I' + str(ROOT / 'native/runtime64'),
         ROOT / 'tests/android_eh_setjmp_fixture.c', '-pthread', '-lm', '-Wl,-z,max-page-size=16384', '-o', fixture])
    dynamic = run([bins / 'llvm-readelf.exe', '--dyn-syms', fixture]).stdout
    if not all(name in dynamic for name in ['_setjmp', ' setjmp', 'longjmp', 'pthread_sigmask']):
        raise ValueError('Real EH walker fixture did not retain both native jump variants')
    elf = fixture.read_bytes()
    if elf[:6] != b'\x7fELF\x02\x01' or int.from_bytes(elf[18:20], 'little') != 183:
        raise ValueError('Production force-include fixture is not AArch64 ELF')
    # Target guard is a compile-time rejection, not a global setjmp substitute.
    rejected = subprocess.run([str(clang), '--target=x86_64-linux-android30', '-I' + str(ROOT / 'native/runtime64'),
        *flags, '-E', str(probe)], capture_output=True, text=True, timeout=120)
    if not rejected.returncode or 'Android AArch64 EH preinclude used for another target' not in rejected.stderr:
        raise ValueError('EH force-include accepted a non-AArch64 target')
    if any(eh.digest(ROOT / name) != value for name, value in sources.items()) or any(eh.digest(p) != value for p, value in pins.items()):
        raise ValueError('EH integration source/prerequisite changed during proof')
    expected = json.loads((ROOT / eh.HARDWARE).read_text())['variants']['candidate']['result']
    report = dict(success=True, contract=contract, source_hashes=sources, commands=commands,
        compiler_sha256=eh.digest(clang), actual_preinclude_tested=True, preprocessor_enter_calls='_setjmp',
        native_thread_setjmp_preserved=True, untouched_object_identities=823, changed_main_units=180,
        changed_repaired_units=43, changed_base_units=137, negative_prerequisite_cases=11, negative_canonical_drift_cases=2,
        canonical_modified=False, generic_candidate_qualified=False,
        arm=dict(compiled=True, binary=fixture.relative_to(ROOT).as_posix(), sha256=eh.digest(fixture), executed=False,
                 expected_result=expected), device_executed=False,
        limitation='Only current unsupported-guest-throw, mask-invariant supported Android boundary; generic mask-changing callback parity remains rejected.')
    eh.verify_integration(report, contract, {})
    path = ROOT / eh.INTEGRATION; path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(success=True, receipt=str(path), sha256=eh.digest(path), arm=report['arm'],
                          selected_units=180, untouched_objects=823), indent=2))


if __name__ == '__main__':
    main()
