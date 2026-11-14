"""Compile only: isolated native Vulkan partial-descriptor/range proof for Mali."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/mali-robustness-probe'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    cfg = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    llvm = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    glslang = ROOT / 'build/dxvk-tools/glslang/bin/glslang.exe'
    source = ROOT / 'tests/mali_robustness_probe.cpp'
    shader = ROOT / 'tests/mali_robustness.comp'
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in (source, shader, Path(__file__))}
    tools = {str(p): sha(p) for p in (llvm / 'clang++.exe', glslang, ndk / 'source.properties')}
    OUT.mkdir(parents=True, exist_ok=True)
    spv = OUT / 'mali_robustness.spv'
    subprocess.run([str(glslang), '-V', '--target-env', 'vulkan1.2', '-o', str(spv), str(shader)], check=True)
    words = struct.unpack('<' + 'I' * (spv.stat().st_size // 4), spv.read_bytes())
    branches = 0
    pos = 5
    while pos < len(words):
        size, opcode = words[pos] >> 16, words[pos] & 0xffff
        if size == 0 or pos + size > len(words):
            raise ValueError('Invalid SPIR-V instruction')
        branches += opcode == 250  # OpBranchConditional, not speculative OpSelect.
        pos += size
    if branches < 10:
        raise ValueError('Missing real shader guards')
    header = OUT / 'mali_robustness_spv.h'
    header.write_text('static const uint32_t shader_words[]={\n' +
                      ','.join(hex(w) + 'u' for w in words) + '\n};\n', encoding='ascii')
    binary = OUT / 'mali_robustness_probe'
    command = [str(llvm / 'clang++.exe'), '--target=aarch64-linux-android30', '-O2', '-std=c++17',
               '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
               '-I' + str(ROOT / 'reference/dxvk/include/vulkan/include'), '-I' + str(OUT),
               str(source), '-lvulkan', '-static-libstdc++', '-Wl,-z,max-page-size=16384', '-o', str(binary)]
    subprocess.run(command, check=True)
    if any(sha(ROOT / p) != h for p, h in inputs.items()) or any(sha(Path(p)) != h for p, h in tools.items()):
        raise ValueError('Probe inputs changed during compilation')
    report = {'success': True, 'compiled': True, 'device_executed': False,
              'source_hashes': inputs, 'tools_sha256': tools, 'shader_sha256': sha(spv),
              'shader_conditional_branches': branches, 'binary': str(binary.relative_to(ROOT)),
              'binary_sha256': sha(binary), 'runtime_provider': 'Android system libvulkan.so',
              'requirements': ['Mali-G615', 'descriptorBindingPartiallyBound'],
              'robustness2_enabled': False, 'robust_buffer_access_enabled': False,
              'unwritten_bindings': [3, 4, 6, 8], 'metadata_abi': {'logical_binding': 1216, 'records': 1216, 'stride': 8},
              'checks': ['unbound texture/query/SSBO/image/counter returns zero; stores and atomics no-op',
                         'per-component cross-boundary buffer load zeroes only unavailable words',
                         'descriptor range 12 bytes inside guarded 64-byte allocation',
                         'negative and positive image bounds, positive resource/atomic paths',
                         'readback checks 1026 shader words, 16 guarded buffer words and four image pixels']}
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
