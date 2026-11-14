"""Isolated actual modff candidate: system UCRT oracle/host/ARM; no ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess
from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-modff-candidate'
BASE_SHA = '8687c17d2b1a194a26761fb5ed299d6cf033c7236af614d39d55cd9bfe88c6bc'
ROW = struct.Struct('<IIIIIiII')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    bins, env = windows_toolchain(); commands = []
    names = ['android/host/win64_math.c', 'android/host/win64_math.h', 'android/host/win64.h',
             'android/host/win64.c', 'native/runtime64/cpu64.h', 'tests/modff_oracle.c',
             'tests/modff_fixture.c', 'tools/test_android_modff_candidate.py',
             'build/android-modff-candidate/modff_fragment.inc',
             'build/android-modff-candidate/pre-extension-win64_math.c']
    pins = {name: sha(ROOT / name) for name in names}
    baseline = (ROOT / 'android/host/win64_math.c').read_text()
    if sha(ROOT / 'android/host/win64_math.c') != BASE_SHA or sha(OUT / 'pre-extension-win64_math.c') != BASE_SHA:
        raise ValueError('Accepted production math or baseline drift')
    fragment = (OUT / 'modff_fragment.inc').read_text()
    anchor = 'static uint64_t integer_round('
    if baseline.count(anchor) != 1 or baseline.count('||atanf_call(c,name);') != 1:
        raise ValueError('Exact candidate source anchors drift')
    candidate = baseline.replace(anchor, fragment + anchor).replace('||atanf_call(c,name);', '||atanf_call(c,name)||modff_call(c,name);')
    candidate_path = OUT / 'win64_math_candidate.c'
    candidate_path.write_text(candidate)
    if candidate.replace(fragment, '', 1).replace('||modff_call(c,name);', ';') != baseline:
        raise ValueError('Candidate changed outside two permitted hunks')
    def run(args, expected=0):
        command = list(map(str, args)); commands.append(command)
        value = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if value.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps(dict(command=command, expected=expected,
                actual=value.returncode, stdout=value.stdout, stderr=value.stderr), indent=2) + '\n')
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + value.stdout[-5000:] + value.stderr[-5000:])
        return value
    oracle = OUT / 'modff-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', ROOT / 'tests/modff_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle)])
    values = {0, 0x80000000, 0x7f800000, 0xff800000}
    for exponent in range(256):
        for fraction in [0, 1, 2, 0x155555, 0x3fffff, 0x400000, 0x400001, 0x7ffffe, 0x7fffff]:
            for sign in [0, 0x80000000]:
                values.add((exponent << 23) | fraction | sign)
    rng = random.Random(0x14189fd4b)
    values.update(rng.getrandbits(32) for _ in range(8192))
    bits = lambda x: struct.unpack('<I', struct.pack('<f', x))[0]
    for number in range(-256, 257):
        for quarter in [0, .25, .5, .75]:
            raw = bits(number + quarter)
            values.update(x for x in [raw - 1, raw, raw + 1] if 0 <= x <= 0xffffffff)
    inputs = OUT / 'inputs.bin'; input_count = 0
    with inputs.open('wb') as stream:
        for mode in range(16):
            for index, value in enumerate(sorted(values)):
                csr = 0x1f80 | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0) | (0x35 if index & 1 else 0)
                stream.write(struct.pack('<II', value, csr)); input_count += 1
        # Native-confirm zero-exception paths with every exception-mask combination.
        safe = [0, 0x80000000, 1, 0x80000001, 0x7fffff, 0x807fffff, 0x3e800000,
                0xbe800000, 0x3fc00000, 0xbfc00000, 0x4affffff, 0x7f7fffff,
                0x7f800000, 0xff800000, 0x7fc12345, 0xffc12345]
        for masks in range(64):
            for mode in range(16):
                csr = (masks << 7) | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0)
                for value in safe:
                    stream.write(struct.pack('<II', value, csr)); input_count += 1
    data = OUT / 'cases.bin'; native = json.loads(run([oracle, inputs, data]).stdout)
    rows = list(ROW.iter_unpack(data.read_bytes()))
    if len(rows) != input_count or native['cases'] != input_count or native['matherr_calls']:
        raise ValueError('Native case count or unexpected matherr callback')
    status_counts = {}
    for x, csr, result, integral, expected_csr, error, calls, last_error in rows:
        m = x & 0x7fffffff
        flags = 1 if m > 0x7f800000 and not (m & 0x400000) else 0
        if expected_csr != csr | flags or error != 1379 or calls or last_error != 0x5a17:
            raise ValueError('Unexpected UCRT modff status ' + repr((hex(x), hex(csr), hex(expected_csr), error, calls, last_error)))
        status_counts[str(flags)] = status_counts.get(str(flags), 0) + 1
    route = '    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Existing production math route drift')
    (OUT / 'modff_route.inc').write_text(route + '\n')
    fixture = ROOT / 'tests/modff_fixture.c'; host = OUT / 'modff-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', '/experimental:c11atomics',
         '/I' + str(ROOT / 'native/runtime64'), '/I' + str(ROOT / 'android/host'), '/I' + str(OUT),
         fixture, candidate_path, '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_result = json.loads(run([host, data]).stdout.strip().splitlines()[-1])
    if host_result['cases'] != input_count or host_result['bit_exact_calls'] != input_count:
        raise ValueError('Incomplete host differential')
    negatives = []
    for reason in ['reserved', 'unmasked_invalid', 'null_output', 'short_output', 'unregistered_output', 'out_of_range_output']:
        value = run([host, '--negative', reason], 73)
        if 'STOP Win64' not in value.stderr:
            raise ValueError('Negative case did not explicitly reject')
        negatives.append(dict(name=reason, exit=73, stderr=value.stderr))
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary = OUT / 'modff-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-fno-strict-aliasing',
         '-fwrapv', '-ffp-contract=off', '-ffunction-sections', '-fdata-sections',
         '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'), '-I' + str(OUT),
         fixture, candidate_path, '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-lm', '-o', binary])
    import pefile, capstone
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE identity drift')
    pe = pefile.PE(str(original), fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
    read = lambda va, size: pe.get_data(va - base, size)
    pe.parse_data_directories(directories=[1])
    imports = {i.address: (d.dll.decode(), i.name.decode() if i.name else None) for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports}
    if imports[0x141cc7df0] != ('api-ms-win-crt-math-l1-1-0.dll', 'modff'):
        raise ValueError('Original modff IAT identity')
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    actual = list(md.disasm(read(0x14189fd4b, 6), 0x14189fd4b))
    if len(actual) != 1 or actual[0].mnemonic != 'call' or actual[0].op_str != 'qword ptr [rip + 0x42809f]':
        raise ValueError('Actual direct IAT call gate')
    evidence = []
    for va, size in [(0x14189fd29, 0x48), (0x1410fa290, 0x30)]:
        raw = read(va, size)
        evidence.append(dict(va=hex(va), size=size, bytes=raw.hex(), sha256=hashlib.sha256(raw).hexdigest(),
            disassembly=[dict(va=hex(i.address), mnemonic=i.mnemonic, operands=i.op_str) for i in md.disasm(raw, va)]))
    if pins != {name: sha(ROOT / name) for name in names}:
        raise ValueError('Proof input changed during run')
    report = dict(success=True, production_modified=False, candidate_only=True, sources=pins,
        baseline_sha256=BASE_SHA, candidate=dict(path=str(candidate_path.relative_to(ROOT)), sha256=sha(candidate_path)),
        candidate_changed_only_fragment_and_dispatch_call=True, native_UCRT=native,
        native_ucrt_dll=dict(path='C:/Windows/System32/ucrtbase.dll', sha256=sha(Path('C:/Windows/System32/ucrtbase.dll'))),
        native_status_counts=status_counts, host=host_result, negative_stops=negatives, commands=commands,
        original_sha256=cfg['original_exe_sha256'], original_evidence=evidence,
        verified_IAT={'0x141cc7df0': imports[0x141cc7df0]},
        profile='Binary32 input, writable exactly4B integral output in RDX; full finite/Inf/qNaN/sNaN masked-invalid split. Unmasked sNaN invalid and reserved MXCSR reject before memory/CPU commit. Native UCRT does not flush DAZ/FTZ inputs; exact integral cancellation in[1,2^23) returns negative zero under RCdown, otherwise keeps input sign. Integral sNaN payload is preserved while fraction is quieted with IA. Other CPU/nativeFP/errno unchanged, including a deliberately errno-disturbing pointer validator; one dispatcher-owned pop.',
        validation_limit='Pointer boundary is the existing win64_pointer(address,4,1) contract; this isolated fixture supplies a strict test region implementation. Existing full-runtime region liveness proofs remain separate.',
        binary=str(binary.relative_to(ROOT)), binary_sha256=sha(binary), dataset=str(data.relative_to(ROOT)), dataset_sha256=sha(data),
        expected_ARM=dict(cases=input_count, failures=0, concurrent_cases=8192, guarded_output_cases=8, bit_exact_calls=input_count+8192,
            fullCPU_MXCSR_errno_nativeFP=True, single_import_pop=True),
        device_executed=False, original_game_executed=False)
    path = OUT / 'result.json'; path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ['success', 'host', 'binary', 'binary_sha256', 'dataset', 'dataset_sha256', 'expected_ARM']}, indent=2))
    print('receipt_sha256=' + sha(path)); print('fragment_sha256=' + sha(OUT / 'modff_fragment.inc'))


if __name__ == '__main__':
    main()
