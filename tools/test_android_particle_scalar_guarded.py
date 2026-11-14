"""Isolated finite-only scalar asm experiment; no canonical/production edits or ADB."""
import hashlib
import json
from pathlib import Path
import subprocess
import statistics

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-particle-scalar-guarded-test'
CANDIDATE = '''#pragma once
static inline int fp_particle_finite_pair(float a,float b) {
 uint32_t aa,bb;memcpy(&aa,&a,4);memcpy(&bb,&b,4);
 return (aa&UINT32_C(0x7fffffff))<UINT32_C(0x7f800000)&&
        (bb&UINT32_C(0x7fffffff))<UINT32_C(0x7f800000);
}
static inline float fp_particle_addss(float a,float b){
 if(fp_particle_finite_pair(a,b)) {
#if defined(__aarch64__)
  float result;__asm__ volatile("fadd %s0,%s1,%s2":"=w"(result):"w"(a),"w"(b));return result;
#elif defined(__clang__) && defined(__x86_64__)
  __asm__ volatile("addss %1,%0":"+x"(a):"x"(b));return a;
#else
#error Guarded scalar experiment requires ARM64 or Clang x64
#endif
 }
 return frostpunk_addss(a,b);
}
static inline float fp_particle_mulss(float a,float b){
 if(fp_particle_finite_pair(a,b)) {
#if defined(__aarch64__)
  float result;__asm__ volatile("fmul %s0,%s1,%s2":"=w"(result):"w"(a),"w"(b));return result;
#elif defined(__clang__) && defined(__x86_64__)
  __asm__ volatile("mulss %1,%0":"+x"(a):"x"(b));return a;
#else
#error Guarded scalar experiment requires ARM64 or Clang x64
#endif
 }
 return frostpunk_mulss(a,b);
}
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'port.json').read_text())
    llvm = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    bins, environment = windows_toolchain()
    names = ['native/runtime64/cpu64.h', 'native/runtime64/sse_scalar.h',
             'generated/frostpunk-x64/recomp_funcs_0820.c', 'build/arm64/generated-overlay/recomp_funcs_0820.c',
             'tests/particle_scalar_fixture.c', 'tools/test_android_particle_scalar_guarded.py',
             'tests/particle_scalar_guarded_bench.c',
             'build/android-particle-scalar-test/rejected-summary.json',
             'build/android-particle-scalar-test/finite-pair-rejected/arm-disassembly.txt',
             'build/android-particle-scalar-test/result-first-rejected/arm-disassembly.txt']
    before = {name: sha(ROOT / name) for name in names}
    header = OUT / 'particle_scalar_candidate.h'; header.write_text(CANDIDATE)
    source = ROOT / 'tests/particle_scalar_guarded_bench.c'
    flags = ['-std=c11', '-O2', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-fno-math-errno',
             '-I' + str(ROOT / 'native/runtime64'), '-I' + str(OUT)]
    commands = []

    def run(args, expected=0):
        command = list(map(str, args)); commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=environment, capture_output=True, text=True, timeout=90)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-4000:] + result.stderr[-4000:])
        return result

    obj = OUT / 'guarded-host.obj'; host = OUT / 'guarded-host.exe'
    run([llvm / 'clang.exe', '--target=x86_64-pc-windows-msvc', *flags, '-fms-runtime-lib=static', '-c', source, '-o', obj])
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(host), obj,
         'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_run = run([host]); host_result = json.loads(host_run.stdout)
    (OUT / 'host.stdout.json').write_text(host_run.stdout); (OUT / 'host.stderr.txt').write_text(host_run.stderr)
    if (host_result.get('success') is not True or host_result['paired_cases'] != 574464
            or host_result['full_CPU_alias_cases'] != 512 or host_result['native_FP_modes'] != 32
            or host_result['FP_state_mismatches'] or host_result['bit_mismatches']):
        raise ValueError('Guarded candidate changed native results/state')
    host_benchmark = json.loads(run([host, '--benchmark', '512']).stdout)
    for args in [['--benchmark','0'], ['--benchmark','16385'], ['--benchmark','1junk'],
                 ['--benchmark'], ['--unknown','1']]:
        run([host, *args], expected=2)
    host_benchmark['baseline_median_ns'] = statistics.median(row['baseline_ns'] for row in host_benchmark['samples'])
    host_benchmark['candidate_median_ns'] = statistics.median(row['candidate_ns'] for row in host_benchmark['samples'])
    arm = OUT / 'particle-guarded-arm64'
    run([llvm / 'clang.exe', '--target=aarch64-linux-android30', *flags, '-fPIC', '-fvisibility=hidden', '-g0', '-w', source,
         '-Wl,-z,max-page-size=16384', '-lm', '-o', arm])
    disassembly = run([llvm / 'llvm-objdump.exe', '-d', '--disassemble-symbols=baseline,candidate,baseline_kernel,candidate_kernel', arm]).stdout
    (OUT / 'arm-disassembly.txt').write_text(disassembly)
    if before != {name: sha(ROOT / name) for name in names}:
        raise ValueError('Production/proof source changed during isolated experiment')
    result = dict(success=True, source_hashes=before, compiler_sha256=sha(llvm / 'clang.exe'), commands=commands,
        candidate_header=dict(path=header.relative_to(ROOT).as_posix(), sha256=sha(header)), host=host_result,
        benchmark=host_benchmark,
        arm=dict(compiled=True, binary=arm.relative_to(ROOT).as_posix(), sha256=sha(arm), executed=False,
                 args=[], benchmark_args=['--benchmark','512'], expected_common_result={key:host_result[key] for key in
                     ['success','paired_cases','full_CPU_alias_cases','native_FP_modes','bit_mismatches','FP_state_mismatches']},
                 checksum_cross_platform_golden=False),
        disassembly=dict(path=(OUT/'arm-disassembly.txt').relative_to(ROOT).as_posix(), sha256=sha(OUT/'arm-disassembly.txt')),
        production_changed=False, canonical_modified=False, overlay_emitted=False, gameplay_proven=False,
        negative_benchmark_argument_cases=5,
        limitation='Scalar helper experiment only; ARM native FP side effects and gain require device proof, complete particle function equivalence is not claimed.')
    path = OUT / 'result.json'; path.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(success=True, receipt=str(path), sha256=sha(path), host=host_result,
                         benchmark=host_benchmark, arm=result['arm']), indent=2))


if __name__ == '__main__':
    main()
