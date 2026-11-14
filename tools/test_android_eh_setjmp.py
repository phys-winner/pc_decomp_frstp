"""Isolated real EH walker candidate proof; no production edits, AOT build, or ADB."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-eh-affinity-setjmp-test'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'port.json').read_text())
    files = ['native/runtime64/cpu64.h', 'native/runtime64/eh64.h', 'native/runtime64/eh64.c',
             'android/host/bootstrap.c', 'android/host/win64_threads.c',
             'tests/android_eh_setjmp_fixture.c', 'tools/test_android_eh_setjmp.py', 'port.json',
             'tools/build_lifted.py', 'tools/build_startup.py', 'tools/lift_dependency.py',
             'generated/frostpunk-x64/recomp_imports.c',
             'generated/dependencies/msvcp140/recomp/recomp_imports.c']
    files += [str(p.relative_to(ROOT)) for p in (ROOT / 'native/runtime64').glob('*.h') if str(p.relative_to(ROOT)) not in files]
    # An unsupported guest C++ throw must not acquire a new nonlocal-jump path
    # unnoticed. This audit covers the complete project-owned Android boundary,
    # not arbitrary external library code or future APIs.
    boundary_paths = sorted(p for folder in ['android/host', 'android/dxvk', 'android/steam', 'android/app']
                            for p in (ROOT / folder).rglob('*') if p.suffix in {'.c', '.h', '.cpp'})
    files += [str(p.relative_to(ROOT)) for p in boundary_paths if str(p.relative_to(ROOT)) not in files]
    before = {name: sha(ROOT / name) for name in files}
    boundary_source = {str(p.relative_to(ROOT)): re.sub(r'/\*.*?\*/|//[^\n]*', '', p.read_text(), flags=re.S)
                       for p in boundary_paths}
    forbidden = re.compile(r'\b(?:es3_eh_throw|sigaction|sigprocmask|pthread_sigmask)\s*\(')
    if any(forbidden.search(code) for code in boundary_source.values()):
        raise ValueError('Android guest-throw/signal-mask prerequisite changed; manual review required')
    if any(re.search(r'"_+CxxThrowException"', code) for code in boundary_source.values()):
        raise ValueError('A project-owned C++ throw import route appeared; candidate must be requalified')
    core = (ROOT / 'android/host/win64.c').read_text()
    bootstrap = (ROOT / 'android/host/bootstrap.c').read_text()
    dependencies = (ROOT / 'android/host/dependencies.c').read_text()
    if 'fail(70, "unimplemented import %s!%s' not in bootstrap or 'STOP unimplemented dependency import %s!%s' not in dependencies:
        raise ValueError('Main/dependency unsupported-import stop boundary changed')
    raise_guard = 'if((uint32_t)cpu->rcx!=UINT32_C(0x406d1388))return 0;'
    if 'if (kernel && !strcmp(name,"RaiseException") && win64_threads_raise_exception(c))' not in core:
        raise ValueError('RaiseException routing changed')
    header = (ROOT / 'native/runtime64/eh64.h').read_text()
    old = '    if (setjmp(_ehjb)) { _ind = _ehf.resume; goto _ljump; }'
    if header.count(old) != 1:
        raise ValueError('Exact canonical EH landing-pad preimage changed')
    # No sigsetjmp cast: Bionic jmp_buf/sigjmp_buf have different declared sizes.
    candidate = header.replace(old, '''    if (FP_ANDROID_EH_SETJMP(_ehjb)) { _ind = _ehf.resume; goto _ljump; }''')
    marker = '/* The lifter emits these. _ind and _ljump belong to the generated body. */'
    if candidate.count(marker) != 1:
        raise ValueError('Canonical EH macro marker changed')
    candidate = candidate.replace(marker, '''/* Candidate only. Windows exercises unchanged EH semantics; ARM uses Bionic
 * _setjmp with the same jmp_buf and no signal-mask capture. Generic native
 * callbacks which change the mask then escape are explicitly NOT equivalent. */
#if defined(__ANDROID__)
#define FP_ANDROID_EH_SETJMP(buffer) _setjmp(buffer)
#else
#define FP_ANDROID_EH_SETJMP(buffer) setjmp(buffer)
#endif

''' + marker)
    thread_source = (ROOT / 'android/host/win64_threads.c').read_text()
    if thread_source.count('if(!setjmp(state->exit_boundary))') != 1 or thread_source.count('longjmp(state->exit_boundary,1)') != 1:
        raise ValueError('Unchanged thread-exit boundary must retain real setjmp/longjmp')
    if thread_source.count(raise_guard) != 1:
        raise ValueError('RaiseException must continue declining all non-thread-name codes, including0xe06d7363')
    ndk = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version']
    bins = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    bionic_header = bins.parent / 'sysroot/usr/include/setjmp.h'
    bionic = bionic_header.read_text()
    if 'int _setjmp(jmp_buf __env) __returns_twice;' not in bionic or 'mask is not saved.' not in bionic or 'mask is saved.' not in bionic:
        raise ValueError('Pinned Bionic API does not prove mask-saving distinction')
    toolchain, env = windows_toolchain()
    commands = []

    def run(args):
        command = list(map(str, args)); commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-4000:] + result.stderr[-4000:])
        return result

    variants = {}
    for name, active_header, flag in [('baseline', header, 0), ('candidate', candidate, 1)]:
        directory = OUT / name
        directory.mkdir(exist_ok=True)
        (directory / 'eh64.h').write_text(active_header)
        # Quote-includes resolve to these exact copies. Runtime C itself is unchanged.
        for item in ['cpu64.h', 'eh64.c']:
            shutil.copyfile(ROOT / 'native/runtime64' / item, directory / item)
        host = directory / 'eh-host.exe'
        host_compile = run([toolchain / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11',
                            '/DFP_EH_CANDIDATE=' + str(flag), '/I' + str(directory), '/I' + str(ROOT / 'native/runtime64'),
                            ROOT / 'tests/android_eh_setjmp_fixture.c', '/Fo' + str(directory) + '/', '/Fe' + str(host)])
        proof_run = run([host]); proof = json.loads(proof_run.stdout)
        if not proof['success'] or proof['cases'] != 3072 or proof['parallel_cases'] != 1024 or proof['generic_candidate_qualified']:
            raise ValueError('Unexpected EH proof scope/result')
        (directory / 'host.log').write_text(proof_run.stdout + proof_run.stderr)
        arm = directory / 'eh-arm64'
        arm_compile = run([bins / 'clang.exe', '--target=aarch64-linux-android30', '-std=c11', '-O2', '-g',
                           '-ffp-contract=off', '-DFP_EH_CANDIDATE=' + str(flag), '-I' + str(directory), '-I' + str(ROOT / 'native/runtime64'),
                           ROOT / 'tests/android_eh_setjmp_fixture.c', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', arm])
        elf = arm.read_bytes()
        if elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 183:
            raise ValueError('Candidate is not AArch64 ELF')
        symbols = run([bins / 'llvm-readelf.exe', '--dyn-syms', arm]).stdout
        for symbol in ['setjmp', 'longjmp', 'pthread_sigmask'] + (['_setjmp'] if flag else []):
            if symbol not in symbols:
                raise ValueError('Required real Bionic API missing: ' + symbol)
        variants[name] = {'header': str((directory / 'eh64.h').relative_to(ROOT)), 'header_sha256': sha(directory / 'eh64.h'),
                          'host': proof, 'host_compiler_stderr': host_compile.stderr, 'arm_compiler_stderr': arm_compile.stderr,
                          'binary': str(arm.relative_to(ROOT)), 'binary_sha256': sha(arm), 'args': [],
                          'benchmark_args': ['--benchmark', '2000000']}
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Production or proof source changed during build')
    report = {'success': True, 'phase': 'host-proof-arm-build', 'sources': before, 'commands': commands,
              'bionic_setjmp_header': {'path': str(bionic_header), 'sha256': sha(bionic_header)}, 'variants': variants,
              'scope': 'Exact unchanged eh64.c over synthetic data-only PE, full CPU checks before/after funclets, RBX unwind, nested rethrow, unhandled frames, four parallel workers, unchanged thread-exit setjmp boundary; Windows is semantic-only because it has no POSIX signal mask.',
              'limitation': 'Generic candidate rejected for callbacks that change native signal mask then throw/longjmp out before restoring it. ARM fixture explicitly requires baseline restores entry mask and candidate retains changed mask. Only a separately proven mask-invariant supported call contract can qualify this candidate.',
              'supported_contract_prerequisite': {
                  'project_owned_Android_source_files_audited': len(boundary_paths),
                  'guest_CxxThrowException_supported': False, 'es3_eh_throw_callers': 0,
                  'project_owned_signal_or_mask_API_calls': 0,
                  'main_CxxThrowException_IAT': '0x141cc7a00', 'dependency_CxxThrowException_IAT': '0x1a00632b8',
                  'RaiseException_only_supported_code': '0x406d1388 (thread-name notification, no nonlocal jump)',
                  'unsupported_main_and_dependency_imports': 'exit70 before guest catch/longjmp',
                  'thread_exit_setjmp_unchanged': True,
                  'not_proven': 'External libraries never alter masks; future guest-throw/native callback combinations. Generic mask-changing callback parity is explicitly rejected.'},
              'integration_cost': {
                  'canonical_header_must_remain_unchanged': True,
                  'main_AOT_objects_invalidated_by_current_common_header_or_flags_change': 1003,
                  'reason': 'build_lifted fingerprints all runtime headers and flags into every main AOT object; build_startup revalidates that complete common hash.',
                  'dependency_objects_with_runtime_header_contract': 8,
                  'safe_future_method': 'Explicit Android-only EH macro header override/source map with producer receipts and own object identity, never global setjmp symbol wrapping.',
                  'historical_context': '459 base objects compiled in323.03s;51 repair objects in111.11s, different mixes. No reliable full rebuild ETA inferred.'},
              'generic_candidate_qualified': False, 'device_executed': False, 'production_changed': False,
              'AOT_recompiled': False, 'original_code_executed': False, 'game_boot_proven': False}
    receipt = OUT / 'result.json'; receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'variants': variants, 'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
