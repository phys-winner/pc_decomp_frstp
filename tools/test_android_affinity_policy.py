"""Isolated byte-pinned QPC/srand affinity policy; never executes ADB."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-affinity-policy-candidate'
EXPECTED_THREADS = '6cd2263cadfe674375ef293f7667bc62c4a4681e884a06a1bf33abdc5262b264'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    config = json.loads((ROOT / 'port.json').read_text())
    source = ROOT / 'android/host/win64_threads.c'
    if sha(source) != EXPECTED_THREADS:
        raise ValueError('Production threads baseline changed; do not overwrite it')
    original = Path(config['game_directory']) / 'Frostpunk.exe'
    if sha(original) != config['original_exe_sha256']:
        raise ValueError('Original PE identity changed')
    import pefile
    pe = pefile.PE(str(original), fast_load=True)
    pe.parse_data_directories(directories=[1])
    names = {item.address: (lib.dll.decode(), item.name.decode()) for lib in pe.DIRECTORY_ENTRY_IMPORT
             for item in lib.imports if item.name}
    for address, name in ((0x141cc7468, 'GetCurrentThread'), (0x141cc7408, 'SetThreadAffinityMask'),
                          (0x141cc7478, 'QueryPerformanceCounter'), (0x141cc80e0, 'srand')):
        if names[address][1] != name:
            raise ValueError('Original import identity differs')
    gate = bytes.fromhex('ff1516b7ea00488bc8ba01000000ff15a8b6ea00')
    if pe.get_data(0x140e1bd4c - pe.OPTIONAL_HEADER.ImageBase, len(gate)) != gate:
        raise ValueError('Original startup affinity sequence differs')
    text = source.read_text()
    begin = text.index('        cpu_set_t before,after;CPU_ZERO(&before);CPU_ZERO(&after);')
    end = text.index('        pthread_mutex_unlock(&threads_mutex);win64_kernel_thread_release(token);c->rax=status?', begin)
    old = text[begin:end]
    expected = '''        cpu_set_t before,after;CPU_ZERO(&before);CPU_ZERO(&after);
        uint64_t mask=c->rdx,old=0;int status=mask?sched_getaffinity(tid,sizeof(before),&before):-1;
        if(!status){for(unsigned i=0;i<64;++i){if(CPU_ISSET(i,&before))old|=UINT64_C(1)<<i;if(mask&(UINT64_C(1)<<i))CPU_SET(i,&after);}status=sched_setaffinity(tid,sizeof(after),&after);}
'''
    if old != expected:
        raise ValueError('Generic affinity source block changed')
    extracted = old.replace('uint64_t mask=c->rdx,old=0;', 'uint64_t mask=c->rdx;*old=0;').replace('old|=', '*old|=')
    baseline = OUT / 'baseline.inc'
    baseline.write_text('static int baseline_affinity(CPU *c,pid_t tid,uint64_t *old){\n' + extracted + '    return status;\n}\n')
    policy = OUT / 'policy.inc'
    fragment = policy.read_text()
    marker = 'int win64_threads_call(CPU *c,const char *name) {'
    replacement = '        uint64_t old=0;int status=apply_thread_affinity(c,state,tid,&old);\n'
    if text.count(marker) != 1:
        raise ValueError('Threads API insertion anchor changed')
    candidate_text = text.replace(marker, fragment + '\n' + marker, 1).replace(old, replacement, 1)
    if candidate_text.replace(fragment + '\n', '', 1).replace(replacement, old, 1) != text:
        raise ValueError('Candidate does not reconstruct exact baseline')
    candidate = OUT / 'win64_threads_candidate.c'
    candidate.write_text(candidate_text)
    fixture = OUT / 'fixture.c'
    pins = {str(p.relative_to(ROOT)): sha(p) for p in
            (source, policy, fixture, Path(__file__), ROOT / 'native/runtime64/cpu64.h', ROOT / 'android/host/win64.c')}
    bins, env = windows_toolchain()
    commands = []

    def run(args, expected=0):
        args = list(map(str, args)); commands.append(args)
        result = subprocess.run(args, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps(dict(command=args, returncode=result.returncode,
                expected=expected, stdout=result.stdout, stderr=result.stderr), indent=2) + '\n')
            raise RuntimeError(result.stdout[-5000:] + result.stderr[-5000:])
        return result

    host = OUT / 'affinity-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', '/experimental:c11atomics',
         '/I' + str(ROOT / 'native/runtime64'), fixture, '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_result = json.loads(run([host]).stdout)
    if not host_result['success'] or host_result['cases'] != 756 or host_result['policy_cases'] != 12:
        raise ValueError('Host paired oracle count differs')
    negatives = []
    for index in range(20):
        output = run([host, '--mutate', index], 73)
        if 'QPC seed affinity source bytes differ' not in output.stderr:
            raise ValueError('Source byte mutation did not fail closed')
        negatives.append(dict(byte_index=index, exit_code=73, stderr=output.stderr))
    clang = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary = OUT / 'affinity-arm64'
    run([clang, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing', '-fwrapv',
         '-ffp-contract=off', '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'native/runtime64'),
         fixture, '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
    if any(sha(ROOT / name) != digest for name, digest in pins.items()):
        raise ValueError('Source changed while building isolated qualification')
    report = dict(success=True, phase='isolated-host-paired-proof-arm-compiled', sources=pins,
        production_modified=False, original_instructions_executed=False, original_exe_sha256=config['original_exe_sha256'],
        original_gate=dict(va='0x140e1bd4c', bytes=gate.hex(), sha256=hashlib.sha256(gate).hexdigest(),
            guest_return='0x140e1bd60', purpose='QPC value seeds srand; no timer calibration claim'),
        candidate=dict(path=str(candidate.relative_to(ROOT)), sha256=sha(candidate)),
        baseline_extraction=dict(path=str(baseline.relative_to(ROOT)), sha256=sha(baseline)),
        host=host_result, negative_cases=negatives,
        binary=dict(path=str(binary.relative_to(ROOT)), sha256=sha(binary)),
        expected_ARM=dict(success=True, cases=756, policy_cases=12, generic_cases=744,
            real_affinity_child_inheritance_checks=2, fullCPU_nativeFP_errno_LastError=True),
        commands=commands, scope='Only byte-pinned current-thread mask1 call at return140E1BD60 preserves its existing OS affinity. Generic calls unchanged; no speedup claim.')
    receipt = OUT / 'result.json'; receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(success=True, receipt=str(receipt), sha256=sha(receipt), host=host_result, binary=report['binary'])))


if __name__ == '__main__':
    main()
