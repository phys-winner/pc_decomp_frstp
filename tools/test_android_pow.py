"""UCRT codec-pow oracle and actual portable provider fixture. Never uses ADB."""
import hashlib
import json
from pathlib import Path
import random
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-pow-test'
ROW = struct.Struct('<QQQIIiiIi')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_math.c', 'android/host/win64_math.h', 'native/runtime64/cpu64.h',
             'tests/win64_pow_oracle.c', 'tests/win64_pow_fixture.c', 'tools/test_android_pow.py']
    before = {name: sha(ROOT / name) for name in files}
    commands = []

    def run(args, expected=0):
        command = list(map(str, args));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-5000:] + result.stderr[-5000:])
        return result

    import pefile
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE changed')
    pe = pefile.PE(str(original), fast_load=True)
    address = 0x141C5677A
    # Gate only the actual scalar argument-construction block and pow call.
    # Keep the full instruction bytes in the receipt, and independently verify
    # each mnemonic/operand against the decoded original (never execute it).
    import capstone
    raw = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, 42)
    decoded = [(i.address, i.mnemonic, i.op_str) for i in capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64).disasm(raw, address)]
    wanted = [('movss','xmm2, dword ptr [rip + 0x59eb36]'), ('mov','edi, dword ptr [rcx]'),
              ('mov','ebx, dword ptr [rcx + 4]'), ('movd','xmm1, edi'), ('cvtdq2ps','xmm1, xmm1'),
              ('movd','xmm0, ebx'), ('divss','xmm2, xmm1'), ('cvtdq2ps','xmm0, xmm0'),
              ('cvtps2pd','xmm1, xmm2'), ('cvtps2pd','xmm0, xmm0'), ('call','0x141ae32d0')]
    if [(m, o) for _, m, o in decoded] != wanted:
        raise ValueError('Original pow argument-construction block changed: ' + repr(decoded))
    if pe.get_data(0x1421F52B8 - pe.OPTIONAL_HEADER.ImageBase, 4) != struct.pack('<f', 1.0):
        raise ValueError('Original reciprocal numerator changed')
    thunk = pe.get_data(0x141AE32D0 - pe.OPTIONAL_HEADER.ImageBase, 6)
    if thunk[:2] != b'\xff\x25' or 0x141AE32D6 + struct.unpack('<i', thunk[2:])[0] != 0x141CC7DB0:
        raise ValueError('Original pow IAT thunk changed')
    route = '    if (crt && win64_math_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Production CRT import route changed')
    (OUT / 'pow_route.inc').write_text(route + '\n')
    pairs = {(x, d) for x in [0,1,2,3,4,7,8,9,15,16,17,26,27,28,63,64,65,255,256,257,1000,
                               65535,65536,65537,16777213,16777214,16777215,16777216,16777217,2147483647]
             for d in [1,2,3,4,5,8,16,24,31,32,64,255,256,1024,65535,2147483647]}
    for d in range(1,25):
        for root in range(1,24):
            value = root ** d
            if value <= 16777215:
                pairs.update((value + offset, d) for offset in (-1,0,1) if 0 <= value + offset <= 16777215)
    rng = random.Random(0x141C56770)
    pairs.update((rng.randrange(1,16777216), rng.choice([1,2,3,4,5,8,16,24,32,64,255,65535])) for _ in range(2048))
    inputs = OUT / 'inputs.bin'
    with inputs.open('wb') as stream:
        for index, (entries, dimension) in enumerate(sorted(pairs)):
            for mode in range(16):
                csr = 0x1f80 | ((mode & 3) << 13) | (64 if mode & 4 else 0) | (32768 if mode & 8 else 0)
                if index & 1: csr |= 0x15
                stream.write(struct.pack('<iiII', entries, dimension, csr, (index + mode) & 1))
    oracle = OUT / 'pow-oracle.exe'
    run([bins / 'cl.exe','/nologo','/O2','/fp:strict','/std:c11',ROOT / 'tests/win64_pow_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle)])
    dataset = OUT / 'cases.bin'
    oracle_result = json.loads(run([oracle, inputs, dataset]).stdout)
    records = list(ROW.iter_unpack(dataset.read_bytes()))
    for x,y,result,csr,_,_,_,expected_csr,expected_errno in records:
        flags = 0 if x in (0,0x3ff0000000000000) or y == 0x3ff0000000000000 else 32
        if expected_csr != csr | flags or expected_errno != 1379:
            raise ValueError('UCRT codec exception/errno policy differs: ' + repr((x,y,csr,expected_csr,expected_errno)))
    host = OUT / 'pow-host.exe'
    run([bins / 'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/I' + str(ROOT / 'native/runtime64'),
         '/I' + str(ROOT / 'android/host'),'/I' + str(OUT),ROOT / 'tests/win64_pow_fixture.c',
         ROOT / 'android/host/win64_math.c','/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_run = run([host, dataset]);host_result = json.loads(host_run.stdout)
    (OUT / 'host.log').write_text(host_run.stdout + host_run.stderr)
    negatives = ['precision','reserved','negative','negative_zero','fraction','subnormal','NaN','infinity',
                 'negative_power','large_base','large_power','wide_power']
    negative_results = []
    for name in negatives:
        result = run([host, '--negative', name], 73)
        if 'MATH_INPUT name=pow x=0x' not in result.stderr or 'y=0x' not in result.stderr:
            raise ValueError('Missing bit-precise pow rejection diagnostic')
        negative_results.append({'case':name, 'exit_code':73, 'stderr':result.stderr})
    binary = OUT / 'pow-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-ffp-contract=off', '-fno-strict-aliasing',
         '-ffunction-sections','-fdata-sections','-I' + str(ROOT / 'native/runtime64'),'-I' + str(ROOT / 'android/host'),
         '-I' + str(OUT),ROOT / 'tests/win64_pow_fixture.c',ROOT / 'android/host/win64_math.c',
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    if before != {name:sha(ROOT / name) for name in files}:
        raise ValueError('Pow sources changed during fixtures')
    report = {'success':True,'phase':'host-oracle-arm-build','sources':before,'commands':commands,
              'original':{'sha256':cfg['original_exe_sha256'],'argument_block_va':hex(address),
                          'bytes':raw.hex(),'decoded':decoded,'pow_iat':'0x141cc7db0','thunk_bytes':thunk.hex()},
              'oracle':oracle_result,'host':host_result,'negative_cases':negative_results,
              'dataset':{'path':str(dataset.relative_to(ROOT)),'sha256':sha(dataset),'row_bytes':48,'count':len(records)},
              'binary':{'path':str(binary.relative_to(ROOT)),'sha256':sha(binary),'args':['cases.bin']},
              'device_executed':False,'scope':'nonnegative binary32 integer-base, widened binary32 reciprocal-dimension codec estimate; <=4 ULP; corrected original signed32 consumer checked only with nonoverflow products; no general pow parity'}
    path=OUT / 'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'host':host_result,'binary':report['binary'],'dataset':report['dataset']},indent=2))


if __name__ == '__main__':
    main()
