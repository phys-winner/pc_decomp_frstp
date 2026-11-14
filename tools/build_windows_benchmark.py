"""Build native-original benchmark isolation; --test runs only a tiny fixture child."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/windows-benchmark'
SOURCE = ROOT / 'native/windows/benchmark'
DETOURS = ROOT / 'reference/goldberg/detours'
GAME = ROOT / 'build/windows/game'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'port.json').read_text())
    original = GAME / 'Frostpunk.exe'
    offline = GAME / 'steam_api64.dll'
    local_save = GAME / 'local_save.txt'
    services = json.loads((ROOT / 'research/windows-services-build.json').read_text())
    assert sha(original) == config['original_exe_sha256']
    assert sha(offline) == services['binary_sha256'] and services['offline'] and not services['cloud']
    assert local_save.read_text().strip() == '../profile/services'
    bins, env = windows_toolchain()
    compiler = bins / 'cl.exe'
    fixture = OUT / 'windows-benchmark-fixture.exe'
    launcher = OUT / 'frostpunk-benchmark.exe'
    dll = OUT / 'frostpunk-benchmark-profile.dll'
    detour_names = ['detours.cpp', 'modules.cpp', 'disasm.cpp', 'image.cpp', 'creatwth.cpp']
    inputs = [SOURCE / 'launcher.cpp', SOURCE / 'profile.cpp', ROOT / 'tests/windows_benchmark_fixture.cpp',
              Path(__file__).resolve(), *DETOURS.iterdir(), ROOT / 'port.json', ROOT / 'research/windows-services-build.json']
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs if p.is_file()}
    commands = []

    def run(command, **kwargs):
        command = list(map(str, command))
        commands.append(command)
        completed = subprocess.run(command, env=env, cwd=OUT, capture_output=True, text=True, **kwargs)
        if completed.returncode:
            raise RuntimeError(completed.stdout + completed.stderr)
        return completed.stdout + completed.stderr

    flags = ['/nologo', '/O2', '/MT', '/EHsc', '/std:c++17', '/W3', '/I' + str(DETOURS), '/I' + str(OUT)]
    objects = []
    for name in detour_names:
        obj = OUT / (Path(name).stem + '.obj')
        run([compiler, *flags, '/c', DETOURS / name, '/Fo' + str(obj)])
        objects.append(obj)
    detours_lib = OUT / 'detours.lib'
    run([bins / 'lib.exe', '/nologo', '/OUT:' + str(detours_lib), *objects])
    run([compiler, *flags, ROOT / 'tests/windows_benchmark_fixture.cpp', detours_lib, '/Fe' + str(fixture),
         '/link', 'shell32.lib', 'ole32.lib', 'uuid.lib'])
    run([compiler, *flags, '/LD', SOURCE / 'profile.cpp', detours_lib, '/Fe' + str(dll),
         '/link', '/EXPORT:DetourFinishHelperProcess,@1,NONAME', 'shell32.lib', 'ole32.lib'])
    defines = {'FP_ORIGINAL_PATH': str(original), 'FP_FIXTURE_PATH': str(fixture),
               'FP_PRIVATE_PROFILE': str(ROOT / 'build/windows/profile'), 'FP_GAME_DIRECTORY': str(GAME),
               'FP_OFFLINE_DLL_PATH': str(offline), 'FP_LOCAL_SAVE_PATH': str(local_save)}
    header = ''.join('#define ' + key + ' L' + json.dumps(value) + '\n' for key, value in defines.items())
    for key, value in {'FP_ORIGINAL_SHA256': sha(original), 'FP_FIXTURE_SHA256': sha(fixture),
                       'FP_PROFILE_DLL_SHA256': sha(dll), 'FP_OFFLINE_DLL_SHA256': sha(offline),
                       'FP_LOCAL_SAVE_SHA256': sha(local_save)}.items():
        header += '#define ' + key + ' "' + value + '"\n'
    (OUT / 'identity.h').write_text(header, newline='\n')
    run([compiler, *flags, SOURCE / 'launcher.cpp', detours_lib, '/Fe' + str(launcher),
         '/link', 'bcrypt.lib'])
    validation = None
    if args.test:
        before_environment = dict(os.environ)
        with tempfile.TemporaryDirectory(prefix='fixture-profile-', dir=OUT) as directory:
            profile = Path(directory)
            proof = run([launcher, fixture, profile, 'two words', 'a"b', 'trailing\\', str(profile)], timeout=20)
            assert 'WINDOWS_BENCHMARK_FIXTURE_PASS' in proof
            assert (profile / 'fixture-marker.txt').read_bytes() == b'private'
            assert sorted(p.name for p in profile.iterdir()) == ['fixture-marker.txt']
            missing_environment = run([fixture, '--test-missing-env', profile], timeout=20)
            assert 'WINDOWS_BENCHMARK_MISSING_ENV_PASS' in missing_environment
            assert not (profile / 'entry-must-not-run.txt').exists()
            absent = profile / 'absent'
            rejected = subprocess.run([str(launcher), str(fixture), str(absent)], env=env, cwd=OUT,
                                      capture_output=True, text=True, timeout=20)
            assert rejected.returncode == 73 and 'private profile missing' in rejected.stderr
            assert not absent.exists()
            original_dll = dll.read_bytes()
            try:
                dll.write_bytes(bytes([original_dll[0] ^ 1]) + original_dll[1:])
                rejected_dll = subprocess.run([str(launcher), str(fixture), str(profile)], env=env, cwd=OUT,
                                             capture_output=True, text=True, timeout=20)
                assert rejected_dll.returncode == 73 and 'isolation DLL identity' in rejected_dll.stderr
            finally:
                dll.write_bytes(original_dll)
            assert sha(dll) == hashlib.sha256(original_dll).hexdigest()
        assert dict(os.environ) == before_environment
        validation = dict(success=True, fixture_only=True, shell_APIs=3, process_environment_private=True,
                          global_environment_changed=False, private_marker=True,
                          missing_profile_rejected_before_child=True, modified_injection_dll_rejected=True,
                          missing_environment_DLL_INIT_FAILED_before_entry=True,
                          output=proof + missing_environment)
    assert hashes == {name: sha(ROOT / name) for name in hashes}
    receipt = dict(success=True, timestamp_utc=datetime.now(timezone.utc).isoformat(),
                   launcher=str(launcher.relative_to(ROOT)), launcher_sha256=sha(launcher),
                   isolation_dll=str(dll.relative_to(ROOT)), isolation_dll_sha256=sha(dll),
                   original_exe_sha256=sha(original), original_native_launched=False,
                   private_profile=str(ROOT / 'build/windows/profile'), fixture=validation,
                   source_sha256=hashes, commands=commands,
                   scope='Benchmark-only native original; same three shell redirects as recompiler. No real user registry or environment writes. Offline Steam provider pinned; local_save shares owned private service profile. Snapshot/restore that private profile between comparison legs.')
    (OUT / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({key: value for key, value in receipt.items() if key not in ('source_sha256', 'commands')}, indent=2))


if __name__ == '__main__':
    main()
