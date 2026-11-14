"""System XInput 9.1.0 disconnected oracle and extracted production ARM fixture; no ADB."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-xinput-test'
CALLER = bytes.fromhex('4883ec288b4910f30f58c9f30f58d20f57e4488d542438f30f5d0dc98a2701f30f5d15c18a2701f30f5fccf30f5fd4f30f590d050ae200f30f5915fd09e200f30f2cc16689442438f30f2cc2668944243ae8c669b6004883c428c3')
THUNK = bytes.fromhex('ff25ce491e00')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def function(text, signature):
    start = text.index(signature)
    at = text.index('{', start)
    depth = 1
    end = at + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    files = ['android/host/win64.c', 'android/host/win64.h', 'tests/xinput_disconnected_oracle.c',
             'tests/xinput_disconnected_fixture.c', 'tools/test_android_xinput.py', 'port.json',
             'generated/frostpunk-x64/recomp_funcs_0821.c', 'generated/frostpunk-x64/recomp_imports.c']
    files += [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / 'native/runtime64').glob('*.h'))]
    before = {name: sha(ROOT / name) for name in files}
    cfg = json.loads((ROOT / 'port.json').read_text())
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != '45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc':
        raise ValueError('Original PE data pin changed')
    import pefile
    pe = pefile.PE(str(original), fast_load=True)
    for va, data in ((0x140F7C7D0, CALLER), (0x141AE31EC, THUNK)):
        if pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, len(data)) != data:
            raise ValueError('Original XInput caller/thunk byte gate changed')
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    imports = [(x.address, m.dll.decode(), x.name.decode()) for m in pe.DIRECTORY_ENTRY_IMPORT
               for x in m.imports if x.name and m.dll.lower() == b'xinput9_1_0.dll']
    if imports != [(0x141CC7BB8, 'XINPUT9_1_0.dll', 'XInputGetState'),
                   (0x141CC7BC0, 'XINPUT9_1_0.dll', 'XInputSetState')]:
        raise ValueError('Original XInput typed namespace changed')
    core = (ROOT / 'android/host/win64.c').read_text()
    start = core.index('    if (!strcasecmp(dll,"xinput9_1_0.dll")')
    end = core.index('    if ((!strcasecmp(dll,"kernel32.dll")', start)
    clause = core[start:end]
    if '"XInputGetState"' not in clause or '"XInputSetState"' not in clause or 'win64_pointer' in clause:
        raise ValueError('Production no-controller XInput gate changed')
    declarations = function(core, 'static int is_crt(')
    declarations += function(core, 'void win64_set_error(') + function(core, 'uint32_t win64_get_error(')
    declarations += 'static int execute(CPU*c,const char*dll,const char*name){uint64_t a=c->rcx,b=c->rdx;\n' + clause + 'return 0;}\n'
    router = function(core, 'int win64_dispatch_import(')
    api = set(re.findall(r'\b(win64_[a-z0-9_]+_call)\(', router))
    imported = set(re.findall(r'\b(win64_[a-z0-9_]+_import)\(', router)) - {'win64_dispatch_import'}
    for name in sorted(api): declarations += '#define ' + name + ' unrelated_api\n'
    for name in sorted(imported): declarations += '#define ' + name + ' unrelated_import\n'
    declarations += '#define win64_threads_raise_exception unrelated_notification\n' + router
    for name in sorted(api | imported | {'win64_threads_raise_exception'}): declarations += '#undef ' + name + '\n'
    declarations += function(core, 'static void xinput_disconnected_self_test(')
    extracted = OUT / 'xinput_production.inc'
    extracted.write_text(declarations, newline='\n')
    bins, env = windows_toolchain()
    llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = llvm / 'clang.exe'
    commands = []
    def run(args):
        command = list(map(str, args));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout + result.stderr)
        return result
    oracle = OUT / 'xinput-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', ROOT / 'tests/xinput_disconnected_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle), '/link', 'kernel32.lib'])
    rows = []
    for api_number in (0, 1):
        for index in (0, 1, 2, 3, 4, 5, 0x80000000, 0xFFFFFFFF):
            for kind in range(7):
                row = json.loads(run([oracle, api_number, index, kind]).stdout)
                if row != {'api': api_number, 'index': index, 'pointer_kind': kind,
                           'result': 1167 if index < 4 and kind != 1 else 160,
                           'LastError': 0 if index < 4 and kind != 1 else 0x51CAFE, 'errno': 2718,
                           'buffer_untouched': True, 'controllers_absent_verified': True}:
                    raise ValueError('Native XInput disconnected profile changed: ' + repr(row))
                rows.append(row)
    header = OUT / 'xinput_native_rows.h'
    header.write_text('static const struct {unsigned api;uint32_t index;unsigned kind,result,error;int native_errno;} native_rows[]={\n' +
                      ''.join('{%du,%du,%du,%du,%du,%d},\n' % (r['api'], r['index'], r['pointer_kind'], r['result'], r['LastError'], r['errno']) for r in rows) +
                      '};\n#define NATIVE_ROW_COUNT ((unsigned)(sizeof(native_rows)/sizeof(*native_rows)))\n', newline='\n')
    fixture = ROOT / 'tests/xinput_disconnected_fixture.c'
    includes = ['-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'), '-I' + str(OUT)]
    host = OUT / 'xinput-host.exe'; obj = OUT / 'xinput-fixture.obj'
    run([compiler, '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11', '-fno-strict-aliasing',
         '-D_CRT_SECURE_NO_WARNINGS', *includes, '-c', fixture, '-o', obj])
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(host), obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_run = run([host]); expected = json.loads(host_run.stdout)
    if not expected['success'] or 'cases=48 passed' not in host_run.stderr:
        raise ValueError('Production XInput self-test or fixture did not complete')
    arm = OUT / 'xinput-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing',
         '-D_GNU_SOURCE', *includes, fixture, '-pthread', '-lm', '-Wl,-z,max-page-size=16384', '-o', arm])
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('XInput proof inputs changed during build')
    system_dll = Path(os.environ['SystemRoot']) / 'System32/xinput9_1_0.dll'
    report = {'success': True, 'source_sha256': before, 'original_exe_sha256': sha(original),
              'original_caller': {'va': '0x140f7c7d0', 'bytes': CALLER.hex(), 'return': '0x140f7c826',
                                  'index': '[controller+0x10] DWORD', 'vibration': 'two clamped floats converted to two uint16 on guest stack; return ignored'},
              'original_thunk': {'va': '0x141ae31ec', 'bytes': THUNK.hex(), 'IAT': '0x141cc7bc0'},
              'original_imports': imports,
              'native_oracle': {'dll': str(system_dll), 'dll_sha256': sha(system_dll), 'rows': rows,
                                'binary_sha256': sha(oracle), 'all_controllers_absent_checked_each_process': True,
                                'vibration_always_zero': True, 'invalid_pointers_isolated': True,
                                'official_contract': 'https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputsetstate',
                                'LastError_scope': 'Observed system XInput9_1_0: valid index/non-NULL disconnected clears LastError; invalid index or NULL preserves it. Documentation does not promise LastError.'},
              'host': expected, 'host_binary_sha256': sha(host),
              'arm': {'binary': arm.relative_to(ROOT).as_posix(), 'binary_sha256': sha(arm),
                      'compiled': True, 'executed': False, 'argv': [], 'expected_stdout': expected,
                      'expected_stderr_contains': 'XINPUT_SELF_TEST controllers=0 GetState_SetState=1'},
              'generated': {p.relative_to(ROOT).as_posix(): sha(p) for p in (extracted, header)},
              'compiler_sha256': sha(compiler), 'commands': commands, 'device_actions': False,
              'scope': 'Exact production XInput clause, import router, TEB error accessors and regression selftest. Other providers stub declines; no connected-controller backend or GetCapabilities claim.'}
    path = OUT / 'result.json';path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': expected, 'arm': report['arm'],
                      'receipt': str(path), 'receipt_sha256': sha(path)}, indent=2))


if __name__ == '__main__': main()
