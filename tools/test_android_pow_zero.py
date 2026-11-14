"""Exact zero-exponent extension of the accepted codec pow; never uses ADB."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-pow-zero-test'
ROW = struct.Struct('<QQQIIiiIi')
NATIVE_ROW = struct.Struct('<QQQIIIIiI')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    bins, env = windows_toolchain()
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_math.c', 'android/host/win64_math.h', 'android/host/win64.c', 'native/runtime64/cpu64.h',
             'tests/win64_pow_fixture.c', 'tests/win64_pow_zero_oracle.c', 'tools/test_android_pow.py', 'tools/test_android_pow_zero.py',
             'generated/frostpunk-x64/recomp_funcs_0835.c', 'generated/frostpunk-x64/recomp_funcs_0964.c',
             'generated/frostpunk-x64/recomp_funcs_0983.c', 'generated/frostpunk-x64/recomp_funcs_0984.c',
             'generated/frostpunk-x64/recomp_funcs_0985.c', 'generated/frostpunk-x64/recomp_funcs_0986.c']
    before = {name: sha(ROOT / name) for name in files}
    old = ROOT / 'build/android-pow-test'
    old_pins = {name: sha(old / name) for name in ('result.json', 'cases.bin', 'pow-arm64', 'pow-host.exe')}
    route = '    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Existing production CRT math route changed')
    (OUT / 'pow_route.inc').write_text(route + '\n')
    import pefile
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE changed')
    pe = pefile.PE(str(original), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    if pe.get_data(0x1421F5338-base, 8) != struct.pack('<d', 10.0):
        raise ValueError('Original audio power10 constant changed')
    spans = []
    for va, size, role in [(0x141C33100, 0x27, 'signed16bit field+0x20 widened exactly; pow(10,value)'),
                           (0x141C3326B, 0x13, 'second pow(10,-same value), including -0'),
                           (0x141AE32D0, 6, 'pow IAT thunk')]:
        data = pe.get_data(va-base, size)
        spans.append({'va': hex(va), 'size': size, 'role': role, 'bytes': data.hex(), 'sha256': hashlib.sha256(data).hexdigest()})
    import capstone
    decoded = [(i.mnemonic, i.op_str) for i in capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64).disasm(bytes.fromhex(spans[0]['bytes']), 0x141C33100)]
    if decoded != [('movsx', 'eax, word ptr [rcx + 0x20]'), ('mov', 'rdi, rdx'),
                   ('movsd', 'xmm9, qword ptr [rip + 0x5c2228]'), ('mov', 'rbx, rcx'),
                   ('movaps', 'xmm0, xmm9'), ('movd', 'xmm7, eax'), ('cvtdq2pd', 'xmm7, xmm7'),
                   ('movaps', 'xmm1, xmm7'), ('call', '0x141ae32d0')]:
        raise ValueError('Original power10 construction changed: ' + repr(decoded))
    callers = []
    for file in files[8:]:
        code = (ROOT / file).read_text()
        callers += [int(value, 16) for value in re.findall(r'/\* 000([0-9A-F]+): call 0x141ae32d0 \*/', code)]
    if len(set(callers)) != 11:
        raise ValueError('Expected eleven unique direct pow callsites')
    values = [0, 1, 2, 3, 7, 10, 16, 31, 255, 256, 257, 65535, 65536, 16777215, 16777216, 2147483392, 2147483648]
    inputs = OUT / 'inputs.bin'
    with inputs.open('wb') as stream:
        for value in values:
            x = struct.unpack('<Q', struct.pack('<d', float(value)))[0]
            for y in (0, 1 << 63):
                for mask in range(64):
                    for mode in range(16):
                        csr = (mask << 7) | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0) | 0x24
                        stream.write(struct.pack('<QQII', x, y, csr, mode & 1))
    commands = []

    def run(args, expected=0):
        commands.append(list(map(str, args)))
        result = subprocess.run(commands[-1], cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + result.stdout[-5000:] + result.stderr[-5000:])
        return result

    oracle = OUT / 'pow-zero-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/fp:strict', ROOT / 'tests/win64_pow_zero_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle)])
    native = OUT / 'native.bin'
    oracle_result = json.loads(run([oracle, inputs, native]).stdout)
    records = list(NATIVE_ROW.iter_unpack(native.read_bytes()))
    if oracle_result != {'success': True, 'cases': 34816, 'exceptions': 0} or len(records) != 34816:
        raise ValueError('Unexpected native zero-power matrix result')
    dataset = OUT / 'cases.bin'
    with dataset.open('wb') as stream:
        for x, y, result, csr, expected_csr, routed, error, expected_errno, exception in records:
            if result != 0x3ff0000000000000 or csr != expected_csr or error != 0x5a17 or expected_errno != 1379 or exception:
                raise ValueError('Native zero-power contract differed')
            stream.write(ROW.pack(x, y, result, csr, routed, 0, 0, expected_csr, expected_errno))
    include = [ROOT / 'native/runtime64', ROOT / 'android/host', OUT]
    host = OUT / 'pow-zero-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', *['/I' + str(p) for p in include],
         ROOT / 'tests/win64_pow_fixture.c', ROOT / 'android/host/win64_math.c', '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_run = run([host, '--exact-zero', dataset])
    proof = json.loads(host_run.stdout)
    if not proof['success'] or proof['cases'] != 34816 or proof['bit_exact'] != 34816 or proof['max_ULP']:
        raise ValueError('Zero-power provider did not exactly match UCRT')
    (OUT / 'zero-host.log').write_text(host_run.stdout + host_run.stderr)
    regression = run([host, old / 'cases.bin'])
    regression_result = json.loads(regression.stdout)
    if not regression_result['success'] or regression_result['cases'] != 47728:
        raise ValueError('Accepted codec-pow regression failed')
    (OUT / 'codec-host.log').write_text(regression.stdout + regression.stderr)
    negatives = ['precision', 'reserved', 'negative', 'negative_zero', 'fraction', 'subnormal', 'NaN', 'infinity',
                 'negative_power', 'large_base', 'large_power', 'wide_power']
    negative_results = []
    for name in negatives:
        failed = run([host, '--negative', name], expected=73)
        negative_results.append({'arg': name, 'exit_code': 73, 'stderr': failed.stderr})
    binary = OUT / 'pow-zero-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-ffp-contract=off', '-fno-strict-aliasing',
         '-ffunction-sections', '-fdata-sections', *['-I' + str(p) for p in include], ROOT / 'tests/win64_pow_fixture.c',
         ROOT / 'android/host/win64_math.c', '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
    if before != {name: sha(ROOT / name) for name in files} or old_pins != {name: sha(old / name) for name in old_pins}:
        raise ValueError('Production/proof inputs changed, or historical artifacts were overwritten')
    report = {'success': True, 'phase': 'host-oracle-arm-build', 'sources': before, 'commands': commands,
              'original_exe_sha256': cfg['original_exe_sha256'], 'original_spans': spans, 'pow_direct_callsites': list(map(hex, sorted(set(callers)))),
              'actual_observation': {'x': '0x4024000000000000', 'y': '0x0000000000000000', 'mxcsr': '0x1fa4', 'guest_rip': '0x141ae32d0',
                                     'exact_caller_return_captured': False},
              'original_inference': '141C330D0 matches observed10^0 via signed16bit field+0x20 and contains a second10^-0; no claim it is the actual return address without a captured stack return',
              'native_oracle': oracle_result, 'host': proof, 'accepted_codec_regression': regression_result,
              'negative_cases': negative_results, 'preserved_historical_artifacts': old_pins,
              'dataset': {'path': str(dataset.relative_to(ROOT)), 'sha256': sha(dataset), 'count': len(records), 'row_bytes': ROW.size},
              'binary': {'path': str(binary.relative_to(ROOT)), 'sha256': sha(binary), 'args': ['--exact-zero', 'cases.bin']},
              'scope': 'Exact1 for +0/-0 exponent over unchanged existing nonnegative binary32-integer base band0..2^31. Both signed-zero powers and0^0 measured; all64mask combinations/16RC+DAZ+FTZ controls. Other exponent/base domains remain failclosed. Not general pow parity.',
              'device_executed': False, 'original_bytes_executed': False, 'gameplay_proven': False}
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': proof, 'codec_regression': regression_result,
                      'binary': report['binary'], 'dataset': report['dataset'], 'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
