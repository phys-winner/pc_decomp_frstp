"""Incremental release runtime using the validated local AOT object cache.

The generated game objects, link inputs and base APKs are private build inputs;
see README.md for the build boundary. Entry point: tools/build_release.py.
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

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fastbuild as fb  # noqa: E402
import android_native_payload as payload  # noqa: E402
import android_launcher  # noqa: E402

OUT = ROOT / 'build/release-runtime'
CANONICAL = ROOT / 'build/fast/runtime-inputs.json'
BASE_APK = ROOT / 'build/fast/Frostpunk.apk'
FL18_APK = ROOT / 'build/fastlift/fl18/Frostpunk-fl18.apk'
EXTRA_HOST = ['android/host/mobile_lod.c', 'android/host/thread_placement.c']
ARCHIVES = {'build/dxvk-bridge/libwin64_d3d11.a': 'build/ui-20261114/fl/host/libwin64_d3d11-fastlift.a',
            'build/openal-bridge/libwin64_openal.a': 'build/fastlift/host/win64_openal.o'}
# Source overrides of this task (canonical generated wrappers are copied before being edited).
SOURCE_OVERRIDE = {'build/fast/mobile-objects/wrappers.c': 'android/host/mobile_wrappers.c',
                   'reference/launcher-zipmount/zipmount.c': 'android/launcher-base/native/zipmount.c'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compile_sources(sources):
    headers = fb.header_digest()
    flags = fb.COMPILE_FLAGS

    def one(source):
        actual = SOURCE_OVERRIDE.get(source, source)
        src = ROOT / actual
        key = hashlib.sha256((sha(src) + json.dumps(flags) + headers).encode()).hexdigest()[:20]
        obj = OUT / 'obj' / (Path(source).stem + '-' + key + '.o')
        if obj.exists():
            return obj, False
        obj.parent.mkdir(parents=True, exist_ok=True)
        tmp = obj.with_suffix('.tmp')
        cmd = [str(fb.CLANG), *flags, *['-I' + str(ROOT / d) for d in fb.INCLUDES], '-c', str(src), '-o', str(tmp)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode:
            raise RuntimeError('compile failed: %s\n%s' % (actual, done.stderr[-8000:]))
        tmp.replace(obj)
        return obj, True

    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        results = list(pool.map(one, sources))
    print('host objects: %d, recompiled %s' % (len(results), [o.name for o, fresh in results if fresh]), flush=True)
    return [o for o, _ in results]


def unit_objects(rows):
    """Canonical object rows with every lifted Frostpunk.exe unit replaced by its fastlift object."""
    return fb.production_game_objects(rows)


def link(name):
    data = json.loads(CANONICAL.read_text())
    host = compile_sources(data['sources'] + EXTRA_HOST)
    game = unit_objects(data['objects'])
    archives = [str(ROOT / ARCHIVES.get(a, a)) for a in data['archives']]
    folder = OUT / name
    folder.mkdir(parents=True, exist_ok=True)
    rsp = folder / 'link.rsp'
    rsp.write_text('\n'.join('"' + Path(p).as_posix() + '"' for p in [*map(str, host), *game, *archives]))
    runtime = folder / 'libfrostpunk_runtime.so'
    done = subprocess.run([str(fb.CLANG), '--target=aarch64-linux-android30', '@' + str(rsp), *fb.LINK_FLAGS, '-o', str(runtime)],
                          capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError('link failed:\n' + done.stderr[-8000:])
    print('runtime %s %d bytes sha256 %s' % (runtime, runtime.stat().st_size, sha(runtime)), flush=True)
    return runtime


def build_dex():
    """fastbuild.build_dex with private output folders (build/fast is left alone)."""
    package = 'com.frostpunk.staticrecomp'
    platform = fb.SDK / 'platforms/android-37.0/android.jar'
    work = OUT / 'java'
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
    prepared = android_launcher.prepare(OUT / 'launcher', manifest)
    assets = OUT / 'launcher-assets'
    shutil.rmtree(assets, ignore_errors=True)
    shutil.copytree(Path(prepared['assets']), assets)
    compiled = OUT / 'launcher/resources.zip'
    subprocess.run([str(fb.BUILD_TOOLS / 'aapt2.exe'), 'compile', '--dir', str(ROOT / 'android/app/res'), '-o', str(compiled)], check=True)
    subprocess.run([str(fb.BUILD_TOOLS / 'aapt2.exe'), 'link', '-I', str(platform), '--manifest', prepared['manifest'],
                    '-R', str(compiled), '-A', str(assets), '-o', str(OUT / 'launcher/resources.apk')], check=True)
    sources = [work / 'src/MainActivity.java']
    sources[0].write_text(main, encoding='utf-8')
    for name in ('TouchControls', 'NativeLibrary', 'PortSupport', 'PortProfile', 'GameWatchActivity', 'CrashReport'):
        target = work / ('src/%s.java' % name)
        target.write_text((ROOT / ('android/app/%s.java' % name)).read_text(encoding='utf-8')
                          .replace('com.frostpunk.staticrecomp.probe', package), encoding='utf-8')
        sources.append(target)
    sources += [Path(p) for p in prepared['sources']]
    classes, dex = work / 'classes', OUT / 'dex'
    classes.mkdir()
    shutil.rmtree(dex, ignore_errors=True)
    dex.mkdir()
    subprocess.run([str(fb.JAVA.parent / 'javac.exe'), '--release', '8', '-encoding', 'UTF-8', '-classpath', str(platform),
                    '-d', str(classes), *map(str, sources)], check=True)
    subprocess.run([str(fb.JAVA), '-cp', str(fb.BUILD_TOOLS / 'lib/d8.jar'), 'com.android.tools.r8.D8', '--min-api', '30',
                    '--lib', str(platform), '--output', str(dex), *sorted(map(str, classes.rglob('*.class')))], check=True)
    print('dex', dex / 'classes.dex', sha(dex / 'classes.dex'), flush=True)


def build_app():
    cmake = fb.SDK / 'cmake/3.22.1/bin/cmake.exe'
    native = ROOT / 'build/app-game/native'  # the canonical folder: the path is in the DWARF comp_dir
    subprocess.run([str(cmake), '-S', str(ROOT / 'android/app'), '-B', str(native), '-G', 'Ninja',
                    '-DCMAKE_MAKE_PROGRAM=' + str(cmake.parent / 'ninja.exe'),
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(fb.NDK / 'build/cmake/android.toolchain.cmake'),
                    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30', '-DANDROID_STL=c++_static',
                    '-DCMAKE_BUILD_TYPE=Release', '-DGPU_RUNTIME=' + (ROOT / 'build/gpu-runtime/runtime').as_posix(),
                    '-DFP_GAME_RUNTIME=ON', '-DFP_DXVK_FIXTURE=OFF', '-DFP_NATIVE_PRESENT=OFF', '-DFP_GPU_SYSTEM=ON',
                    '-DFP_ORIGINAL_EXE_SHA256=' + fb.CONFIG['original_exe_sha256']], check=True, capture_output=True)
    done = subprocess.run([str(cmake), '--build', str(native), '--parallel', '4'], capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError('app build failed:\n' + done.stdout[-8000:] + done.stderr[-8000:])
    target = OUT / 'app/libfrostpunk_app.so'
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(native / 'libfrostpunk_app.so', target)
    print('app', target, sha(target), flush=True)


# The one PanVK build of all ports (owner's decision of 24 November 2026): the shared folder
# launcher/drivers/panvk beside the project, described by its MANIFEST.json. It is the build of
# android/gpu/panvk-r10.patch with tools/build_panvk.sh of 20 November (a superset of the 6e85bc6f… build
# BioShock pinned). PORT_PANVK may name another copy of that folder; the hash below stays pinned.
PANVK_SHA256 = '04bde80d79916b6934fda15cc2aeb0a59e2a395a4eb564f3a7dece35d5e54492'


def panvk_payload():
    directory = Path(os.environ.get('PORT_PANVK') or ROOT.parent / 'launcher/drivers/panvk')
    manifest_path = directory / 'MANIFEST.json'
    if not manifest_path.is_file():
        raise RuntimeError('No shared PanVK folder with MANIFEST.json at ' + str(directory))
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    driver = directory / manifest['library']['file']
    notice = directory / manifest['licence']['file']
    if manifest['library']['sha256'] != PANVK_SHA256:
        raise RuntimeError('The shared PanVK manifest names ' + manifest['library']['sha256'] + ', the port pins ' + PANVK_SHA256)
    if not driver.is_file() or sha(driver) != PANVK_SHA256:
        raise RuntimeError('The shared PanVK library does not match its manifest: ' + str(driver))
    if not notice.is_file() or sha(notice) != manifest['licence']['sha256']:
        raise RuntimeError('The shared PanVK notice does not match its manifest: ' + str(notice))
    return {'lib/arm64-v8a/libvulkan_panfrost.so': (driver, zipfile.ZIP_DEFLATED),
            'assets/licenses/PanVK-JimVulkan-NOTICE.txt': (notice, zipfile.ZIP_DEFLATED)}


def package(runtime, name, dxvk=None, native_libraries=()):
    folder = OUT / name
    libraries = {p.name: p for p in (runtime, *native_libraries)}
    app = OUT / 'app/libfrostpunk_app.so'
    if app.exists():
        libraries[app.name] = app
    codec = ROOT / 'build/texture-converter/libfrostpunk_texconv.so'
    if codec.exists():
        libraries[codec.name] = codec
    services = ROOT / 'build/offline-services/runtime/libfrostpunk_offline_services.so'
    if services.exists():
        libraries[services.name] = services
    entries, packaging = payload.prepare(libraries, fb.BIN, folder / 'payload')
    replace = dict(entries)
    replace.update(panvk_payload())
    if fb.sha(fb.GPU_RUNTIME / 'vulkan.ad07xx.so') != fb.TURNIP_SHA256:
        raise RuntimeError('Pinned Turnip driver missing or changed')
    # Android extracts native libraries at install time (extractNativeLibs=true).
    replace['lib/arm64-v8a/libpurple_turnip.so'] = (fb.GPU_RUNTIME / 'vulkan.ad07xx.so', zipfile.ZIP_DEFLATED)
    replace.update({'lib/arm64-v8a/' + name: (fb.GPU_RUNTIME / name, zipfile.ZIP_DEFLATED) for name in fb.GPU_LIBS})
    replace.update({'assets/licenses/' + p.name: (p, zipfile.ZIP_DEFLATED) for p in (fb.GPU_RUNTIME / 'licenses').iterdir()})
    with zipfile.ZipFile(FL18_APK) as fl18:
        # The base APK's DXVK libraries pass through here unchanged; tools/build_release.py drops them.
        for lib in ('lib/arm64-v8a/libdxvk_d3d11.so', 'lib/arm64-v8a/libdxvk_dxgi.so'):
            path = folder / Path(lib).name
            # --dxvk: libraries relinked by build/ui-20261114/dxvk_cache/build.py (pipeline cache fix)
            path.write_bytes((Path(dxvk) / path.name).read_bytes() if dxvk else fl18.read(lib))
            replace[lib] = (path, zipfile.ZIP_DEFLATED)
    dex = OUT / 'dex/classes.dex'
    if dex.exists():
        replace['classes.dex'] = (dex, None)
    probe = OUT / 'launcher/probe/libportprobe.so'
    if probe.is_file():
        replace['lib/arm64-v8a/libportprobe.so'] = (probe, zipfile.ZIP_DEFLATED)
    assets = OUT / 'launcher-assets'
    resources = OUT / 'launcher/resources.apk'
    if assets.exists():
        for p in assets.rglob('*'):
            if p.is_file() and p.relative_to(assets).parts[0] in ('launcher', 'port-ui'):
                replace['assets/' + p.relative_to(assets).as_posix()] = (p, zipfile.ZIP_DEFLATED)
    unsigned = folder / 'unsigned.apk'
    changed = []
    with zipfile.ZipFile(BASE_APK) as src, zipfile.ZipFile(unsigned, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as dst:
        res = zipfile.ZipFile(resources) if resources.exists() else None
        names = set()
        for info in src.infolist():
            if info.filename.startswith(('META-INF/', 'assets/turnip/')) or info.filename in (
                    'lib/arm64-v8a/libfrostpunk_turnip.so',
                    'assets/licenses/Turnip-SM8250-origin.json', 'assets/licenses/Turnip-SM8250-LICENSE-NOTE.txt'):
                continue
            names.add(info.filename)
            if res is not None and (info.filename in ('AndroidManifest.xml', 'resources.arsc') or info.filename.startswith('res/')) \
                    and info.filename in res.namelist():
                data = res.read(info.filename)
                if data != src.read(info.filename):
                    changed.append(info.filename)
                dst.writestr(info, data)
            elif info.filename in replace:
                path, compress = replace.pop(info.filename)
                if path.read_bytes() != src.read(info.filename):
                    changed.append(info.filename)
                dst.write(path, info.filename, compress_type=info.compress_type if compress is None else compress)
            elif info.filename == 'assets/licenses/runtime-provenance.json':
                row = json.loads(src.read(info.filename))
                row['binary_sha256'], row['binary_bytes'] = sha(runtime), runtime.stat().st_size
                row['performance_patch'] = 'fastlift units + canonical host (ui-20261114 %s)' % name
                dst.writestr(info, (json.dumps(row, indent=1) + '\n').encode())
                changed.append(info.filename)
            else:
                compression = zipfile.ZIP_DEFLATED if info.filename.startswith('lib/') else info.compress_type
                dst.writestr(info, src.read(info.filename), compress_type=compression)
        for entry, (path, compress) in sorted(replace.items()):
            dst.write(path, entry, compress_type=zipfile.ZIP_DEFLATED if compress is None else compress)
            changed.append(entry)
        if res is not None:
            for entry in res.namelist():
                if (entry == 'resources.arsc' or entry.startswith('res/')) and entry not in names:
                    dst.writestr(entry, res.read(entry))
                    changed.append(entry)
    aligned = folder / 'aligned.apk'
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
    apk = folder / ('Frostpunk-ui-%s.apk' % name)
    subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / fb.MACHINE['signer']['apksigner']), 'sign',
                    '--key', str(fb.SIGNER / fb.MACHINE['signer']['key']), '--cert', str(fb.SIGNER / fb.MACHINE['signer']['cert']),
                    '--out', str(apk), str(aligned)], check=True)
    subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / fb.MACHINE['signer']['apksigner']), 'verify', str(apk)], check=True)
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(apk)], check=True)
    unsigned.unlink()
    aligned.unlink()
    manifest = dict(apk=str(apk.relative_to(ROOT)), apk_sha256=sha(apk), runtime_sha256=sha(runtime), changed=sorted(changed),
                    base_apk_sha256=sha(BASE_APK), packaging=packaging)
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(json.dumps({k: manifest[k] for k in ('apk', 'apk_sha256', 'runtime_sha256', 'changed')}, indent=1), flush=True)
    return apk
