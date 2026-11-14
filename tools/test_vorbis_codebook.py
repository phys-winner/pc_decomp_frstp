#!/usr/bin/env python3
"""Device test: native libvorbis codebook decoders against the translated originals.

Extracts the three codebook decoders and their oggpack_look/oggpack_adv helpers
from generated/frostpunk-x64, builds them with
tools/vorbis_codebook_test.c and android/host/vorbis_native.c for arm64 Android (the runtime's
compile flags), runs the binary on a device and reports its summary line.

  python -X utf8 -B tools/test_vorbis_codebook.py --serial SERIAL [--cases 200000] [--seed 0x1234]
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fastbuild  # noqa: E402  (toolchain paths and the runtime compile flags)
import pefile

ROOT = fastbuild.ROOT
OUT = ROOT / 'build/test-vorbis'
FUNCTIONS = {
    'L_000141C54E60': 'generated/frostpunk-x64/recomp_funcs_0986.c',
    'L_000141C54AF0': 'generated/frostpunk-x64/recomp_funcs_0986.c',
    'L_000141C551D0': 'generated/frostpunk-x64/recomp_funcs_0986.c',
    'L_000140DFE110': 'generated/frostpunk-x64/recomp_funcs_0801.c',
    'L_000140DFE1F0': 'generated/frostpunk-x64/recomp_funcs_0801.c',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(name, source):
    text = (ROOT / source).read_text(encoding='utf-8')
    match = re.search(r'^void %s\(CPU \*c\)\n\{\n.*?^\}\n' % name, text, re.S | re.M)
    if not match:
        raise SystemExit(f'{name} not found in {source}')
    return match.group(0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--serial', required=True)
    parser.add_argument('--cases', type=int, default=200000)
    parser.add_argument('--seed', default='0x9E3779B97F4A7C15')
    args = parser.parse_args()
    if args.cases <= 0:
        parser.error('--cases must be positive')
    try:
        int(args.seed, 0)
    except ValueError:
        parser.error('--seed must be an integer')
    adb = ['adb', '-s', args.serial]
    running = subprocess.run([*adb, 'shell', 'pidof com.frostpunk.staticrecomp || true'],
                             check=True, capture_output=True, text=True)
    if running.stdout.strip():
        raise SystemExit('Stop the game before running the decoder test.')
    OUT.mkdir(parents=True, exist_ok=True)
    original = Path(fastbuild.CONFIG['game_directory']) / 'Frostpunk.exe'
    if sha(original) != fastbuild.CONFIG['original_exe_sha256']:
        raise SystemExit('Original EXE hash mismatch.')
    with pefile.PE(str(original), fast_load=True) as pe:
        table = pe.get_data(0x1C556B0, 32)
    if len(table) != 32:
        raise SystemExit('Original decoder jump table is incomplete.')
    tables = OUT / 'tables.h'
    tables.write_text('static const unsigned char original_jump_table[32]={' +
                      ','.join(str(value) for value in table) + '};\n', encoding='utf-8')
    lifted = OUT / 'lifted.c'
    lifted.write_text('/* extracted by tools/test_vorbis_codebook.py */\n#include "cpu64.h"\n\n' +
                      '\n'.join(extract(name, source) for name, source in FUNCTIONS.items()),
                      encoding='utf-8', newline='\n')
    binary = OUT / 'vorbis_codebook_test'
    flags = [flag for flag in fastbuild.COMPILE_FLAGS if flag not in ('-fPIC', '-DFROSTPUNK_ANDROID_LIBRARY=1')]
    command = [str(fastbuild.CLANG), *flags, '-fPIE', '-pie', '-Wno-unused-parameter',
               *[f'-I{ROOT / include}' for include in fastbuild.INCLUDES],
               f'-I{OUT}', '-Wl,-z,max-page-size=16384',
               str(ROOT / 'tools/vorbis_codebook_test.c'), str(lifted), '-lm', '-o', str(binary)]
    subprocess.run(command, check=True)
    remote = '/data/local/tmp/frostpunk-vorbis-' + sha(binary)[:20]
    try:
        subprocess.run([*adb, 'push', str(binary), remote], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([*adb, 'shell', 'chmod', '755', remote], check=True)
        result = subprocess.run([*adb, 'shell', remote, str(args.cases), args.seed],
                                capture_output=True, text=True, timeout=300)
    finally:
        subprocess.run([*adb, 'shell', 'rm', '-f', remote], check=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr[-4000:])
    (OUT / 'device.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    sources = [Path(__file__), ROOT / 'tools/vorbis_codebook_test.c',
               ROOT / 'android/host/vorbis_native.c', ROOT / 'android/host/vorbis_native.h',
               tables, lifted, *{ROOT / source for source in FUNCTIONS.values()}]
    success = (result.returncode == 0 and 'failures=0 self_check_state=1' in result.stdout
               and 'VORBIS_ADD_TEST verified=' in result.stdout
               and 'MISMATCH' not in result.stderr)
    receipt = dict(success=success, serial=args.serial, cases=args.cases, seed=args.seed,
                   binary_sha256=sha(binary), original_exe_sha256=sha(original),
                   sources={str(path): sha(path) for path in sources})
    (OUT / 'result.json').write_text(json.dumps(receipt, indent=1) + '\n', encoding='utf-8')
    return 0 if success else result.returncode or 1


if __name__ == '__main__':
    sys.exit(main())
