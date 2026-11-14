"""Native GDI oracle and bounded CPU bitmap fixtures; no ADB or game execution."""
import hashlib
import json
from pathlib import Path
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-cursor-bitmap-test'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_display.c', 'android/host/win64_display.h',
             'android/host/win64.c', 'android/host/win64.h', 'native/runtime64/cpu64.h',
             'tests/display_fixture.c', 'tests/display_change_fixture.c', 'tests/getdc_fixture.c',
             'tests/cursor_bitmap_fixture.c', 'tests/cursor_bitmap_oracle.c',
             'tests/environment_windows_host/pthread.h', 'tools/test_android_cursor_bitmap.py']
    # The CPU header name is shared with the actual build, rather than a fixture copy.
    before = {name: sha(ROOT / name) for name in files}
    route = '    if ((user || !strcasecmp(dll,"gdi32.dll") || !strcasecmp(dll,"shcore.dll")) && win64_display_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Production display import route changed')
    (OUT / 'cursor_bitmap_route.inc').write_text(
        'static int cursor_bitmap_import(CPU*c,const char*dll,const char*name){\n'
        '    const int user=!strcasecmp(dll,"user32.dll");\n' + route + '\n    return 0;\n}\n')
    commands = []

    def run(args, expected=0, marker=None):
        command = list(map(str, args))
        commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected or (marker and marker not in result.stdout + result.stderr):
            raise RuntimeError(subprocess.list2cmdline(command) + '\nexit=' + str(result.returncode) + '\n' + result.stdout + result.stderr)
        return {'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}

    includes = ['/I' + str(ROOT / 'native/runtime64'), '/I' + str(ROOT / 'android/host'),
                '/I' + str(ROOT / 'tests/environment_windows_host'), '/I' + str(OUT)]

    def host(name, oracle=False):
        exe = OUT / (name + '.exe')
        source = ROOT / ('tests/' + name + '.c')
        sources = [source]
        if name not in ('cursor_bitmap_fixture', 'cursor_bitmap_oracle'):
            sources.append(ROOT / 'android/host/win64_display.c')
        command = [bins / 'cl.exe', '/nologo', '/O2', '/std:c11']
        if not oracle:
            command += includes
        command += sources + ['/Fo' + str(OUT) + '/', '/Fe' + str(exe)]
        if oracle:
            command += ['/link', 'gdi32.lib', 'user32.lib']
        run(command)
        result = run([exe])
        return exe, result

    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original executable SHA256 changed')
    import pefile
    pe = pefile.PE(str(original), fast_load=True)
    start, end = 0x141b20020, 0x141b2020e
    original_bytes = pe.get_data(start - pe.OPTIONAL_HEADER.ImageBase, end - start)
    if len(original_bytes) != end - start:
        raise ValueError('Original SDL cursor function is truncated')
    # The observed fatal call is byte-gated independently from the whole-PE SHA.
    fatal = pe.get_data(0x141b20136 - pe.OPTIONAL_HEADER.ImageBase, 6)
    if fatal[:2] != b'\xff\x15' or 0x141b2013c + int.from_bytes(fatal[2:], 'little', signed=True) != 0x141cc7138:
        raise ValueError('Original CreateDIBSection caller changed')
    _, oracle_run = host('cursor_bitmap_oracle', oracle=True)
    oracle = json.loads(oracle_run['stdout'].strip().splitlines()[-1])
    if not oracle.get('success') or oracle.get('image_cases') != 8:
        raise ValueError('Native GDI oracle did not qualify expected layouts/ownership')
    names = ['cursor_bitmap_fixture', 'display_fixture', 'display_change_fixture', 'getdc_fixture']
    negatives = {'cursor_bitmap_fixture': ['--short-bmi', '--short-output', '--short-mono'],
                 'display_change_fixture': ['--flags', '--reserved', '--size', '--extra', '--fields'],
                 'getdc_fixture': ['wrong-owner', 'private-release', 'common-delete', 'live-reconfigure']}
    native = {}
    binaries = {}
    for name in names:
        exe, result = host(name)
        if name == 'cursor_bitmap_fixture':
            data = json.loads(result['stdout'].strip().splitlines()[-1])
            if not data.get('success') or data.get('image_cases') != 17:
                raise ValueError('Cursor bitmap CPU fixture did not qualify the complete closure')
        result['negative_cases'] = [dict(argument=argument, **run([exe, argument], expected=73,
                marker='DISPLAY_FIXTURE_BADPTR' if name == 'cursor_bitmap_fixture' else 'STOP Win64 display'))
                for argument in negatives.get(name, [])]
        native[name] = result
        binary = OUT / (name.replace('_fixture', '').replace('_', '-') + '-arm64')
        sources = [ROOT / ('tests/' + name + '.c')]
        if name != 'cursor_bitmap_fixture':
            sources.append(ROOT / 'android/host/win64_display.c')
        run([clang, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
             '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'),
             '-I' + str(ROOT / 'tests'), '-I' + str(OUT), *sources,
             '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
        binaries[name] = {'path': str(binary.relative_to(ROOT)), 'sha256': sha(binary),
                          'arguments': [], 'expected_exit': 0, 'negative_arguments_exit73': negatives.get(name, [])}
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Fixture input sources changed during validation')
    report = {'success': True, 'sources': before, 'native_oracle': oracle,
              'original_executable_sha256': cfg['original_exe_sha256'],
              'original_SDL_cursor': {'start': hex(start), 'end': hex(end), 'sha256': hashlib.sha256(original_bytes).hexdigest(),
                  'CreateDIBSection_call': hex(0x141b20136), 'call_bytes': fatal.hex(), 'IAT_slot': hex(0x141cc7138),
                  'return': hex(0x141b2013c), 'actual_image': {'width': 32, 'height': -32, 'pitch': 128, 'bytes': 4096,
                  'mono_WORD_pitch': 4, 'mono_bytes': 128}},
              'native_fixtures': native, 'arm_binaries': binaries, 'commands': commands,
              'device_executed': False, 'original_game_executed': False, 'game_boot_proven': False,
              'scope': 'Real bounded CPU DIB32/mono storage and lifetime; native cursor images honestly unavailable with error50'}
    path = OUT / 'result.json'
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'receipt': str(path), 'receipt_sha256': sha(path),
                      'native_oracle': oracle, 'arm_binaries': binaries}, indent=2))


if __name__ == '__main__':
    main()
