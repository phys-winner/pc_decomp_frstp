"""Exact Ansel capability boundary and original static-C false paths; no ADB."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-ansel-unavailable-test'
EXPORT = '?isAnselAvailable@ansel@@YA_NXZ'
ROUTE = '    if (!strcasecmp(dll,"anselsdk64.dll") && !strcmp(name,"?isAnselAvailable@ansel@@YA_NXZ")) { c->rax=0; c->rsp+=8; return 1; }'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def function(source, declaration):
    if source.count(declaration) != 1:
        raise ValueError('Canonical caller is ambiguous')
    start = source.index(declaration)
    at = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    bins, env = windows_toolchain()
    llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    canonical = 'generated/frostpunk-x64/recomp_funcs_0904.c'
    files = ['android/host/win64.c', 'native/runtime64/cpu64.h', 'native/runtime64/eh64.h',
             canonical, 'generated/frostpunk-x64/recomp_imports.c',
             'tests/ansel_unavailable_fixture.c', 'tools/test_android_ansel_unavailable.py']
    before = {name: sha(ROOT / name) for name in files}
    source = (ROOT / files[0]).read_text()
    if source.count(ROUTE) != 1:
        raise ValueError('Exact production Ansel capability route changed')
    (OUT / 'ansel_route.inc').write_text(ROUTE + '\n')
    code = (ROOT / canonical).read_text()
    bodies = '\n\n'.join(function(code, 'void L_000%X(CPU *c)' % va) for va in (0x1415D84F0, 0x1415D8280))
    (OUT / 'ansel_callers.inc').write_text(bodies + '\n')
    import pefile
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original game PE changed')
    pe = pefile.PE(str(original))
    entries = [(entry.dll.decode(), symbol.name.decode(), symbol.address)
               for entry in pe.DIRECTORY_ENTRY_IMPORT for symbol in entry.imports if symbol.name and symbol.name.decode() == EXPORT]
    if entries != [('AnselSDK64.dll', EXPORT, 0x141CC7030)]:
        raise ValueError('Actual Ansel import signature/slot changed')
    base = pe.OPTIONAL_HEADER.ImageBase
    gates = {0x1415D854D: 'ff15ddea6e0041b00db201488d0d4177390184c00f841b020000',
             0x1415D8782: '4c8d0d878e9d00e8b2df84ffc6475001',
             0x1415D829F: 'ff158bed6e0084c07464'}
    for va, raw in gates.items():
        if pe.get_data(va - base, len(bytes.fromhex(raw))).hex() != raw:
            raise ValueError('Original unavailable branch byte drift: ' + hex(va))
    spans = []
    for va, size, role in [(0x1415D84F0, 0x2C6, 'actual initialization caller including unavailable log/completion flag/return'),
                           (0x1415D8280, 0xBA, 'secondary availability guard returns false without session setup'),
                           (0x141FB1610, 24, 'original unavailable text')]:
        raw = pe.get_data(va - base, size)
        spans.append({'va': hex(va), 'size': size, 'role': role, 'bytes': raw.hex(), 'sha256': hashlib.sha256(raw).hexdigest()})
    if pe.get_data(0x141FB1610 - base, 24) != b'Ansel is not available.\0':
        raise ValueError('Original unavailable diagnostic changed')
    sdk = Path(cfg['game_directory']) / 'AnselSDK64.dll'
    sdk_pe = pefile.PE(str(sdk))
    sdk_exports = [x for x in sdk_pe.DIRECTORY_ENTRY_EXPORT.symbols if x.name == EXPORT.encode()]
    if len(sdk_exports) != 1:
        raise ValueError('Local SDK export differs')
    commands = []

    def run(args):
        commands.append(list(map(str, args)))
        result = subprocess.run(commands[-1], cwd=OUT, env=env, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + result.stdout + result.stderr)
        return result

    includes = [ROOT / 'native/runtime64', OUT]
    host = OUT / 'ansel-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/W3',
         *['/I' + str(p) for p in includes], ROOT / 'tests/ansel_unavailable_fixture.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    result = run([host])
    proof = json.loads(result.stdout)
    if not (proof['success'] and proof['provider_cases'] == 48 and proof['parallel_calls'] == 16384
            and proof['original_static_C_false_paths'] == 384):
        raise ValueError('Unexpected unavailable fixture result')
    arm = OUT / 'ansel-arm64'
    run([llvm / 'clang.exe', '--target=aarch64-linux-android30', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
         *['-I' + str(p) for p in includes], ROOT / 'tests/ansel_unavailable_fixture.c',
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', arm])
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Fixture/production inputs changed')
    report = {'success': True, 'source_sha256': before, 'original_exe_sha256': cfg['original_exe_sha256'],
              'original_spans': spans, 'actual_import': {'dll': entries[0][0], 'name': EXPORT, 'slot': hex(entries[0][2]),
                'typed_ABI': 'bool(void), AL consumed; provider returns fullRAX0; RSP increments8 once'},
              'local_sdk': {'sha256': sha(sdk), 'export_rva': hex(sdk_exports[0].address), 'executed': False},
              'host': proof, 'host_binary_sha256': sha(host), 'host_stderr': result.stderr,
              'arm': {'path': str(arm.relative_to(ROOT)), 'sha256': sha(arm), 'executed': False, 'args': []},
              'generated_sha256': {name: sha(OUT / name) for name in ('ansel_route.inc', 'ansel_callers.inc')},
              'commands': commands, 'device_actions': False, 'original_bytes_executed': False,
              'scope': 'Honest unavailable capability on Android with no Ansel backend. Exact DLL/export only; other SDK calls unhandled. Unmodified static C verifies both original false paths. No SDK native execution or full Ansel parity/gameplay claim.'}
    path = OUT / 'result.json'
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': proof, 'binary': str(arm), 'binary_sha256': sha(arm),
                      'production_sha256': before[files[0]], 'receipt': str(path), 'receipt_sha256': sha(path)}, indent=2))


if __name__ == '__main__':
    main()
