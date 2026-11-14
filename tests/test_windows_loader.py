"""Compile and exercise the NX PE loader; --original maps the pinned input only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def compiler_environment():
    vswhere = Path('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
    studio = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*',
                                         '-property', 'installationPath'], text=True).strip())
    msvc = max((studio / 'VC/Tools/MSVC').iterdir())
    sdk = Path('C:/Program Files (x86)/Windows Kits/10')
    version = max((sdk / 'Lib').iterdir()).name
    compiler = msvc / 'bin/Hostx64/x64/cl.exe'
    env = os.environ.copy()
    env['INCLUDE'] = ';'.join(map(str, [msvc / 'include', *[sdk / f'Include/{version}/{x}'
                                                         for x in ['ucrt', 'shared', 'um']]]))
    env['LIB'] = ';'.join(map(str, [msvc / 'lib/x64', *[sdk / f'Lib/{version}/{x}/x64'
                                                     for x in ['ucrt', 'um']]]))
    env['PATH'] = str(sdk / f'bin/{version}/x64') + os.pathsep + str(compiler.parent) + os.pathsep + env['PATH']
    return compiler, env


def synthetic_image():
    data = bytearray(0x600)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x8664, 2, 0, 0, 0, 240, 0x22)
    optional = 0x98
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 16, 0x1000)
    struct.pack_into('<Q', data, optional + 24, 0x140000000)
    struct.pack_into('<II', data, optional + 32, 4096, 512)
    struct.pack_into('<II', data, optional + 56, 0x3000, 0x200)
    struct.pack_into('<I', data, optional + 108, 16)
    struct.pack_into('<II', data, optional + 112 + 5 * 8, 0x2000, 12)
    for offset, name, rva, raw, flags in [(0x188, b'.text', 0x1000, 0x200, 0x60000020),
                                         (0x1b0, b'.reloc', 0x2000, 0x400, 0x42000040)]:
        struct.pack_into('<8sIIIIIIHHI', data, offset, name, 0x200, rva, 0x200, raw, 0, 0, 0, 0, flags)
    struct.pack_into('<Q', data, 0x200, 0x140000123)
    struct.pack_into('<IIHH', data, 0x400, 0x1000, 12, 0xa000, 0)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', action='store_true')
    args = parser.parse_args()
    compiler, env = compiler_environment()
    output = ROOT / 'build/windows-loader-test'
    output.mkdir(exist_ok=True)
    executable = output / 'windows_loader_fixture.exe'
    subprocess.run([str(compiler), '/nologo', '/W4', '/std:c11', '/Od',
                    str(ROOT / 'tests/windows_loader_fixture.c'), str(ROOT / 'native/windows/loader.c'),
                    '/Fo' + str(output) + '/', '/Fe' + str(executable), '/link',
                    '/BASE:0x180000000', '/DYNAMICBASE:NO'], env=env, check=True)
    def run(mode, path):
        subprocess.run([str(executable), mode, str(path)], check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix='frostpunk-loader-') as folder:
        target = Path(folder) / 'fixture.exe'
        valid = synthetic_image()
        target.write_bytes(valid)
        run('synthetic', target)
        malformed = [b'', b'MZ' + bytes(62), valid[:300]]
        overlap = bytearray(valid)
        struct.pack_into('<I', overlap, 0x1b0 + 12, 0x1000)
        malformed.append(overlap)
        malformed_reloc = bytearray(valid)
        struct.pack_into('<I', malformed_reloc, 0x400 + 4, 0xffffffff)
        for value in malformed:
            target.write_bytes(value)
            run('reject', target)
        target.write_bytes(malformed_reloc)
        run('reject-reloc', target)
    if args.original:
        config = json.loads((ROOT / 'port.json').read_text())
        original = Path(config['game_directory']) / 'Frostpunk.exe'
        with original.open('rb') as stream:
            assert hashlib.file_digest(stream, 'sha256').hexdigest() == config['original_exe_sha256']
        run('map', original)
    print('WINDOWS_LOADER_SUITE_PASS')


if __name__ == '__main__':
    main()
