"""Build the production app worker/wake fixture for ARM64; never uses ADB."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/android-worker-lifecycle-test'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'port.json').read_text())
    original = Path(config['game_directory']) / 'Frostpunk.exe'
    if sha(original) != config['original_exe_sha256']:
        raise ValueError('Original PE changed')
    files = ['android/app/app.c', 'android/app/host_api.h', 'android/app/window_probe.h',
             'android/gpu/driver.h', 'tests/android_worker_lifecycle.c',
             'tools/test_android_worker_lifecycle.py', 'port.json']
    before = {name: sha(ROOT / name) for name in files}
    ndk = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version']
    bins = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    binary = OUT / 'worker-lifecycle-arm64'
    command = [str(bins / 'clang.exe'), '--target=aarch64-linux-android30', '-std=c11', '-O2',
               '-ffunction-sections', '-fdata-sections', '-fPIE', '-pie',
               '-DFP_ORIGINAL_EXE_SHA256="' + config['original_exe_sha256'] + '"',
               '-I' + str(ROOT / 'android/app'),
               '-I' + str(ndk / 'sources/android/native_app_glue'),
               str(ROOT / 'tests/android_worker_lifecycle.c'), '-Wl,--gc-sections',
               '-Wl,-z,max-page-size=16384', '-pthread', '-landroid', '-llog', '-ldl',
               '-o', str(binary)]
    compiled = subprocess.run(command, cwd=OUT, capture_output=True, text=True, timeout=90)
    if compiled.returncode:
        raise RuntimeError(subprocess.list2cmdline(command) + '\n' + compiled.stdout + compiled.stderr)
    header = binary.read_bytes()[:64]
    if header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
        raise ValueError('Fixture is not an ELF64 little-endian AArch64 binary')
    inspect_command = [str(bins / 'llvm-readelf.exe'), '--dyn-syms', str(binary)]
    symbols = subprocess.run(inspect_command, cwd=OUT, capture_output=True, text=True, timeout=15)
    if symbols.returncode or not all(name in symbols.stdout for name in
                                    ['ALooper_prepare', 'ALooper_wake', 'pthread_cond_wait', 'pthread_join']):
        raise ValueError('Production worker/real libandroid fixture dependencies were removed')
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Worker/wake sources changed during fixture compilation')
    report = {'success': True, 'sources': before, 'original_exe_sha256': config['original_exe_sha256'],
              'binary': str(binary.relative_to(ROOT)), 'binary_sha256': sha(binary), 'args': [],
              'expected': {'success': True, 'runtime_API_looper_wakes': 0,
                           'explicit_cooperative_stop_looper_wakes': 64,
                           'explicit_worker_finish_and_stop_looper_wakes': 2,
                           'real_wait_transition_cases': 2, 'prewake_duringwait_races': 512,
                           'production_worker_join_cases': 64, 'stuck_worker_process_exit': 74,
                           'bounded_stop_milliseconds': [1800, 5000]},
              'commands': [command, inspect_command], 'compiler_stderr': compiled.stderr,
              'device_executed': False, 'device_actions': False, 'game_boot_proven': False}
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'binary': str(binary), 'binary_sha256': sha(binary),
                      'args': [], 'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
