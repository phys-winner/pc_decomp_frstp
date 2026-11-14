"""Check exact backend overlay drift gates and compile its three native ARM units."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import dxvk_robustness_backend_overlay as backend
from leaf_probe import windows_toolchain

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/dxvk-robustness-backend-test'
SOURCE = ROOT / 'reference/dxvk'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(path):
    for header in (SOURCE / 'src/dxvk').glob('*.h'):
        target = path / 'src/dxvk' / header.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(header, target)
    for name in backend.FILES:
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(SOURCE / name, target)
    # This fixture tests only the backend half, never relaxes a running DXVK device.
    for name, text in [('dxbc_compiler.cpp', '// FROSTPUNK_ROBUSTNESS_ABI_1\n'),
                       ('dxbc_options.h', '// emulateRobustness2\n')]:
        target = path / 'src/dxbc' / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    work = OUT / 'source'
    inputs(work)
    before = {name: sha(SOURCE / name) for name in backend.FILES}
    backend.apply(work)
    rejected = 0
    for changed in (*backend.FILES, 'src/dxbc/dxbc_compiler.cpp', 'src/dxbc/dxbc_options.h'):
        with tempfile.TemporaryDirectory(prefix='backend-drift-', dir=OUT) as directory:
            test = Path(directory)
            inputs(test)
            path = test / changed
            if changed in backend.FILES:
                path.write_text('')
            else:
                path.write_text('// absent shader ABI\n')
            try:
                backend.apply(test)
            except ValueError:
                rejected += 1
            else:
                raise ValueError('Unverified preimage accepted: ' + changed)
    try:
        backend.apply(work)
    except ValueError:
        rejected += 1
    else:
        raise ValueError('Non-pristine repeated application accepted')
    candidate = (work / 'src/dxvk/dxvk_context.cpp').read_text()
    methods = backend.POLICY + candidate[candidate.index('  DxvkBufferSlice DxvkContext::createRobustnessMetadata('):
                                         candidate.index('  void DxvkContext::updateComputeShaderResources()')]
    (OUT / 'backend_methods.inc').write_text(methods)
    binaries, env = windows_toolchain()
    host = OUT / 'backend-fixture.exe'
    fixture = ROOT / 'tests/dxvk_robustness_backend_fixture.cpp'
    compiled = subprocess.run([str(binaries / 'cl.exe'), '/nologo', '/O2', '/EHsc', '/std:c++17', '/W3',
                               '/I' + str(OUT), '/I' + str(SOURCE / 'include/vulkan/include'), str(fixture),
                               '/Fe' + str(host)], cwd=OUT, env=env, capture_output=True, text=True)
    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    ran = subprocess.run([str(host)], capture_output=True, text=True, timeout=20)
    if ran.returncode:
        raise RuntimeError(ran.stdout + ran.stderr + ': ' + str(ran.returncode))
    host_result = json.loads(ran.stdout)
    if not host_result.get('success'):
        raise ValueError('Backend methods fixture did not pass')
    cfg = json.loads((ROOT / 'port.json').read_text())
    llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    include = [work / 'src/dxvk', ROOT / 'build/dxvk-arm64/include', ROOT / 'build/dxvk-source/src/dxvk',
               ROOT / 'build/dxvk-source/include', ROOT / 'build/dxvk-source/include/vulkan/include',
               ROOT / 'build/dxvk-source/include/spirv/include', ROOT / 'build/dxvk-source/include/native',
               ROOT / 'build/dxvk-source/include/native/windows', ROOT / 'build/dxvk-source/include/native/directx',
               ROOT / 'build/dxvk-arm64']
    commands = []
    objects = []
    for name in backend.FILES:
        if not name.endswith('.cpp'):
            continue
        obj = OUT / (Path(name).name + '.o')
        command = [str(llvm / 'clang++.exe'), '--target=aarch64-linux-android30', '-std=c++17', '-O2', '-fPIC',
                   '-DDXVK_WSI_ANDROID', '-D_FILE_OFFSET_BITS=64', '-Wall', '-Wextra',
                   '-Wno-missing-field-initializers', '-Wno-unused-parameter', '-Wno-unused-private-field',
                   '-Wno-microsoft-exception-spec', '-Wno-extern-c-compat', '-Wno-unused-const-variable',
                   *['-I' + str(p) for p in include], '-c', str(work / name), '-o', str(obj)]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
        (OUT / (Path(name).name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        commands.append(command)
        objects.append({'path': str(obj.relative_to(ROOT)), 'sha256': sha(obj)})
    if before != {name: sha(SOURCE / name) for name in before}:
        raise ValueError('Pristine source changed')
    report = {'success': True, 'abi': backend.ABI, 'drift_rejections': rejected,
              'upstream_files_sha256': before, 'overlay_sha256': {name: sha(work / name) for name in backend.FILES},
              'producer_sha256': sha(ROOT / 'tools/dxvk_robustness_backend_overlay.py'),
              'test_tool_sha256': sha(Path(__file__)), 'compiler_sha256': sha(llvm / 'clang++.exe'),
              'host': host_result, 'host_fixture_sha256': sha(fixture), 'host_executable_sha256': sha(host),
              'objects': objects, 'commands': commands, 'runtime_linked': False, 'device_executed': False,
              'shader_half_is_fixture_marker_only': True}
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['success', 'abi', 'drift_rejections', 'objects']}, indent=2))


if __name__ == '__main__':
    main()
