"""Windows LONG32 strtol/strtoul differential proof and ARM CLI; never ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-strtol-test'
INPUT = struct.Struct('<iiII256s')
OUTPUT = struct.Struct('<QiiII')
NEGATIVES = ['--null', '--base1', '--base-negative', '--base37', '--input', '--output', '--readonly-output', '--short-output', '--unterminated']


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def encode(value, base):
    alphabet = '0123456789abcdefghijklmnopqrstuvwxyz'
    if not value:
        return '0'
    text = ''
    while value:
        value, digit = divmod(value, base)
        text = alphabet[digit] + text
    return text


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_utility.c', 'android/host/win64_utility.h', 'android/host/win64.c',
             'tests/strtol_oracle.c', 'tests/strtol_oracle.asm', 'tests/win64_strtol_fixture.c',
             'tools/test_android_strtol.py', 'native/runtime64/cpu64.h', 'generated/frostpunk-x64/recomp_funcs_0816.c']
    before = {name: sha(ROOT / name) for name in files}
    route = '    if (crt && win64_utility_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Production CRT utility route changed')
    (OUT / 'strtol_route.inc').write_text(route + '\n')
    import pefile
    import capstone
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE changed')
    pe = pefile.PE(str(original), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    address, size = 0x140F0712E, 0x32
    raw = pe.get_data(address-base, size)
    decoded = [(i.address, i.mnemonic, i.op_str) for i in capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64).disasm(raw, address)]
    if [(m, o) for _, m, o in decoded] != [
        ('movzx', 'eax, byte ptr [r9 + rdx*2 + 2]'), ('mov', 'byte ptr [rbp - 0x80], al'),
        ('movzx', 'eax, byte ptr [r9 + rdx*2 + 4]'), ('mov', 'byte ptr [rbp - 0x7f], al'),
        ('mov', 'byte ptr [rbp - 0x7e], r14b'), ('xor', 'edx, edx'), ('lea', 'r8d, [rdx + 0x10]'),
        ('lea', 'rcx, [rbp - 0x80]'), ('call', 'qword ptr [rip + 0xdc0aa4]'),
        ('movd', 'xmm6, eax'), ('cvtdq2ps', 'xmm6, xmm6'), ('mulss', 'xmm6, xmm8')]:
        raise ValueError('Original two-byte hexadecimal caller changed: ' + repr(decoded))
    cases = []
    texts = [b'', b' ', b'\t\r\n\v\f -1tail', b'+', b'-', b'+ 1', b'- 1', b'0x', b'-0x', b'+0X', b'0xg',
             b'0b101', b'0X123z', b'09', b'08', b'-0', b'0', b'1', b'-1', b'2147483647', b'2147483648',
             b'-2147483648', b'-2147483649', b'4294967295', b'4294967296', b'-4294967296', b'Zz', b'\x80123',
             b'\xa0123', b'\xff-1', b'9'*240, b'-' + b'9'*239]
    def add(text, radix, flags, csr=0x1fa4, seed=1379):
        if len(text) >= 256:
            raise ValueError('Test text too long')
        cases.append((radix, seed, csr, flags, text))
    for index, text in enumerate(texts):
        for radix in [0, *range(2, 37)]:
            for operation in (0, 1):
                for mode in range(4):
                    flags = operation | (2 if (index + mode) & 1 else 0) | (8 if mode & 1 else 0)
                    add(text, radix, flags, 0x1fa4 | (mode << 13) | (64 if index & 1 else 0) | (32768 if index & 2 else 0))
    for radix in range(2, 37):
        for value in [0, 1, 2, 2147483646, 2147483647, 2147483648, 2147483649, 4294967294, 4294967295, 4294967296, 2**63, 2**64]:
            for sign in ['', '+', '-']:
                text = (' \t' + sign + encode(value, radix) + '!').encode()
                for operation in (0, 1):
                    add(text, radix, operation | 8)
    # Exact original caller's two-character hexadecimal bytes, both cases.
    for value in range(256):
        for uppercase in (False, True):
            text = format(value, '02X' if uppercase else '02x').encode()
            for operation in (0, 1):
                add(text, 16, operation | 2 | 8)
    rng = random.Random(0x140F0714E)
    for index in range(1024):
        radix = rng.randrange(2, 37)
        text = (rng.choice(['', '-', '+', '  -']) + encode(rng.getrandbits(rng.randrange(1, 100)), radix) + rng.choice(['', '!', '  end'])).encode()
        for operation in (0, 1):
            add(text, radix, operation | (index & 2) | (index & 8), 0x1fa4 | ((index & 3) << 13))
    inputs = OUT / 'inputs.bin'
    inputs.write_bytes(b''.join(INPUT.pack(*row) for row in cases))
    commands = []

    def run(args, expected=0):
        commands.append(list(map(str, args)))
        result = subprocess.run(commands[-1], cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + result.stdout[-4000:] + result.stderr[-4000:])
        return result

    bridge = OUT / 'oracle-bridge.obj'
    run([bins / 'ml64.exe', '/nologo', '/c', '/Fo' + str(bridge), ROOT / 'tests/strtol_oracle.asm'])
    oracle = OUT / 'strtol-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', ROOT / 'tests/strtol_oracle.c', bridge,
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle)])
    native = OUT / 'native.bin'
    native_result = json.loads(run([oracle, inputs, native]).stdout)
    results = list(OUTPUT.iter_unpack(native.read_bytes()))
    if native_result['cases'] != len(cases) or native_result['native_LONG_bytes'] != 4 or len(results) != len(cases):
        raise ValueError('Native oracle rows differ')
    dataset = OUT / 'cases.bin'
    with dataset.open('wb') as stream:
        for row, output in zip(cases, results, strict=True):
            if output[3] != row[2] or output[4] != 0x5a17 or output[0] > 0xffffffff or output[1] not in (1379, 34):
                raise ValueError('Unexpected native FP/LastError/LONG32/errno contract')
            stream.write(INPUT.pack(*row) + OUTPUT.pack(*output))
    includes = [ROOT / 'native/runtime64', ROOT / 'android/host', ROOT / 'generated/frostpunk-x64', ROOT / 'generated/win64', OUT]
    host = OUT / 'strtol-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/fp:strict', '/Gy', *['/I' + str(p) for p in includes],
         ROOT / 'tests/win64_strtol_fixture.c', ROOT / 'android/host/win64_utility.c', '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_run = run([host, dataset])
    host_result = json.loads(host_run.stdout)
    if not host_result['success'] or host_result['cases'] != len(cases) or host_result['concurrent_cases'] != 8192:
        raise ValueError('Host strtol/strtoul differential failed')
    negative_results = []
    for arg in NEGATIVES:
        failure = run([host, arg], expected=73)
        negative_results.append({'arg': arg, 'exit_code': 73, 'stderr': failure.stderr})
    binary = OUT / 'strtol-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-ffunction-sections', '-fdata-sections',
         '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', *['-I' + str(p) for p in includes],
         ROOT / 'tests/win64_strtol_fixture.c', ROOT / 'android/host/win64_utility.c', '-Wl,--gc-sections',
         '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-lm', '-o', binary])
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Production/proof input drift')
    report = {'success': True, 'phase': 'host-native-oracle-arm-build', 'sources': before, 'commands': commands,
              'original_exe_sha256': cfg['original_exe_sha256'], 'original_caller': {'va': hex(address), 'bytes': raw.hex(),
                 'sha256': hashlib.sha256(raw).hexdigest(), 'decoded': decoded, 'base': 16, 'endptr': None, 'input': 'two byte values copied from UTF16 lowbytes plus NUL', 'return': '0x140f07154'},
              'native': native_result, 'host': host_result, 'host_stderr': host_run.stderr, 'negative_cases': negative_results,
              'dataset': {'path': str(dataset.relative_to(ROOT)), 'sha256': sha(dataset), 'count': len(cases), 'row_bytes': 296},
              'binary': {'path': str(binary.relative_to(ROOT)), 'sha256': sha(binary), 'args': ['cases.bin']},
              'scope': 'C/UTF8-locale ASCII integer parser for bases0,2..36; Windows LONG32 fullRAXzeroextension, signed saturation/errno34, endptr and unchanged strtoul semantics. NULLinput/invalidbase require unsupported guest invalid-parameter callbacks and STOP73; invalid buffers STOP. No Windows account/device/locale parity beyond existing supported locale.',
              'device_actions': False, 'original_bytes_executed': False, 'gameplay_proven': False}
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': host_result, 'binary': report['binary'], 'dataset': report['dataset'],
                      'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
