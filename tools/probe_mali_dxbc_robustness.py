"""Compile only: execute the actual guarded DXBC compiler's focused compute SPIR-V."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/mali-dxbc-robustness-probe'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def array(name, values):
    return 'static const uint32_t ' + name + '[]={' + ','.join(hex(x) + 'u' for x in values) + '};\n'


def main():
    proof_path = ROOT / 'build/dxvk-robustness-shader-test/compiler-proof.json'
    proof = json.loads(proof_path.read_text())
    spv = proof_path.parent / 'shaders' / proof['shader']
    if sha(spv) != proof['shader_sha256'] or proof['metadata'] != {
            'binding': 1216, 'entries': 1216, 'stride': 8, 'fields': ['bound_bit0', 'actual_descriptor_byte_range'],
            'unspecified_entries': [0, 0]}:
        raise ValueError('Actual compiler shader/proof ABI changed')
    bindings = proof['bindings']
    if {x['binding'] for x in bindings} != {1216,1088,832,837,1089,1090,1091,1155,800,833,834,835,836}:
        raise ValueError('Focused proof descriptor contract changed')
    roles = {'metadata': 0, 'output': 1, 'input': 2, 'input_alias': 2, 'writable': 3, 'counter_main': 3,
             'ubo': 4, 'bound_image': 5, 'texel_alias': 6, 'unbound': 7,
             'unbound_hidden_counter': 7, 'unbound_image': 7, 'unbound_texel': 7}
    OUT.mkdir(parents=True, exist_ok=True)
    words = struct.unpack('<' + 'I' * (spv.stat().st_size // 4), spv.read_bytes())
    text = array('compiler_shader_words', words)
    text += 'struct ProofBinding { uint32_t slot;VkDescriptorType type;bool bound;uint64_t bytes;unsigned resource; };\n'
    text += 'static const ProofBinding proof_bindings[]={\n'
    for binding in bindings:
        text += '{%du,VK_DESCRIPTOR_TYPE_%s,%s,%du,%du},\n' % (
            binding['binding'], binding['descriptor'], str(binding['bound']).lower(),
            binding.get('byte_range', 0), roles[binding['role']])
    text += '};\n'
    by_slot = {x['binding']: x for x in bindings}
    expected = [word for row in proof['expected_output_rows_u32'] for word in row]
    if len(expected) != 100 or proof['output_bytes'] != 512:
        raise ValueError('Expected exactly 25 output vectors with 112 bytes of canaries')
    expected += [by_slot[1088]['initial_u32']] * (128 - len(expected))
    text += array('expected_output', expected)
    for slot, name in [(832,'initial_input'),(1090,'initial_writable'),(800,'initial_ubo')]:
        text += array(name, by_slot[slot]['initial_words'])
    text += array('expected_writable', by_slot[1090]['expected_final_words'])
    (OUT / 'compiler_proof.h').write_text(text)
    cfg = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    source = ROOT / 'tests/mali_dxbc_robustness_probe.cpp'
    dependencies = [source, ROOT / 'tests/mali_robustness_probe.cpp', proof_path, spv, Path(__file__),
                    ROOT / 'build/mali-robustness-probe/mali_robustness_spv.h',
                    ROOT / 'build/dxvk-robustness-shader-test/result.json']
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in dependencies}
    binary = OUT / 'mali_dxbc_robustness_probe'
    command = [str(clang), '--target=aarch64-linux-android30', '-O2', '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-missing-field-initializers', '-I' + str(ROOT / 'reference/dxvk/include/vulkan/include'),
               '-I' + str(ROOT / 'build/mali-robustness-probe'), '-I' + str(OUT), str(source),
               '-lvulkan', '-static-libstdc++', '-Wl,-z,max-page-size=16384', '-o', str(binary)]
    subprocess.run(command, check=True)
    if any(sha(ROOT / name) != digest for name, digest in inputs.items()):
        raise ValueError('Actual compiler proof inputs changed during compilation')
    report = {'success': True, 'compiled': True, 'device_executed': False, 'source_hashes': inputs,
              'binary': str(binary.relative_to(ROOT)), 'binary_sha256': sha(binary),
              'compiler_sha256': sha(clang), 'actual_dxbc_compiler_spirv_sha256': sha(spv),
              'bindings': bindings, 'checks': 176, 'output_vectors': 25, 'output_canary_words': 28,
              'robustness2_enabled': False, 'robust_buffer_access_enabled': False,
              'partially_bound': True, 'metadata_abi': 1}
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('success','binary','binary_sha256','actual_dxbc_compiler_spirv_sha256','checks')}, indent=2))


if __name__ == '__main__':
    main()
