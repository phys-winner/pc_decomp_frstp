"""Reproduce two rejected particle-only scalar shortcuts; never promotes or uses ADB."""
import hashlib
import json
from pathlib import Path
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-particle-scalar-test'
VARIANTS = {
    'result-first-rejected': '''#pragma once
static inline float fp_particle_result(float a,float b,float result) {
 uint32_t bits;memcpy(&bits,&result,4);
 if((bits&UINT32_C(0x7fffffff))<=UINT32_C(0x7f800000))return result;
 return frostpunk_sse_result(a,b,result);
}
static inline float fp_particle_addss(float a,float b){return fp_particle_result(a,b,a+b);}
static inline float fp_particle_mulss(float a,float b){return fp_particle_result(a,b,a*b);}
''',
    'finite-pair-rejected': '''#pragma once
static inline int fp_particle_finite_pair(float a,float b) {
 uint32_t aa,bb;memcpy(&aa,&a,4);memcpy(&bb,&b,4);
 aa&=UINT32_C(0x7fffffff);bb&=UINT32_C(0x7fffffff);
 return aa<UINT32_C(0x7f800000)&&bb<UINT32_C(0x7f800000);
}
static inline float fp_particle_addss(float a,float b){
 if(fp_particle_finite_pair(a,b))return a+b;
 return frostpunk_addss(a,b);
}
static inline float fp_particle_mulss(float a,float b){
 if(fp_particle_finite_pair(a,b))return a*b;
 return frostpunk_mulss(a,b);
}
'''}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    bins, env = windows_toolchain()
    files = ['native/runtime64/sse_scalar.h', 'native/runtime64/cpu64.h',
             'generated/frostpunk-x64/recomp_funcs_0820.c', 'build/arm64/generated-overlay/recomp_funcs_0820.c',
             'android/host/aot_cpu.c', 'android/host/aot_cpu_float.c', 'android/host/bootstrap.c',
             'tests/particle_scalar_fixture.c', 'tools/test_android_particle_scalar.py']
    before = {name: sha(ROOT / name) for name in files}
    source = ROOT / 'tests/particle_scalar_fixture.c'
    runtime = ROOT / 'native/runtime64'
    commands, variants = [], {}

    def run(args, cwd, expected=0):
        command = list(map(str, args));commands.append(command)
        result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, timeout=90)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout + result.stderr)
        return result

    for name, header in VARIANTS.items():
        directory = OUT / name
        directory.mkdir(exist_ok=True)
        path = directory / 'particle_scalar_candidate.h'
        if path.exists() and path.read_text() != header:
            raise ValueError('Preserved rejected candidate source differs: ' + name)
        path.write_text(header)
        strict = directory / 'strict-host.exe'
        run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/fp:strict',
             '/I' + str(runtime), '/I' + str(directory), source,
             '/Fo' + str(directory) + '/', '/Fe' + str(strict)], directory)
        strict_run = run([strict], directory)
        strict_result = json.loads(strict_run.stdout)
        obj, executable = directory / 'clang-host.obj', directory / 'clang-host.exe'
        flags = ['-std=c11', '-O2', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-fno-math-errno',
                 '-I' + str(runtime), '-I' + str(directory)]
        run([llvm / 'clang.exe', '--target=x86_64-pc-windows-msvc', *flags,
             '-fms-runtime-lib=static', '-c', source, '-o', obj], directory)
        run([bins / 'link.exe', '/nologo', '/OUT:' + str(executable), obj,
             'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'oldnames.lib', 'kernel32.lib'], directory)
        actual = run([executable], directory, expected=1)
        actual_result = json.loads(actual.stdout)
        if not (strict_result['success'] and actual_result['bit_mismatches'] == 0
                and actual_result['FP_state_mismatches'] == 7488
                and actual_result['paired_cases'] == 574464 and actual_result['full_CPU_alias_cases'] == 512):
            raise ValueError('Expected compiler-dependent failure changed: ' + name)
        (directory / 'clang-host.stdout.json').write_text(actual.stdout)
        (directory / 'clang-host.stderr.txt').write_text(actual.stderr)
        arm = directory / 'particle-arm64'
        run([llvm / 'clang.exe', '--target=aarch64-linux-android30', *flags, source,
             '-Wl,-z,max-page-size=16384', '-lm', '-o', arm], directory)
        disassembly = run([llvm / 'llvm-objdump.exe', '-d', '--disassemble-symbols=baseline,candidate', arm], directory)
        (directory / 'arm-disassembly.txt').write_text(disassembly.stdout)
        variants[name] = {'rejected': True, 'strict_host': strict_result, 'actual_clang_host': actual_result,
                         'stderr': actual.stderr, 'arm_compiled': True, 'arm_executed': False,
                         'artifacts': {p.name: sha(p) for p in [path, strict, executable, arm,
                              directory / 'clang-host.stdout.json', directory / 'clang-host.stderr.txt',
                              directory / 'arm-disassembly.txt']}}
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Production or fixture sources changed during experiment')
    result = {'experiment_completed': True, 'candidate_accepted': False, 'production_changed': False,
              'source_sha256': before, 'compiler_sha256': sha(llvm / 'clang.exe'), 'commands': commands,
              'variants': variants, 'intended_literal_rewrite_scope_only': {
                  'unit': '0820', 'F63A85_calls': 39, 'F6460C_calls': 36, 'F66DC6_calls': 21,
                  'overlay_emitted': False, 'canonical_or_header_edited': False},
              'native_mode_note': 'Each platform compares its own baseline/candidate. ARM FZ/DN modes and x64 DAZ/FTZ differ; checksums are diagnostic, not cross-platform goldens.',
              'conclusion': 'Both simple inline variants change signaling-NaN native invalid status with production-equivalent Clang flags. No third variant or FP-environment workaround implemented.',
              'device_actions': False, 'gameplay_proven': False}
    path = OUT / 'rejected-summary.json'
    path.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'experiment_completed': True, 'candidate_accepted': False, 'production_changed': False,
                      'receipt': str(path), 'receipt_sha256': sha(path),
                      'variants': {name: value['actual_clang_host'] for name, value in variants.items()}}, indent=2))


if __name__ == '__main__':
    main()
