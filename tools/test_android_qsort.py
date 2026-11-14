"""Build qsort's System32-UCRT oracle/host test and an ARM CLI, without device actions."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT, windows_toolchain

NEGATIVE = ['zero-width', 'null-base', 'null-base-one', 'zero-width-empty', 'null-compare-empty', 'null-compare', 'span-overflow', 'address-overflow',
            'array-bounds', 'array-readonly', 'stack-underflow', 'frame-bounds', 'frame-readonly',
            'bad-compare', 'callback-pop', 'callback-sentinel', 'callback-free-array',
            'callback-free-pointee', 'frame-overlap']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    config = json.loads((ROOT / 'port.json').read_text())
    binaries, env = windows_toolchain()
    output = ROOT / 'build/android-qsort-test'
    include = output / 'include'
    include.mkdir(parents=True, exist_ok=True)
    original_sources = []
    functions = []
    for unit, name in [('0832', 'L_0001410310D0'), ('0986', 'L_000141C561A0')]:
        source = ROOT / f'generated/frostpunk-x64/recomp_funcs_{unit}.c'
        original_sources.append(source)
        text = source.read_text()
        start = text.index(f'void {name}(CPU *c)\n{{')
        end = text.index('    /* int3 */', start)
        functions.append(text[start:end] + '}\n')
    comparator = include / 'qsort_comparators.h'
    comparator.write_text('\n'.join(functions), encoding='utf-8', newline='\n')
    utility = ROOT / 'android/host/win64_utility.c'
    fixture = ROOT / 'tests/win64_qsort_fixture.c'
    watched = [utility, fixture, comparator, *original_sources, Path(__file__).resolve(),
               ROOT / 'android/host/win64.c', ROOT / 'generated/win64/exports.h',
               *sorted((ROOT / 'native/runtime64').glob('*.h')),
               *sorted((ROOT / 'android/host').glob('*.h'))]
    before = {str(path.relative_to(ROOT)): sha(path) for path in watched}
    commands = []

    def run(command, timeout=60):
        command = list(map(str, command))
        commands.append(command)
        result = subprocess.run(command, env=env, cwd=output, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=timeout)
        if result.returncode:
            raise RuntimeError(str(result.returncode) + '\n' + result.stdout + result.stderr)
        return result.stdout

    includes = [include, ROOT / 'native/runtime64', ROOT / 'android/host',
                ROOT / 'generated/frostpunk-x64', ROOT / 'generated/win64']
    exe = output / 'qsort-host-fixture.exe'
    run([binaries / 'cl.exe', '/nologo', '/O2', '/Gy', '/fp:strict', '/std:c11', '/W4',
         *['/I' + str(path) for path in includes], fixture, utility,
         '/Fo' + str(output) + '/', '/Fe' + str(exe)])
    dataset = output / 'native-oracle.bin'
    oracle = json.loads(run([exe, '--oracle', dataset]))
    host = json.loads(run([exe, dataset]))
    negatives = {}
    for name in NEGATIVE:
        result = subprocess.run([str(exe), '--' + name], cwd=output, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=10)
        assert result.returncode == 73 and 'STOP ' in result.stderr, (name, result.returncode, result.stderr)
        negatives[name] = {'exit_code': result.returncode, 'diagnostic': result.stderr.strip()}
    llvm = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    arm = output / 'qsort-arm64-fixture'
    run([llvm / 'clang.exe', '--target=aarch64-linux-android30', '-O2', '-std=c11',
         '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-ffunction-sections', '-fdata-sections', '-DFP_CHECKED_GUEST_POINTERS=1',
         '-fPIE', '-pie', *['-I' + str(path) for path in includes], fixture, utility,
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-ldl', '-lm', '-o', arm])
    assert before == {name: sha(ROOT / name) for name in before}, 'Source drift during qsort fixture build'
    receipt = {'success': True, 'native_oracle': oracle, 'host': host, 'negative_boundaries': negatives,
               'source_sha256': before, 'dataset': str(dataset.relative_to(ROOT)), 'dataset_sha256': sha(dataset),
               'host_binary_sha256': sha(exe), 'arm_binary': str(arm.relative_to(ROOT)),
               'arm_binary_sha256': sha(arm), 'arm_executed': False, 'device_actions': False,
               'original_game_executed': False, 'commands': commands,
               'root_device_commands': ['<verified-private-directory>/qsort-arm64-fixture <verified-private-directory>/native-oracle.bin',
                                        *['<verified-private-directory>/qsort-arm64-fixture --' + name for name in NEGATIVE]]}
    path = output / 'result.json'
    path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in receipt.items() if key not in ['source_sha256', 'commands', 'negative_boundaries', 'root_device_commands']}, indent=2))


if __name__ == '__main__':
    main()
