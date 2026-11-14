"""Fast Android iteration: relink the runtime DSO and repackage the APK.

    python tools/fastbuild.py runtime [--extra-flags "..."]   # compile host C + link game objects
    python tools/fastbuild.py dex                             # android/app Java -> build/fast/dex/classes.dex
    python tools/fastbuild.py apk                             # base APK + build/fast/lib/*.so -> signed APK
    python tools/fastbuild.py install                         # adb install -r on the configured device

The source/object list comes from build/fast/runtime-inputs.json, seeded once
from the last accepted link response file. Host C files are compiled one by
one (cached by content and flags), then everything is linked by clang/lld.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
import android_launcher
import android_native_payload
import public_metadata
from build_gpu_runtime import sm8250_driver

ROOT = Path(__file__).resolve().parents[1]
FAST = ROOT / 'build/fast'
CONFIG = json.loads((ROOT / 'port.json').read_text())
SDK = Path(CONFIG['android_sdk'])
NDK = SDK / 'ndk' / CONFIG['android_ndk_version']
BIN = NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
CLANG = BIN / 'clang.exe'
BASE_APK = ROOT / 'build/fast/base.apk'
MACHINE = json.loads((Path.home() / '.agents/skills/adapt-arm32-game/machine_config.json').read_text())
JAVA = Path(MACHINE['java_exe'])
BUILD_TOOLS = Path(MACHINE['project_root']) / 'tools/android/sdk/build-tools' / MACHINE['build_tools_version']
SIGNER = Path(MACHINE['signer']['dir'])
INPUTS = FAST / 'runtime-inputs.json'
# Optional Mesa Turnip provider (selected in the shared launcher): adrenotools + hooks from
# tools/build_gpu_runtime.py and the pinned community Turnip build, loaded only when selected.
GPU_RUNTIME = ROOT / 'build/gpu-runtime/runtime'
GPU_LIBS = ('libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so', 'libfile_redirect_hook.so', 'libgsl_alloc_hook.so')
TURNIP_SHA256 = 'fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09'
QA_ONLY_LIBRARIES = {'libVkLayer_khronos_validation.so'}

COMPILE_FLAGS = ['--target=aarch64-linux-android30', '-O2', '-std=c11', '-fPIC', *android_native_payload.COMPILE_FLAGS, '-DFROSTPUNK_ANDROID_LIBRARY=1',
                 '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-fno-math-errno', '-g0',
                 '-Wall', '-Wextra', '-Wno-unused-label', '-Wno-unused-function',
                 '-DFROSTPUNK_HAVE_SUPPLEMENTAL=1', '-DFROSTPUNK_HAVE_TEXTURE_FORMATS=1',
                 '-DFROSTPUNK_HAVE_STRING_RESIZE=1', '-DFP_GPU_SYSTEM=1', '-DFROSTPUNK_HAVE_PAYLOAD=1']
INCLUDES = ['native/runtime64', 'generated/frostpunk-x64', 'generated/supplemental', 'generated/win64',
            'android/dxvk', 'android/gpu', 'reference/adrenotools/include', 'reference/dxvk/include/vulkan/include',
            'android/host']
LINK_FLAGS = ['-shared', *android_native_payload.LINK_FLAGS, '-Wl,-Bsymbolic-functions', '-Wl,-soname,libfrostpunk_runtime.so', '-Wl,--build-id=sha1', '-Wl,--no-undefined', '-Wl,-z,global',
              str(NDK / 'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'),
              '-ldl', '-landroid', '-lm',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_get_instance_proc_addr',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_get_display_mode',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_acquire_window',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_surface_state',
              '-Wl,--export-dynamic-symbol=frostpunk_frame_profile_present',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_mobile_effects',
              '-Wl,--export-dynamic-symbol=frostpunk_switch_ui',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_upscaler',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_upscaler_failed',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_scene_active',
              '-Wl,--export-dynamic-symbol=frostpunk_dxvk_object_highlight', '-Wl,-z,max-page-size=16384']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def seed_inputs():
    if INPUTS.exists():
        return json.loads(INPUTS.read_text())
    rows = [line.strip().strip('"') for line in (ROOT / 'build/startup/library-link.rsp').read_text().splitlines() if line.strip()]
    rel = lambda p: str(Path(p).resolve().relative_to(ROOT)).replace('\\', '/')
    data = dict(sources=[rel(p) for p in rows if p.endswith('.c')],
                objects=[rel(p) for p in rows if p.endswith('.o')],
                archives=[rel(p) for p in rows if p.endswith('.a')])
    FAST.mkdir(parents=True, exist_ok=True)
    INPUTS.write_text(json.dumps(data, indent=1) + '\n')
    return data


def header_digest():
    h = hashlib.sha256()
    for d in INCLUDES:
        for p in sorted((ROOT / d).glob('*.h')) + sorted((ROOT / d).glob('*.inc')):
            h.update(p.name.encode() + sha(p).encode())
    return h.hexdigest()


def compile_one(source, flags, headers):
    src = ROOT / source
    key = hashlib.sha256((sha(src) + json.dumps(flags) + headers).encode()).hexdigest()[:20]
    obj = FAST / 'obj' / (Path(source).stem + '-' + key + '.o')
    if obj.exists():
        return obj, False
    obj.parent.mkdir(parents=True, exist_ok=True)
    tmp = obj.with_suffix('.tmp')
    cmd = [str(CLANG), *flags, *['-I' + str(ROOT / d) for d in INCLUDES], '-c', str(src), '-o', str(tmp)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError('compile failed: %s\n%s' % (source, p.stderr[-8000:]))
    tmp.replace(obj)
    return obj, True


def build_runtime(extra, objects_override=None):
    data = seed_inputs()
    flags = COMPILE_FLAGS + extra
    headers = header_digest()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        results = list(pool.map(lambda s: compile_one(s, flags, headers), data['sources']))
    rebuilt = [str(o.name) for o, fresh in results if fresh]
    objects = [str(o) for o, _ in results]
    game = objects_override or [str(ROOT / o) for o in data['objects']]
    out = FAST / 'lib/libfrostpunk_runtime.so'
    out.parent.mkdir(parents=True, exist_ok=True)
    rsp = FAST / 'link.rsp'
    rsp.write_text('\n'.join('"' + Path(p).as_posix() + '"' for p in objects + game +
                             [str(ROOT / a) for a in data['archives']]))
    tmp = out.with_suffix('.tmp')
    p = subprocess.run([str(CLANG), '--target=aarch64-linux-android30', *[f for f in extra if f.startswith('-Wl') or f.startswith('-fuse')],
                        '@' + str(rsp), *LINK_FLAGS, '-o', str(tmp)], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError('link failed:\n' + p.stderr[-8000:])
    tmp.replace(out)
    print(json.dumps(dict(runtime=str(out), bytes=out.stat().st_size, sha256=sha(out), recompiled=rebuilt)))


def build_dex():
    """Java exactly as tools/build_android_game.py compiles it (same package and status-text edits)."""
    package = 'com.frostpunk.staticrecomp'
    platform = SDK / 'platforms/android-37.0/android.jar'
    work = FAST / 'java'
    shutil.rmtree(work, ignore_errors=True)
    (work / 'src').mkdir(parents=True)
    main = (ROOT / 'android/app/MainActivity.java').read_text(encoding='utf-8')
    for before, after in (('com.frostpunk.staticrecomp.probe', package),
                          ('Frostpunk native window probe\\nStarting native Vulkan/window fixture',
                           'Frostpunk static recompilation\\nWaiting for original game data and native runtime'),
                          ('if(status!=null)status.setText(text);',
                           'if(status!=null){status.setText(text);status.setVisibility(text.isEmpty()?View.GONE:View.VISIBLE);}')):
        if main.count(before) != 1:
            raise RuntimeError('MainActivity anchor drift: ' + before)
        main = main.replace(before, after)
    main = android_launcher.main_java(main)
    manifest = (ROOT / 'android/app/AndroidManifest.xml').read_text().replace('com.frostpunk.staticrecomp.probe', package)
    manifest = manifest.replace('Frostpunk native window probe', 'Frostpunk').replace('0.1-probe', '0.1-static-startup')
    manifest = manifest.replace('android:allowBackup="false"', 'android:allowBackup="false" android:allowNativeHeapPointerTagging="false"')
    prepared = android_launcher.prepare(FAST / 'launcher', manifest)
    shutil.copytree(Path(prepared['assets']), FAST / 'assets', dirs_exist_ok=True)
    (FAST / 'lib').mkdir(exist_ok=True)
    shutil.copyfile(prepared['probe_library'], FAST / 'lib/libportprobe.so')
    shutil.copyfile(prepared['texture_library'], FAST / 'lib/libfrostpunk_texconv.so')
    compiled = FAST / 'launcher/resources.zip'
    subprocess.run([str(BUILD_TOOLS / 'aapt2.exe'), 'compile', '--dir', str(ROOT / 'android/app/res'), '-o', str(compiled)], check=True)
    subprocess.run([str(BUILD_TOOLS / 'aapt2.exe'), 'link', '-I', str(platform), '--manifest', prepared['manifest'],
                    '-R', str(compiled), '-A', str(FAST / 'assets'), '-o', str(FAST / 'launcher/resources.apk')], check=True)
    sources = [work / 'src/MainActivity.java', work / 'src/TouchControls.java']
    sources[0].write_text(main, encoding='utf-8')
    sources[1].write_text((ROOT / 'android/app/TouchControls.java').read_text(encoding='utf-8')
                          .replace('com.frostpunk.staticrecomp.probe', package), encoding='utf-8')
    native_source = work / 'src/NativeLibrary.java'
    native_source.write_text((ROOT / 'android/app/NativeLibrary.java').read_text(encoding='utf-8')
                              .replace('com.frostpunk.staticrecomp.probe', package), encoding='utf-8')
    sources.append(native_source)
    support_source = work / 'src/PortSupport.java'
    support_source.write_text((ROOT / 'android/app/PortSupport.java').read_text(encoding='utf-8')
                              .replace('com.frostpunk.staticrecomp.probe', package), encoding='utf-8')
    sources.append(support_source)
    profile_source = work / 'src/PortProfile.java'
    profile_source.write_text((ROOT / 'android/app/PortProfile.java').read_text(encoding='utf-8')
                              .replace('com.frostpunk.staticrecomp.probe', package), encoding='utf-8')
    sources.append(profile_source)
    sources += [Path(p) for p in prepared['sources']]
    classes, dex = work / 'classes', FAST / 'dex'
    classes.mkdir()
    dex.mkdir(exist_ok=True)
    subprocess.run([str(JAVA.parent / 'javac.exe'), '--release', '8', '-encoding', 'UTF-8', '-classpath', str(platform),
                    '-d', str(classes), *map(str, sources)], check=True)
    subprocess.run([str(JAVA), '-cp', str(BUILD_TOOLS / 'lib/d8.jar'), 'com.android.tools.r8.D8', '--min-api', '30',
                    '--lib', str(platform), '--output', str(dex), *sorted(map(str, classes.rglob('*.class')))], check=True)
    print(json.dumps(dict(dex=str(dex / 'classes.dex'), sha256=sha(dex / 'classes.dex'))))


def build_app():
    """libfrostpunk_app.so with the CMake configuration of tools/build_android_game.py (system provider build)."""
    cmake = SDK / 'cmake/3.22.1/bin/cmake.exe'
    native = ROOT / 'build/app-game/native'  # same directory as build_android_game.py: the path is in the DWARF comp_dir
    subprocess.run([str(cmake), '-S', str(ROOT / 'android/app'), '-B', str(native), '-G', 'Ninja',
                    '-DCMAKE_MAKE_PROGRAM=' + str(cmake.parent / 'ninja.exe'),
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(NDK / 'build/cmake/android.toolchain.cmake'),
                    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30', '-DANDROID_STL=c++_static',
                    '-DCMAKE_BUILD_TYPE=Release', '-DGPU_RUNTIME=' + (ROOT / 'build/gpu-runtime/runtime').as_posix(),
                    '-DFP_GAME_RUNTIME=ON', '-DFP_DXVK_FIXTURE=OFF', '-DFP_NATIVE_PRESENT=OFF', '-DFP_GPU_SYSTEM=ON',
                    '-DFP_ORIGINAL_EXE_SHA256=' + CONFIG['original_exe_sha256']], check=True, capture_output=True)
    p = subprocess.run([str(cmake), '--build', str(native), '--parallel', '4'], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError('app build failed:\n' + p.stdout[-8000:] + p.stderr[-8000:])
    out = FAST / 'lib/libfrostpunk_app.so'
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(native / 'libfrostpunk_app.so', out)
    print(json.dumps(dict(app=str(out), sha256=sha(out))))


def build_apk(base):
    work = FAST / 'apk'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    unsigned = work / 'unsigned.apk'
    replaced = {}
    libs = {p.name: p for p in (FAST / 'lib').glob('*.so') if p.name not in QA_ONLY_LIBRARIES}
    fsr3 = ROOT / 'build/fsr3-android-sdk/libfrostpunk_fsr3.so'
    if not fsr3.exists():
        raise RuntimeError('optional FSR 3 module missing; run tools/build_fsr3.py')
    libs[fsr3.name] = fsr3
    # build/fast/assets/<name> replaces assets/<name> (game-inputs.tsv from prepare_android_port_settings_asset.py).
    assets = {p.name: p for p in (FAST / 'assets').glob('*') if p.is_file()}
    if sha(GPU_RUNTIME / 'vulkan.ad07xx.so') != TURNIP_SHA256:
        raise RuntimeError('pinned Turnip driver missing or changed; run tools/build_gpu_runtime.py')
    extra = {'lib/arm64-v8a/' + name: (GPU_RUNTIME / name, zipfile.ZIP_STORED) for name in GPU_LIBS}
    extra['assets/turnip/vulkan.ad07xx.so'] = (GPU_RUNTIME / 'vulkan.ad07xx.so', zipfile.ZIP_DEFLATED)
    extra['assets/turnip/vulkan.sm8250.so'] = (sm8250_driver(), zipfile.ZIP_DEFLATED)
    extra['lib/arm64-v8a/libfrostpunk_turnip.so'] = (GPU_RUNTIME / 'vulkan.ad07xx.so', zipfile.ZIP_STORED)
    extra.update({'assets/' + p.relative_to(FAST / 'assets').as_posix(): (p, zipfile.ZIP_DEFLATED)
                  for p in (FAST / 'assets').rglob('*') if p.is_file()})
    extra.update({'assets/licenses/' + p.name: (p, zipfile.ZIP_DEFLATED) for p in (GPU_RUNTIME / 'licenses').iterdir()})
    extra['assets/licenses/Turnip-origin.json'] = (ROOT / 'reference/turnip-android/provenance.json', zipfile.ZIP_DEFLATED)
    extra['assets/licenses/Turnip-SM8250-origin.json'] = (ROOT / 'reference/turnip-sm8250/provenance.json', zipfile.ZIP_DEFLATED)
    extra['assets/licenses/Turnip-SM8250-LICENSE-NOTE.txt'] = (ROOT / 'reference/turnip-sm8250/LICENSE-NOTE.txt', zipfile.ZIP_DEFLATED)
    extra['assets/licenses/FSR1.txt'] = (ROOT / 'reference/fsr1/license.txt', zipfile.ZIP_DEFLATED)
    extra['assets/licenses/FSR3.txt'] = (ROOT / 'reference/fsr3-android/LICENSE.txt', zipfile.ZIP_DEFLATED)
    extra['assets/licenses/FSR3-SDK.txt'] = (ROOT / 'reference/fsr3-android/sdk/LICENSE.txt', zipfile.ZIP_DEFLATED)
    public_fsr3 = work / 'FSR3-origin.json'
    public_fsr3.write_text(json.dumps(public_metadata.fsr3_contract(json.loads((ROOT / 'build/fsr3-android-sdk/build.json').read_text())),indent=2)+'\n')
    extra['assets/licenses/FSR3-origin.json'] = (public_fsr3,zipfile.ZIP_DEFLATED)
    extra['assets/licenses/stb.txt'] = (ROOT / 'reference/goldberg/stb/LICENCE', zipfile.ZIP_DEFLATED)
    public_game = work / 'game-data-contract.json'
    game_input = assets.get('game-data-contract.json')
    if game_input:
        public_game.write_text(json.dumps(public_metadata.game_contract(json.loads(game_input.read_text())),indent=2)+'\n')
        extra['assets/game-data-contract.json'] = (public_game,zipfile.ZIP_DEFLATED)
    public_runtime = work / 'runtime-provenance.json'
    private_runtime = json.loads((ROOT / 'research/runtime-library.json').read_text())
    runtime_binary = libs['libfrostpunk_runtime.so']
    public_runtime.write_text(json.dumps(public_metadata.runtime_contract(private_runtime,
        binary_sha256=sha(runtime_binary),binary_bytes=runtime_binary.stat().st_size),indent=2)+'\n')
    extra['assets/licenses/runtime-provenance.json'] = (public_runtime,zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(base) as source:
        for info in source.infolist():
            if info.filename.startswith('lib/arm64-v8a/') and info.filename.endswith('.so'):
                name = Path(info.filename).name
                if name not in libs and info.filename not in extra and name != 'libfrostpunk_xz.so' and name not in QA_ONLY_LIBRARIES:
                    path = work / 'base-libs' / name
                    path.parent.mkdir(exist_ok=True)
                    path.write_bytes(source.read(info))
                    libs[name] = path
    libs.update({Path(name).name: path for name, (path, _) in extra.items() if name.startswith('lib/arm64-v8a/')})
    extra = {name: item for name, item in extra.items() if not name.startswith('lib/arm64-v8a/')}
    native_entries, native_report = android_native_payload.prepare(libs, BIN, FAST / 'native-apk')
    extra.update(native_entries)
    (FAST / 'native-packaging.json').write_text(json.dumps(native_report, indent=2) + '\n')
    resources = zipfile.ZipFile(FAST / 'launcher/resources.apk')
    with zipfile.ZipFile(base) as src, zipfile.ZipFile(unsigned, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as dst, resources:
        for info in src.infolist():
            if (info.filename.startswith('META-INF/') or info.filename.startswith('assets/native/')
                    or info.filename == 'lib/arm64-v8a/libfrostpunk_runtime.so'
                    or info.filename.startswith('lib/arm64-v8a/') and Path(info.filename).name in QA_ONLY_LIBRARIES):
                continue
            name = info.filename.rsplit('/', 1)[-1]
            if info.filename in resources.namelist() and (info.filename == 'AndroidManifest.xml' or info.filename == 'resources.arsc' or info.filename.startswith('res/')):
                dst.writestr(info, resources.read(info.filename))
            elif info.filename == 'classes.dex' and (FAST / 'dex/classes.dex').exists():
                dst.write(FAST / 'dex/classes.dex', info.filename, compress_type=info.compress_type)
                replaced[info.filename] = sha(FAST / 'dex/classes.dex')
            elif info.filename in extra:
                path, compress = extra.pop(info.filename)
                dst.write(path, info.filename, compress_type=compress)
                replaced[info.filename] = sha(path)
            else:
                dst.writestr(info, src.read(info.filename), compress_type=info.compress_type)
        for name, (path, compress) in sorted(extra.items()):
            dst.write(path, name, compress_type=compress)
            replaced[name] = sha(path)
        for name in resources.namelist():
            if (name == 'resources.arsc' or name.startswith('res/')) and name not in src.namelist():
                dst.writestr(name, resources.read(name))
    aligned = work / 'aligned.apk'
    subprocess.run([str(BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
    apk = FAST / 'Frostpunk.apk'
    subprocess.run([str(JAVA), '-jar', str(SIGNER / MACHINE['signer']['apksigner']), 'sign',
                    '--key', str(SIGNER / MACHINE['signer']['key']), '--cert', str(SIGNER / MACHINE['signer']['cert']),
                    '--out', str(apk), str(aligned)], check=True)
    subprocess.run([str(JAVA), '-jar', str(SIGNER / MACHINE['signer']['apksigner']), 'verify', str(apk)], check=True)
    subprocess.run([str(BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(apk)], check=True)
    shutil.rmtree(work, ignore_errors=True)
    print(json.dumps(dict(apk=str(apk), sha256=sha(apk), replaced=replaced)))
    return apk


def install(apk):
    adb = [str(SDK / 'platform-tools/adb.exe'), '-s', CONFIG['device']]
    subprocess.run(adb + ['shell', 'am', 'force-stop', 'com.frostpunk.staticrecomp'], check=True)
    p = subprocess.run(adb + ['install', '-r', str(apk)], capture_output=True, text=True)
    print(p.stdout.strip(), p.stderr.strip())
    if p.returncode or 'Success' not in p.stdout:
        raise RuntimeError('install failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('steps', nargs='+', choices=('runtime', 'app', 'dex', 'apk', 'install'))
    ap.add_argument('--extra-flags', default='')
    ap.add_argument('--base-apk', default=str(BASE_APK))
    args = ap.parse_args()
    apk = FAST / 'Frostpunk.apk'
    for step in args.steps:
        if step == 'runtime':
            build_runtime(args.extra_flags.split())
        elif step == 'app':
            build_app()
        elif step == 'dex':
            build_dex()
        elif step == 'apk':
            apk = build_apk(Path(args.base_apk))
        elif step == 'install':
            install(apk)


if __name__ == '__main__':
    main()
