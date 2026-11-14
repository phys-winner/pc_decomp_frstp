"""Test the isolated native-cache overlay; compile ARM, never invoke ADB."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

import build_dxvk
import dxvk_pipeline_cache_overlay as overlay
from leaf_probe import windows_toolchain

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/dxvk-pipeline-cache-test'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fresh_directory(path):
    path = Path(path).resolve()
    if path.parent != OUT.resolve() or path.name not in ('precache-source', 'source', 'partial-source', 'drift-inputs') or path.is_symlink():
        raise ValueError('Refuse to replace a non-fixture tree')
    if path.exists():
        shutil.rmtree(path)
    path.mkdir()


def reconstruct_precache(live, destination):
    """Preserve real WSI/build inputs; regenerate only the five overlay union."""
    fresh_directory(destination)
    for directory in ('src', 'include'):
        shutil.copytree(live / directory, destination / directory, ignore=shutil.ignore_patterns('.git'))
    for name in ('meson.build', 'meson_options.txt'):
        shutil.copy2(live / name, destination / name)
    base_overlays = build_dxvk.OVERLAYS[:-1]
    if build_dxvk.OVERLAYS[-1] is not overlay:
        raise ValueError('Actual cache overlay composition order changed')
    reset = set(overlay.INPUT_SHA256)
    reset.update(name for producer in base_overlays for name in producer.FILES)
    for name in reset:
        target, original = destination / name, build_dxvk.SOURCE / name
        if original.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, target)
        elif target.exists():
            target.unlink()
    for name in overlay.NEW_FILES:
        path = destination / name
        if path.exists():
            path.unlink()
    for producer in base_overlays:
        producer.apply(destination)
    overlay.verify_preimages(destination)
    # Files outside the reset union must retain actual platform-specific bytes.
    for directory in ('src', 'include'):
        for path in (live / directory).rglob('*'):
            if path.is_file() and '.git' not in path.parts:
                name = path.relative_to(live).as_posix()
                if name not in reset and name not in overlay.NEW_FILES and sha(path) != sha(destination / name):
                    raise ValueError('Non-overlay GPU input changed during reconstruction: ' + name)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    live_source = ROOT / 'build/dxvk-source'
    live_gpu_before = overlay.fingerprint(live_source)[1]
    source = OUT / 'precache-source'
    reconstruct_precache(live_source, source)
    identity, gpu_before, implementation = overlay.fingerprint(source)
    live_identity = live_source / overlay.NEW_FILES[2]
    if live_identity.exists() and ('FrostpunkPipelineCacheIdentityHex = "' + identity + '"') not in live_identity.read_text():
        raise ValueError('Reconstructed pre-cache identity differs from integrated production identity')
    files = [Path(__file__), ROOT / 'tests/dxvk_pipeline_cache_fixture.cpp',
             ROOT / 'tests/pipeline_cache_io_win32.h', ROOT / 'port.json',
             ROOT / 'build/dxvk-arm64/compile_commands.json', Path(build_dxvk.__file__),
             *(Path(producer.__file__) for producer in build_dxvk.OVERLAYS),
             *(ROOT / name for name in implementation)]
    before = {str(p.relative_to(ROOT)): sha(p) for p in files}
    candidate = OUT / 'source'
    fresh_directory(candidate)
    for directory in ('src', 'include'):
        shutil.copytree(source / directory, candidate / directory, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns('.git'))
    for name in ('meson.build', 'meson_options.txt'):
        shutil.copy2(source / name, candidate / name)
    report = overlay.apply(candidate, source)
    if report['calls'] != 14 or report['fingerprint_sha256'] != identity:
        raise ValueError('Native cache route/fingerprint mismatch')
    partial = OUT / 'partial-source'
    fresh_directory(partial)
    for name in overlay.INPUT_SHA256:
        path = partial / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, path)
    partial_report = overlay.apply(partial, gpu_source=source)
    if partial_report['outputs'] != report['outputs'] or partial_report['fingerprint_sha256'] != identity:
        raise ValueError('Partial candidate identity differs from full composed GPU tree')
    try:
        overlay.apply(partial, gpu_source=source)
    except ValueError:
        pass
    else:
        raise ValueError('Native cache overlay reapplication was accepted')
    shader_name = 'src/dxbc/dxbc_compiler.cpp'
    if overlay.fingerprint(source, {shader_name: (source / shader_name).read_bytes() + b'\n'})[0] == identity:
        raise ValueError('Shader compiler content change did not invalidate cache identity')
    if overlay.fingerprint(source, {'android/host/win64.c': b'CPU-only-change'})[0] != identity:
        raise ValueError('CPU-only change invalidated GPU cache identity')
    device = (candidate / 'src/dxvk/dxvk_device.h').read_text()
    if not device.rindex('m_vkd;') < device.rindex('m_pipelineCache;') < device.rindex('m_objects;'):
        raise ValueError('Native cache member lifetime drift')
    for route in report['routes']:
        text = (candidate / route['path']).read_text()
        if '->vkCreateGraphicsPipelines(' in text or '->vkCreateComputePipelines(' in text:
            raise ValueError('An existing pipeline creation bypasses the cache owner')
    # Drift gates are evaluated before any output writes.
    drift_tests = 0
    drift_dir = OUT / 'drift-inputs'
    fresh_directory(drift_dir)
    for name in overlay.INPUT_SHA256:
        path = drift_dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, path)
    for name in overlay.INPUT_SHA256:
        path = drift_dir / name
        raw = path.read_bytes()
        path.write_bytes(raw + b'\n')
        try:
            overlay.verify_preimages(drift_dir)
        except ValueError as error:
            if name not in str(error):
                raise
        else:
            raise ValueError('Exact preimage accepted a mutation: ' + name)
        path.write_bytes(raw)
        drift_tests += 1
    # GPU-source identity is content-derived, not a manually maintained ABI bump.
    identity_input = json.dumps({'revision': overlay.REVISION, 'gpu_sources': gpu_before,
                                'cache_implementation': implementation}, sort_keys=True, separators=(',', ':')).encode()
    if hashlib.sha256(identity_input).hexdigest() != identity:
        raise ValueError('Native cache identity is not reproducible')

    config = json.loads((ROOT / 'port.json').read_text())
    llvm = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = llvm / 'clang++.exe'
    bins, env = windows_toolchain()
    commands = []
    def run(command, **kwargs):
        commands.append(list(map(str, command)))
        result = subprocess.run(commands[-1], capture_output=True, text=True, timeout=90, **kwargs)
        if result.returncode:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + result.stdout + result.stderr)
        return result
    includes = ['-I' + str(candidate / 'src/dxvk'), '-I' + str(candidate / 'include/vulkan/include'),
                '-I' + str(ROOT / 'tests')]
    common = ['-O2', '-std=c++17', '-DFP_PIPELINE_CACHE_TEST=1', '-Wall', '-Wextra', '-Werror',
              '-Wno-missing-field-initializers', *includes]
    obj, binary = OUT / 'cache-host.obj', OUT / 'cache-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/EHsc', '/std:c++17', '/W4', '/WX',
         '/DFP_PIPELINE_CACHE_TEST=1', '/D_CRT_SECURE_NO_WARNINGS', '/DNOMINMAX',
         *['/I' + item[2:] for item in includes],
         '/c', ROOT / 'tests/dxvk_pipeline_cache_fixture.cpp', '/Fo' + str(obj)], env=env)
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(binary), obj,
         'libcpmt.lib', 'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'oldnames.lib', 'kernel32.lib'], env=env)
    scratch = OUT / ('scratch-' + str(os.getpid()))
    host_run = run([binary, scratch])
    host = json.loads(host_run.stdout)
    arm = OUT / 'cache-arm64'
    run([compiler, '--target=aarch64-linux-android30', *common,
         ROOT / 'tests/dxvk_pipeline_cache_fixture.cpp', '-static-libstdc++', '-pthread',
         '-Wl,-z,max-page-size=16384', '-o', arm])

    # Check actual integration TUs against their existing production ARM flags,
    # with only source/include paths redirected to the isolated candidate.
    compile_rows = json.loads((ROOT / 'build/dxvk-arm64/compile_commands.json').read_text())
    touched = {name for name in overlay.INPUT_SHA256 if name.endswith('.cpp')}
    syntax = []
    for row in compile_rows:
        resolved = (Path(row['directory']) / row['file']).resolve()
        try:
            relative = resolved.relative_to(live_source).as_posix()
        except ValueError:
            continue
        if relative not in touched:
            continue
        tokens = [token.strip('"') for token in shlex.split(row['command'], posix=False)]
        args = [str(compiler)]
        skip = False
        for token in tokens[1:]:
            if skip:
                skip = False
                continue
            if token in ('-MQ', '-MF', '-o'):
                skip = True
                continue
            if token in ('-MD', '-c'):
                continue
            if token.startswith('-I'):
                path = (Path(row['directory']) / token[2:]).resolve()
                if path.is_relative_to(live_source):
                    path = candidate / path.relative_to(live_source)
                args.append('-I' + str(path))
            elif token == row['file']:
                args.append(str(candidate / relative))
            else:
                args.append(token)
        args += ['-fsyntax-only', '-Werror', '-Wno-unused-private-field']
        run(args, cwd=row['directory'])
        syntax.append(relative)
    if set(syntax) != touched:
        raise ValueError('Not all isolated pipeline integration TUs were ARM syntax checked')
    if (before != {str(p.relative_to(ROOT)): sha(p) for p in files}
            or overlay.fingerprint(source)[0] != identity or overlay.fingerprint(live_source)[1] != live_gpu_before):
        raise ValueError('Native cache fixture/GPU inputs changed during validation')
    result = {'success': True, 'compiled': True, 'host': host,
              'host_binary_sha256': sha(binary), 'host_stderr': host_run.stderr,
              'arm': {'compiled': True, 'executed': False, 'binary': str(arm.relative_to(ROOT)),
                      'binary_sha256': sha(arm), 'argv': ['NEW_OWNED_SCRATCH_DIRECTORY'], 'expected': host},
              'overlay': report, 'source_hashes': before, 'compiler_sha256': sha(compiler),
              'host_compiler_sha256': sha(bins / 'cl.exe'),
              'preimage_gates': drift_tests, 'arm_syntax_checked': syntax, 'commands': commands,
              'partial_full_fingerprint_parity': True, 'reapply_rejected': True,
              'precache_reconstructed_from_integrated_live_tree': True,
              'non_overlay_WSI_inputs_preserved': True, 'live_gpu_source_hashes': live_gpu_before,
              'gpu_change_invalidates_cpu_change_preserves_identity': True,
              'device_actions': False, 'game_boot_proven': False,
              'scope': 'Exact production cache class with fake Vulkan PFNs; real bounded files, writer concurrency and teardown. Host Win32 file shim; ARM real POSIX.'}
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': host, 'arm': result['arm'],
                      'calls': report['calls'], 'syntax_units': len(syntax),
                      'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
