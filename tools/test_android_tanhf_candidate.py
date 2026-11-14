"""Isolated finite easing tanhf candidate: system UCRT, host and ARM CLI; no ADB."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import re
import struct
import subprocess
from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-tanhf-candidate'
ROW = struct.Struct('<IIIIIiIIII')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--oracle-only', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT / 'port.json').read_text())
    bins, env = windows_toolchain()
    commands = []

    def run(arguments, expected=0):
        command = list(map(str, arguments))
        commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=180)
        if result.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps(dict(command=command, expected=expected,
                actual=result.returncode, stdout=result.stdout, stderr=result.stderr), indent=2) + '\n')
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-7000:] + result.stderr[-7000:])
        return result

    original = Path(config['game_directory']) / 'Frostpunk.exe'
    if sha(original) != config['original_exe_sha256']:
        raise ValueError('Pinned original PE differs')
    import pefile
    pe = pefile.PE(str(original), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    imports = [(entry.dll.decode(), item.name.decode()) for entry in pe.DIRECTORY_ENTRY_IMPORT
               for item in entry.imports if item.address == 0x141cc7d50 and item.name]
    if imports != [('api-ms-win-crt-math-l1-1-0.dll', 'tanhf')]:
        raise ValueError('tanhf import namespace/slot drift')
    base = pe.OPTIONAL_HEADER.ImageBase
    read = lambda va, size: pe.get_data(va - base, size)
    expected = {0x141ae328e: bytes.fromhex('ff25bc4a1e00'),
                0x1410535aa: bytes.fromhex('e8dffca800'),
                0x1410c9d8a: bytes.fromhex('e8ff94a100'),
                0x141053597: bytes.fromhex('f30f5c15ed1c1a01f30f5915d51d1a010f28c2e8dffca8000f28d0f30f58d7f30f5915ce1c1a01'),
                0x1410c9d77: bytes.fromhex('f30f5c150db51201f30f5915f5b512010f28c2e8ff94a1000f28d0f30f58d7f30f5915eeb41201')}
    gates = []
    for va, raw in expected.items():
        if read(va, len(raw)) != raw:
            raise ValueError('Original tanhf boundary differs at ' + hex(va))
        gates.append(dict(va=hex(va), bytes=raw.hex(), sha256=hashlib.sha256(raw).hexdigest()))
    half, one, scale = (struct.unpack('<f', read(va, 4))[0] for va in (0x1421f528c, 0x1421f52b8, 0x1421f537c))
    if (half, one, scale) != (.5, 1., 10.367256164550781):
        raise ValueError('Easing constants differ')
    original_evidence = dict(import_slot='0x141cc7d50', imports=imports, gates=gates,
        easing_constants=dict(half=half, one=one, scale=scale),
        conditional_domain='Progress in [0,1]: (progress-.5)*10.367256164550781, tanhf, then (+1)*.5.',
        actual_startup_call_observed=False)

    snapshot = OUT / 'production-math-snapshot.c'
    identity = OUT / 'snapshot.json'
    if not snapshot.exists():
        source = ROOT / 'android/host/win64_math.c'
        snapshot.write_bytes(source.read_bytes())
        identity.write_text(json.dumps(dict(source='android/host/win64_math.c', sha256=sha(snapshot)), indent=2) + '\n')
    if sha(snapshot) != json.loads(identity.read_text())['sha256']:
        raise ValueError('Preserve isolated production snapshot')

    source_oracle = ROOT / 'tests/win64_angles_oracle.c'
    oracle = source_oracle.read_text()
    replacements = {
        'uint32_t kind,matherr_calls;} row;': 'uint32_t kind,matherr_calls,consumer,consumer_mxcsr;} row;',
        '_Static_assert(sizeof(row)==32,"angle row32");': '_Static_assert(sizeof(row)==40,"tanhf row40");',
        'float(__cdecl *atan_fn)(float,float)=(float(__cdecl*)(float,float))GetProcAddress(crt,"atan2f");':
            'float(__cdecl *fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"tanhf");',
        '    float(__cdecl *asin_fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"asinf");\n': '',
        '!atan_fn||!asin_fn': '!fn',
        '_mm_setcsr(in.mxcsr);*crt_errno()=1379;': '''_mm_setcsr(in.mxcsr);
        if(in.kind){
            __m128 progress=_mm_set_ss(x);
            __m128 shifted=_mm_sub_ss(progress,_mm_set_ss(0.5f));
            y=_mm_cvtss_f32(_mm_mul_ss(shifted,_mm_set_ss(10.367256164550781f)));
            memcpy(&out.y,&y,4);out.mxcsr=_mm_getcsr();
        }
        *crt_errno()=1379;''',
        'value=in.kind?asin_fn(y):atan_fn(y,x);': 'value=fn(y);',
        'if(GetLastError()!=0x5a17)return 6;memcpy(&out.result,&value,4);': '''if(GetLastError()!=0x5a17)return 6;memcpy(&out.result,&value,4);
        if(in.kind){
            float consumer=_mm_cvtss_f32(_mm_mul_ss(_mm_add_ss(_mm_set_ss(value),_mm_set_ss(1.0f)),_mm_set_ss(0.5f)));
            memcpy(&out.consumer,&consumer,4);out.consumer_mxcsr=_mm_getcsr();
        }'''}
    for old, new in replacements.items():
        if oracle.count(old) != 1:
            raise ValueError('Native oracle source gate: ' + old)
        oracle = oracle.replace(old, new, 1)
    oracle_path = OUT / 'tanhf_oracle.c'
    oracle_path.write_text(oracle)
    oracle_exe = OUT / 'tanhf-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', oracle_path,
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle_exe)])
    bits = lambda value: struct.unpack('<I', struct.pack('<f', value))[0]
    values = {0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000}
    values.update(bits((i - 4096) * 6. / 4096) for i in range(8193))
    rng = random.Random(0x141cc7d50)
    values.update(bits(rng.uniform(-6, 6)) for _ in range(4096))
    values.update(rng.randrange(0x40c00001) | (0x80000000 if i & 1 else 0) for i in range(4096))
    for value in [0x00800000, 0x03800000, 0x30800000, 0x31800000, 0x32800000,
                  0x33800000, 0x38800000, 0x39800000, 0x3f000000, 0x3f800000, bits(scale * .5), bits(6.)]:
        for delta in range(-8, 9):
            if 0 <= value + delta <= 0x40c00000:
                values.update([value + delta, (value + delta) | 0x80000000])
    inputs = OUT / 'inputs.bin'
    input_count = 0
    with inputs.open('wb') as stream:
        for mode in range(16):
            for index, value in enumerate(sorted(values)):
                csr = 0x1f80 | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0) | (0x15 if index & 1 else 0)
                stream.write(struct.pack('<IIII', value, 0, csr, 0)); input_count += 1
            for index in range(1025):
                progress = bits(index / 1024.)
                csr = 0x1f80 | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0)
                stream.write(struct.pack('<IIII', 0, progress, csr, 1)); input_count += 1
        for mask in range(64):
            for mode in range(16):
                for value in [0, 0x80000000]:
                    csr = (mask << 7) | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0)
                    stream.write(struct.pack('<IIII', value, 0, csr, 0)); input_count += 1
    data = OUT / 'cases.bin'
    native = json.loads(run([oracle_exe, inputs, data]).stdout)
    rows = list(ROW.iter_unpack(data.read_bytes()))
    if len(rows) != input_count or native['cases'] != input_count:
        raise ValueError('System UCRT row count differs')
    if args.oracle_only:
        seen = {}
        for y, x, result, csr, after, error, kind, calls, consumer, consumer_csr in rows:
            category = 'zero' if not y & 0x7fffffff else 'subnormal' if y & 0x7fffffff < 0x800000 else 'tiny' if y & 0x7fffffff < 0x39800000 else 'normal'
            key = (category, bool(csr & 64), bool(csr & 32768), after & ~csr, error, calls,
                   'same' if result == y else 'zero' if not result & 0x7fffffff else 'different')
            seen.setdefault(key, (hex(y), hex(result), hex(csr), hex(after)))
        print(json.dumps(dict(native=native, input_count=input_count,
            patterns=[dict(category=list(key), example=value) for key, value in seen.items()]), indent=2))
        return
    for y, x, result, csr, after, error, kind, calls, consumer, consumer_csr in rows:
        if after != csr | (32 if y & 0x7fffffff else 0) or error != 1379 or calls:
            raise ValueError('Measured finite tanhf status/errno/matherr policy differs')
        if y & 0x7fffffff < 0x38800000 and result != y:
            raise ValueError('Measured tiny tanhf raw-return policy differs')
    fragment_path = OUT / 'tanhf_fragment.inc'
    fixture = OUT / 'tanhf_fixture.c'
    fragment = fragment_path.read_text()
    baseline = snapshot.read_text()
    marker = 'static uint64_t integer_round('
    suffix = '||acosf_candidate_call(c,name)||atanf_call(c,name);'
    if baseline.count(marker) != 1 or baseline.count(suffix) != 1 or 'tanhf_candidate_call' in baseline:
        raise ValueError('Isolated production baseline insertion gate differs')
    candidate_text = baseline.replace(marker, fragment + '\n' + marker, 1).replace(suffix,
        '||acosf_candidate_call(c,name)||atanf_call(c,name)||tanhf_candidate_call(c,name);', 1)
    if candidate_text.replace(fragment + '\n', '', 1).replace(
            '||acosf_candidate_call(c,name)||atanf_call(c,name)||tanhf_candidate_call(c,name);', suffix, 1) != baseline:
        raise ValueError('Candidate does not reconstruct exact pinned baseline')
    candidate = OUT / 'win64_math_candidate.c'
    candidate.write_text(candidate_text)
    core_path = ROOT / 'android/host/win64.c'
    core = core_path.read_text()
    predicate = re.search(r'static int is_crt\(const char \*dll\) \{.*?\n.*?\}', core)
    route = '    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if not predicate or core.count(route) != 1:
        raise ValueError('Actual CRT namespace/math-pop route source gate differs')
    route_path = OUT / 'tanhf_route.inc'
    route_path.write_text(predicate.group(0) + '\nstatic int import_math(CPU*c,const char*dll,const char*name){int crt=is_crt(dll);\n' + route + '\nreturn 0;}\n')
    include = [ROOT / 'native/runtime64', ROOT / 'android/host', OUT]
    host = OUT / 'tanhf-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', '/experimental:c11atomics',
         *['/I' + str(path) for path in include], fixture, candidate,
         '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_result = run([host, data])
    (OUT / 'host.log').write_text(host_result.stdout + host_result.stderr)
    host_proof = json.loads(host_result.stdout)
    if not host_proof['success'] or host_proof['cases'] != input_count or host_proof['max_ULP'] > 2 or host_proof['failures']:
        raise ValueError('Candidate did not meet finite tanhf oracle bound')
    negatives = ['reserved', 'positive_range', 'negative_range', 'positive_infinity', 'negative_infinity',
                 'qNaN', 'sNaN', 'unmasked_precision', 'unmasked_subnormal']
    negative_results = []
    for name in negatives:
        failed = run([host, '--negative', name], expected=73)
        if 'STOP Win64 math' not in failed.stderr or 'tanhf' not in failed.stderr:
            raise ValueError('Missing typed tanhf failure diagnostic')
        negative_results.append(dict(arg=name, exit_code=73, stderr=failed.stderr))
    ndk = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = ndk / 'clang.exe'
    binary = OUT / 'tanhf-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-ffp-contract=off', '-fno-strict-aliasing',
         '-fwrapv', '-ffunction-sections', '-fdata-sections', *['-I' + str(path) for path in include], fixture, candidate,
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-lm', '-o', binary])
    disassembly = run([ndk / 'llvm-objdump.exe', '-d', '--no-show-raw-insn', binary])
    disassembly_path = OUT / 'arm-disassembly.txt'
    disassembly_path.write_text(disassembly.stdout)
    if sha(snapshot) != json.loads(identity.read_text())['sha256']:
        raise ValueError('Pinned isolated baseline changed during proof')
    sources = {str(path.relative_to(ROOT)): sha(path) for path in [snapshot, fragment_path, fixture,
        Path(__file__), source_oracle, core_path, ROOT / 'android/host/win64_math.h', ROOT / 'native/runtime64/cpu64.h']}
    report = dict(success=True, phase='native-oracle-host-proof-arm-compiled', sources=sources,
        baseline_source=json.loads(identity.read_text()), candidate=dict(path=str(candidate.relative_to(ROOT)), sha256=sha(candidate)),
        fragment_path=str(fragment_path.relative_to(ROOT)), fragment_sha256=sha(fragment_path),
        original=original_evidence, original_exe_sha256=config['original_exe_sha256'], native_oracle=native,
        native_status_policy='Every admitted finite nonzero raises PE32, no DE/UF; signed zero exact noflags; subnormal raw bits preserved independent of DAZ/FTZ; guest errno1379 and LastError0x5a17 unchanged; matherr not invoked.',
        host=host_proof, negative_cases=negative_results, commands=commands,
        dataset=dict(path=str(data.relative_to(ROOT)), sha256=sha(data), row_bytes=ROW.size, count=input_count),
        binary=dict(path=str(binary.relative_to(ROOT)), sha256=sha(binary), args=['cases.bin']),
        disassembly=dict(path=str(disassembly_path.relative_to(ROOT)), sha256=sha(disassembly_path)),
        scope='Isolated finite [-6,6] tanhf only. Original conditional easingmode4 progress[0,1] has magnitude <=5.1836281; not observed on actual startup. Tested ULP bound2, exact status/fullCPU/errno/nativeFP, source-extracted CRT namespace and one outer pop. Composedconsumer absoluteerror <=2^-23, not ULP bound near cancellation. Unmasked precision, reserved MXCSR and all values outside this finite band failclosed. No general tanhf parity claim.',
        fixture_scope='Actual candidate and full pinned math baseline; exact current core CRT predicate and math-pop route only. Other core provider/dependency initialization not simulated or claimed.',
        promotion='Fragment-only review onto then-current production math; never overwrite production with this snapshot.',
        device_executed=False, original_bytes_executed=False, production_modified=False)
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(success=True, host=host_proof, dataset=report['dataset'], binary=report['binary'],
                         receipt=str(receipt), receipt_sha256=sha(receipt)), indent=2))


if __name__ == '__main__':
    main()
