"""Build the isolated Win64 offline service provider; --test checks fresh-process persistence."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import build_offline_services as services

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/windows-services'
SOURCE = ROOT / 'reference/goldberg'


def write_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def migrate_owned_local_save(game):
    path = game / 'local_save.txt'
    if not path.exists():
        return None
    if path.is_symlink() or not path.is_file() or path.read_bytes() != b'../profile/services':
        raise ValueError('Preserve unknown local_save.txt; standard AppData requires resolving it: ' + str(path))
    backup = path.with_name('local_save.private-profile-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '.bak')
    if backup.exists():
        raise ValueError('Preserve existing local_save backup: ' + str(backup))
    path.rename(backup)
    return dict(original=str(path), backup=str(backup), sha256=services.sha(backup))


def overlay():
    if services.checked(['git', '-C', SOURCE, 'rev-parse', 'HEAD']) != services.REVISION:
        raise ValueError('Goldberg revision differs')
    if services.checked(['git', '-C', SOURCE, 'status', '--porcelain', '--untracked-files=no']):
        raise ValueError('Goldberg vendor tree modified')
    destination = OUT / 'source'
    patches = {}
    texts = {}
    def patch(name, before, after):
        text = texts.get(name, (SOURCE / name).read_text(encoding='utf-8'))
        if text.count(before) != 1:
            raise ValueError('Patch anchor differs: ' + name)
        texts[name] = text.replace(before, after)
    patch('dll/settings_parser.cpp',
          'bool steam_offline_mode = false;\n    bool disable_networking = false;\n    bool disable_overlay = false;\n    bool disable_lobby_creation = false;',
          'bool steam_offline_mode = true;\n    bool disable_networking = true;\n    bool disable_overlay = true;\n    bool disable_lobby_creation = true;')
    patch('dll/settings_parser.cpp',
          'settings_client->unlockAllDLC(true);\n            settings_server->unlockAllDLC(true);',
          'settings_client->unlockAllDLC(false);\n            settings_server->unlockAllDLC(false);')
    patch('dll/network.cpp', 'bool disable_sockets)\n{\n    tcp_port',
          'bool disable_sockets)\n{\n    disable_sockets = true; // Local-only provider, independent of config files.\n    tcp_port')
    network = texts['dll/network.cpp']
    network, count = re.subn(r'(?<![A-Za-z_])socket\(', 'fp_offline_socket(', network)
    if count != 4:
        raise ValueError('Network socket call inventory differs')
    policy = '''
static volatile LONG64 fp_denied_sockets;
static SOCKET fp_offline_socket(int, int, int) {
    InterlockedIncrement64(&fp_denied_sockets);
    WSASetLastError(WSAEACCES);
    return INVALID_SOCKET;
}
extern "C" __declspec(dllexport) unsigned long long FrostpunkOffline_DeniedSocketCalls() {
    return (unsigned long long)InterlockedCompareExchange64(&fp_denied_sockets, 0, 0);
}
extern "C" __declspec(dllexport) int FrostpunkOffline_TestNetworkDenied() {
    return fp_offline_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) == INVALID_SOCKET && WSAGetLastError() == WSAEACCES;
}
'''
    texts['dll/network.cpp'] = network.replace('#include "network.h"', '#include "network.h"\n' + policy)
    remote = (SOURCE / 'dll/steam_remote_storage.h').read_text(encoding='utf-8')
    for name in ['IsCloudEnabledForAccount', 'IsCloudEnabledForApp', 'IsCloudEnabledThisApp']:
        remote, count = re.subn(rf'(bool {name}\(\)\n\{{\n    PRINT_DEBUG\([^\n]+\n    )return [^;]+;',
                                r'\1return false;', remote)
        if count != 1:
            raise ValueError('Cloud method differs: ' + name)
    texts['dll/steam_remote_storage.h'] = remote.replace('steam_cloud_enabled = true;', 'steam_cloud_enabled = false;').replace(
        'steam_cloud_enabled = bEnabled;', 'steam_cloud_enabled = false;')
    inputs = {}
    for name in services.checked(['git', '-C', SOURCE, 'ls-files']).splitlines():
        path = SOURCE / name
        if not path.is_file():
            continue
        inputs[name] = services.sha(path)
        output = destination / name
        if name in texts:
            write_changed(output, texts[name])
            patches[name] = dict(original_sha256=inputs[name], patched_sha256=services.sha(output))
        elif not output.exists() or services.sha(output) != inputs[name]:
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, output)
    return destination, inputs, patches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    protobuf = services.verify_protobuf()
    source, inputs, patches = overlay()
    generated = OUT / 'generated'
    generated.mkdir(exist_ok=True)
    protoc = ROOT / 'reference/protobuf-21.12/protoc-win64/bin/protoc.exe'
    with tempfile.TemporaryDirectory(prefix='protoc-', dir=OUT) as folder:
        services.checked([protoc, '--cpp_out=' + folder, '-I' + str(source / 'dll'), source / 'dll/net.proto'])
        for name in ['net.pb.cc', 'net.pb.h']:
            text = (Path(folder) / name).read_text(encoding='utf-8')
            write_changed(generated / name, text)
    config = json.loads((ROOT / 'port.json').read_text())
    cmake = Path(config['android_sdk']) / 'cmake/3.22.1/bin/cmake.exe'
    # Reuse the installed MSVC environment discovery; do not change global environment.
    sys.path.insert(0, str(ROOT / 'tests'))
    from test_windows_loader import compiler_environment
    compiler, environment = compiler_environment()
    native = OUT / 'native'
    services.checked([cmake, '-S', ROOT / 'native/windows/services', '-B', native, '-G', 'Ninja',
                      '-DCMAKE_MAKE_PROGRAM=' + str(cmake.parent / 'ninja.exe'),
                      '-DCMAKE_C_COMPILER=' + compiler.as_posix(), '-DCMAKE_CXX_COMPILER=' + compiler.as_posix(),
                      '-DCMAKE_BUILD_TYPE=Release', '-DGOLDBERG_SOURCE=' + source.as_posix(),
                      '-DPROTOBUF_SOURCE=' + (ROOT / 'reference/protobuf-21.12/source/protobuf-3.21.12').as_posix(),
                      '-DGENERATED_DIR=' + generated.as_posix()], env=environment)
    log = OUT / 'compile.log'
    print('Compile isolated Windows offline provider', flush=True)
    with log.open('w', encoding='utf-8') as output:
        result = subprocess.run([str(cmake), '--build', str(native), '--target', 'steam_api64',
                                 'windows_services_probe', '--parallel', '4'], stdout=output,
                                stderr=subprocess.STDOUT, text=True, env=environment)
    if result.returncode:
        raise RuntimeError(log.read_text(encoding='utf-8')[-16000:])
    game = ROOT / 'build/windows/game'
    game.mkdir(parents=True, exist_ok=True)
    dll = game / 'steam_api64.dll'
    dlc = (ROOT / 'config/DLC.txt').read_text(encoding='utf-8')
    runs = []
    if args.test:
        profile_root = OUT / 'fixture'
        profile_root.mkdir(exist_ok=True)
        write_changed(profile_root / 'local_save.txt', 'profile')
        write_changed(profile_root / 'steam_appid.txt', '323190')
        write_changed(profile_root / 'steam_settings/DLC.txt', dlc)
        for mode in ['write', 'read']:
            run = services.checked([native / 'windows_services_probe.exe', native / dll.name, profile_root, mode], timeout=30)
            proof = json.loads(run)
            if not proof.get('pass'):
                raise ValueError('Windows provider fixture failed')
            runs.append(dict(mode=mode, result=proof))
        assert (profile_root / 'profile/323190/remote/windows_probe.bin').read_bytes() == bytes([0, 1, 0x1a, 0xff, 42, 0, 7, 9])
    if any(services.sha(SOURCE / name) != expected for name, expected in inputs.items()):
        raise ValueError('Original Goldberg sources changed during build')
    migration = migrate_owned_local_save(game)
    if not dll.exists() or services.sha(dll) != services.sha(native / dll.name):
        shutil.copyfile(native / dll.name, dll)
    write_changed(game / 'steam_appid.txt', '323190')
    settings = game / 'steam_settings'
    for name in ['offline.txt', 'disable_networking.txt', 'disable_overlay.txt', 'disable_lobby_creation.txt']:
        write_changed(settings / name, '')
    write_changed(settings / 'DLC.txt', dlc)
    write_changed(settings / 'force_account_name.txt', 'LocalPlayer')
    write_changed(settings / 'force_language.txt', 'english')
    licenses = game / 'licenses'
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(SOURCE / 'LICENSE', licenses / 'Goldberg-LGPL-3.0-or-later.txt')
    shutil.copyfile(ROOT / 'reference/protobuf-21.12/source/protobuf-3.21.12/LICENSE', licenses / 'protobuf-BSD-3-Clause.txt')
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), target='windows-x64',
                  goldberg_revision=services.REVISION, original_vendor_sources_unchanged=True,
                  protobuf_verified_trees=protobuf, source_inputs=inputs, private_overlay_patches=patches,
                  dlc_config_sha256=services.sha(ROOT / 'config/DLC.txt'),
                  configured_DLC=[int(line.split('=', 1)[0]) for line in dlc.splitlines()],
                  compiler_sha256=services.sha(compiler), binary=str(dll.relative_to(ROOT)),
                  binary_sha256=services.sha(dll), source_policy='offline/network/overlay/lobby disabled; cloud false; gameplay DLC configured',
                  network_hard_deny='all four socket creation sites replaced with WSAEACCES; init disables sockets',
                  save_path_policy='SHGetFolderPathW(CSIDL_APPDATA)/Goldberg SteamEmu Saves; no canonical local_save.txt',
                  local_save_migration=migration,
                  offline=True, cloud=False, fixture_pass=bool(runs),
                  original_inputs_modified=False, game_boot_proven=False, fresh_process_runs=runs)
    (OUT / 'build-receipt.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    (ROOT / 'research/windows-services-build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(build_pass=True, device_test=False, persistence_pass=bool(runs),
                          dll=str(dll), sha256=report['binary_sha256']), indent=2))


if __name__ == '__main__':
    main()
