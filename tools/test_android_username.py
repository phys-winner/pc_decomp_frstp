"""Native GetUserNameA contract and fixed Android profile identity; no ADB."""
import hashlib
import json
from pathlib import Path
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-username-test'
ROUTE = '    if (!strcasecmp(dll,"advapi32.dll") && win64_identity_call(c,name)) { c->rsp+=8; return 1; }'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def function(source, declaration):
    if source.count(declaration) != 1:
        raise ValueError('Provider function is ambiguous')
    start = source.index(declaration)
    at = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_environment.c', 'android/host/win64_environment.h', 'android/host/win64.c',
             'android/host/win64_kernel.c', 'android/host/win64_system.c', 'android/host/bootstrap.c',
             'native/runtime64/cpu64.h', 'generated/frostpunk-x64/recomp_funcs_0859.c',
             'generated/frostpunk-x64/recomp_imports.c', 'tests/username_oracle.c',
             'tests/username_fixture.c', 'tools/test_android_username.py']
    before = {name: sha(ROOT / name) for name in files}
    core = (ROOT / files[2]).read_text()
    if core.count(ROUTE) != 1:
        raise ValueError('Exact ADVAPI32 identity route is not present once')
    source = (ROOT / files[0]).read_text()
    helper = function(source, 'int win64_identity_call(CPU *cpu,const char *name)')
    (OUT / 'username_helper.inc').write_text(helper + '\n')
    (OUT / 'username_route.inc').write_text(ROUTE + '\n')
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE changed')
    import pefile
    pe = pefile.PE(str(original))
    imports = [(entry.dll.decode(), symbol.name.decode(), symbol.address)
               for entry in pe.DIRECTORY_ENTRY_IMPORT for symbol in entry.imports
               if symbol.name and symbol.name.decode() in ('GetUserNameA', 'GetUserNameW')]
    if imports != [('ADVAPI32.dll', 'GetUserNameA', 0x141CC7008)]:
        raise ValueError('Original identity imports differ')
    base = pe.OPTIONAL_HEADER.ImageBase
    gate = bytes.fromhex('c744243001010000488d542430488d4c2440ff15bfeda70033f685c0744c')
    if pe.get_data(0x141248231-base, len(gate)) != gate:
        raise ValueError('Actual caller/capacity/failure branch bytes drift')
    spans = []
    for va, size, role in [(0x141248200, 0x111, 'actual identity caller:257byte buffer, checkedBOOL, successful name stored inobject+0x30')]:
        raw = pe.get_data(va-base, size)
        spans.append({'va': hex(va), 'size': size, 'role': role, 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': raw.hex()})
    commands = []

    def run(args, expected=0):
        commands.append(list(map(str, args)))
        result = subprocess.run(commands[-1], cwd=OUT, env=env, capture_output=True, text=True, timeout=90)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + result.stdout + result.stderr)
        return result

    oracle_exe = OUT / 'username-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', ROOT / 'tests/username_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle_exe), '/link', 'advapi32.lib'])
    oracle_run = run([oracle_exe])
    (OUT / 'native.json').write_text(oracle_run.stdout)
    oracle = json.loads(oracle_run.stdout)
    if not oracle['success'] or oracle['native_required'] != 8:
        raise ValueError('Native oracle must have the measured seven ASCII character profile length; identity text is not exported')
    for row in oracle['rows']:
        short = row['capacity'] < 8
        if row['errno'] != 2345 or row['size_after'] != (16 if short else 8):
            raise ValueError('Native size/errno behavior changed')
        if short:
            if row['exception'] or row['result'] or row['error'] != 122 or not row['unchanged']:
                raise ValueError('Native insufficient-buffer behavior changed')
        elif row['null_buffer']:
            if row['exception'] != 0xC0000005:
                raise ValueError('Native unsupported NULL output behavior changed')
        elif row['exception'] or not row['result'] or row['error'] != 0x1234 or not row['exact_name_only']:
            raise ValueError('Native successful output/LastError behavior changed')
    if oracle['null_size']['exception'] != 0xC0000005 or any(not row['name_then_size'] for row in oracle['overlap']):
        raise ValueError('Native pointer/overlap behavior changed')
    includes = [ROOT / 'native/runtime64', ROOT / 'android/host', OUT]
    host = OUT / 'username-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/W3',
         *['/I' + str(path) for path in includes], ROOT / 'tests/username_fixture.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_run = run([host])
    proof = json.loads(host_run.stdout)
    if not proof['success'] or proof['calls'] != 10503 or proof['parallel_calls'] != 8192:
        raise ValueError('Unexpected fixed-identity fixture result')
    negatives = ['--short-output', '--readonly-output', '--null-output', '--short-size', '--readonly-size', '--null-size']
    for arg in negatives:
        run([host, arg], expected=73)
    arm = OUT / 'username-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing',
         '-ffunction-sections', '-fdata-sections', *['-I' + str(path) for path in includes],
         ROOT / 'tests/username_fixture.c', ROOT / 'android/host/win64_environment.c',
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-o', arm])
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Proof inputs changed during build')
    report = {'success': True, 'sources': before, 'original_exe_sha256': cfg['original_exe_sha256'],
              'original_spans': spans, 'original_import': {'dll': 'ADVAPI32.dll', 'name': 'GetUserNameA', 'slot': '0x141cc7008'},
              'original_call': {'return': '0x141248249', 'capacity': 257, 'type': 'BOOL(LPSTR,LPDWORD)', 'failure_handled': True},
              'identity': {'name': 'Android', 'basis': 'existing private C:/Users/Android aliases in kernel/system/bootstrap; independent of host accounts'},
              'native_oracle': oracle, 'native_oracle_sha256': sha(oracle_exe), 'host_fixture': proof,
              'host_binary_sha256': sha(host), 'host_stderr': host_run.stderr, 'negative_args': negatives,
              'arm': {'path': str(arm.relative_to(ROOT)), 'sha256': sha(arm), 'args': [], 'executed': False},
              'commands': commands, 'generated_sha256': {name: sha(OUT / name) for name in ('username_helper.inc', 'username_route.inc')},
              'scope': 'Fixed private ASCII identity only. Measured native short-buffer UTF16-byte-count quirk16, successful CHAR count8, no host account/impersonation/ACP parity. Invalid buffers STOP73 instead of native AV. Native account text is not recorded. No successor/startup/FPS claim.',
              'documentation': 'https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamea',
              'original_bytes_executed': False, 'device_actions': False}
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': proof, 'native_oracle_cases': 33, 'negative_cases': len(negatives),
                      'binary': str(arm), 'binary_sha256': sha(arm), 'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
