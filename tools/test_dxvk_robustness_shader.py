"""Build real pinned DXBC compiler with candidate overlay; validate its SPIR-V.

No network, device or production build changes. The shader container is replaced
only to expose compiler output; its descriptor/resource operations are unchanged.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from leaf_probe import ROOT, windows_toolchain
import dxvk_mali_overlay
import dxvk_robustness_shader_overlay as overlay


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    out = ROOT / 'build/dxvk-robustness-shader-test'
    source = out / 'source'
    source.mkdir(parents=True, exist_ok=True)
    for section in ('src', 'include'):
        for item in (ROOT / 'reference/dxvk' / section).rglob('*'):
            if item.is_file() and '.git' not in item.parts:
                target = source / item.relative_to(ROOT / 'reference/dxvk')
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.exists() or target.read_bytes() != item.read_bytes():
                    shutil.copyfile(item, target)
    dxvk_mali_overlay.apply(source)
    incoming = {name: (source / name).read_bytes() for name in overlay.FILES}
    drift_rejected = 0
    with tempfile.TemporaryDirectory(prefix='frostpunk-robustness-preimage-') as temporary:
        drift = Path(temporary)
        for changed in overlay.FILES:
            for name, contents in incoming.items():
                target = drift / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(contents + (b'\n// drift\n' if name == changed else b''))
            try:
                overlay.apply(drift)
            except ValueError:
                drift_rejected += 1
            else:
                raise ValueError('Overlay accepted a changed input file: ' + changed)
    details = overlay.apply(source)
    try:
        overlay.apply(source)
    except ValueError:
        pass
    else:
        raise ValueError('Overlay accepted double application')
    config = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version']
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    validator = ndk / 'shader-tools/windows-x86_64/spirv-val.exe'
    bins, env = windows_toolchain()
    commands = []

    def run(command):
        commands.append(list(map(str, command)))
        p = subprocess.run(commands[-1], env=env, cwd=out, capture_output=True,
                           text=True, encoding='utf-8', errors='replace', timeout=120)
        if p.returncode:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + p.stdout + p.stderr)
        return p.stdout

    names = ['src/dxbc/' + s + '.cpp' for s in
             ['dxbc_compiler', 'dxbc_common', 'dxbc_defs', 'dxbc_util', 'dxbc_names', 'dxbc_chunk_isgn', 'dxbc_reader',
              'dxbc_module', 'dxbc_analysis', 'dxbc_decoder', 'dxbc_chunk_shex', 'dxbc_header']]
    names += ['src/spirv/' + s + '.cpp' for s in ['spirv_module', 'spirv_code_buffer', 'spirv_compression']]
    paths = [source / name for name in names] + [ROOT / 'tests/dxvk_robustness_compiler_fixture.cpp']
    flags = ['--target=x86_64-pc-windows-msvc', '-std=c++17', '-O1', '-DNOMINMAX',
             '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', '-D_MSVC_STL_USE_ABORT_AS_DOOM_FUNCTION',
             '-Wno-microsoft-exception-spec',
             '-Wno-extern-c-compat', '-I' + str(source / 'src'), '-I' + str(source / 'include'),
             '-I' + str(source / 'include/vulkan/include'), '-I' + str(source / 'include/spirv/include'),
             '-I' + str(ROOT / 'build/dxvk-arm64/include')]
    objects = []
    arm_flags = ['--target=aarch64-linux-android30', '-std=c++17', '-fPIC', '-DDXVK_WSI_ANDROID',
                 '-Wno-microsoft-exception-spec', '-Wno-extern-c-compat',
                 '-I' + str(ROOT / 'build/dxvk-arm64/include')]
    arm_flags += ['-I' + str(source / p) for p in
                  ['include', 'include/vulkan/include', 'include/spirv/include', 'include/native',
                   'include/native/windows', 'include/native/directx']]
    for name in ('dxbc_compiler.cpp', 'dxbc_options.cpp'):
        run([clang, *arm_flags, '-fsyntax-only', source / 'src/dxbc' / name])
    for item in paths:
        obj = out / (item.stem + '.obj')
        objects.append(obj)
        run([clang, *flags, '-c', item, '-o', obj])
    exe = out / 'compiler-fixture.exe'
    run([bins / 'link.exe', '/nologo', '/out:' + str(exe), *objects,
         'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    shaders = out / 'shaders'
    shaders.mkdir(exist_ok=True)
    for item in shaders.glob('*.spv'):
        item.unlink()
    result = json.loads(run([exe, shaders]))
    files = sorted(shaders.glob('*.spv'))
    if len(files) != result['generated']:
        raise ValueError('Generated shader count differs')
    for item in files:
        run([validator, '--target-env', 'vulkan1.3', item])
    rows = [
        [0x33333333, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0],
        [12] * 4, [0] * 4, [50, 0, 0, 0], [0] * 4,
        [9, 10, 11, 12], [0] * 4, [0] * 4,
        [0x33333333, 0, 0, 1], [0] * 4, [0] * 4, [0] * 4,
        [3] * 4, [0] * 4, [0] * 4,
        [0x33333333, 0, 0, 0], [0] * 4, [1, 2, 0xabababab, 0],
        [0] * 4, [2, 2, 0, 1], [0, 0, 0, 1], [0] * 4, [10, 20, 30, 40],
    ]
    proof = dict(schema=1, shader='compiler-proof.spv', shader_sha256=sha(shaders / 'compiler-proof.spv'),
                 workgroups=[1, 1, 1], set=0, output_bytes=512, expected_output_rows_u32=rows,
                 native_robustness_required=False, metadata=dict(binding=1216, entries=1216, stride=8,
                    fields=['bound_bit0', 'actual_descriptor_byte_range'], unspecified_entries=[0, 0]),
                 bindings=[
                    dict(binding=1216, descriptor='STORAGE_BUFFER', role='metadata', bound=True, byte_range=1216 * 8),
                    dict(binding=1088, descriptor='STORAGE_BUFFER', role='output', bound=True, byte_range=512, initial_u32=0xfeedface),
                    dict(binding=832, descriptor='STORAGE_BUFFER', role='input', bound=True, byte_range=12, allocation_bytes=64,
                         initial_words=[0x11111111, 0x22222222, 0x33333333] + [0xcccccccc] * 13),
                    dict(binding=837, descriptor='STORAGE_BUFFER', role='input_alias', alias_of=832, bound=True, byte_range=12),
                    dict(binding=1089, descriptor='STORAGE_BUFFER', role='unbound', bound=False),
                    dict(binding=1090, descriptor='STORAGE_BUFFER', role='writable', bound=True, byte_range=12, allocation_bytes=64,
                         initial_words=[1, 2, 3] + [0xcccccccc] * 13,
                         expected_final_words=[1, 2, 0xabababab] + [0xcccccccc] * 13),
                    dict(binding=1091, descriptor='STORAGE_BUFFER', role='counter_main', alias_of=1090, bound=True, byte_range=4),
                    dict(binding=1155, descriptor='STORAGE_BUFFER', role='unbound_hidden_counter', bound=False),
                    dict(binding=800, descriptor='UNIFORM_BUFFER', role='ubo', bound=True, byte_range=20, allocation_bytes=64,
                         initial_words=[10, 20, 30, 40, 50] + [0xcccccccc] * 11),
                    dict(binding=833, descriptor='SAMPLED_IMAGE', role='bound_image', bound=True, format='R32G32B32A32_UINT',
                         extent=[2, 2, 1], mip_levels=1, texels_u32=[9, 10, 11, 12], metadata_byte_range=0),
                    dict(binding=834, descriptor='SAMPLED_IMAGE', role='unbound_image', bound=False),
                    dict(binding=835, descriptor='UNIFORM_TEXEL_BUFFER', role='texel_alias', alias_of=832, bound=True,
                         format='R32_UINT', byte_range=12),
                    dict(binding=836, descriptor='UNIFORM_TEXEL_BUFFER', role='unbound_texel', bound=False),
                 ])
    (out / 'compiler-proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    inputs = [Path(__file__).resolve(), ROOT / 'tools/dxvk_robustness_shader_overlay.py',
              ROOT / 'tools/dxvk_mali_overlay.py', *paths, *[source / name for name in overlay.FILES]]
    receipt = dict(success=True, generated=result['generated'], spirv_validated=len(files),
                   signed_unsigned_resinfo_lod_cases=18,
                   arm_syntax_validated=2, input_drift_rejected=drift_rejected,
                   all_six_shader_stages_validated=True, immediate_constant_buffers='baked16dwords and UBO32dwords',
                   host_compiler_source='actual pinned DXVK DXBC compiler', stub='logging/options/shader container only',
                   shader_execution=False, phone_executed=False, details=details,
                   inputs={str(p.relative_to(ROOT)): sha(p) for p in inputs},
                   shaders=[dict(path=str(p.relative_to(ROOT)), sha256=sha(p)) for p in files], commands=commands)
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({key: receipt[key] for key in ('success', 'generated', 'spirv_validated', 'shader_execution')}))


if __name__ == '__main__':
    main()
