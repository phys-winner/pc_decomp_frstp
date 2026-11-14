"""Run the real texture codec, archive transactions and capability policy on one Android device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid
import zipfile
import fastbuild as build
import android_launcher


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', type=Path)
    parser.add_argument('--classes', type=Path)
    args = parser.parse_args()
    out = build.ROOT / 'build/launcher-tests' / uuid.uuid4().hex[:12]
    classes, dex = out / 'classes', out / 'dex'
    classes.mkdir(parents=True); dex.mkdir()
    source = build.ROOT / 'tests/FrostpunkLauncherTest.java'
    platform = build.SDK / 'platforms/android-37.0/android.jar'
    subprocess.run([str(build.JAVA.parent / 'javac.exe'), '--release', '8', '-encoding', 'UTF-8',
                    '-cp', str(platform) + os.pathsep + str(args.classes or build.FAST / 'java/classes'), '-d', str(classes), str(source)], check=True)
    base_dex = build.FAST / 'dex/classes.dex'
    if args.apk:
        base_dex = out / 'current.dex'
        with zipfile.ZipFile(args.apk) as apk:
            base_dex.write_bytes(apk.read('classes.dex'))
    subprocess.run([str(build.JAVA), '-cp', str(build.BUILD_TOOLS / 'lib/d8.jar'), 'com.android.tools.r8.D8',
                    '--min-api', '30', '--lib', str(platform), '--output', str(dex),
                    str(base_dex), *map(str, classes.rglob('*.class'))], check=True)
    library = android_launcher.build_textures()
    if args.apk:
        stripped = out / library.name
        subprocess.run([str(build.BIN / 'llvm-strip.exe'), '--strip-unneeded', '-o', str(stripped), str(library)], check=True)
        with zipfile.ZipFile(args.apk) as apk:
            assert apk.read('lib/arm64-v8a/' + library.name) == stripped.read_bytes(), 'APK contains a different texture codec'
        library = stripped
    cmake = build.SDK / 'cmake/3.22.1/bin/cmake.exe'
    codec_build = build.ROOT / 'build/texture-converter'
    subprocess.run([str(cmake), '-S', str(build.ROOT / 'android/texconv'), '-B', str(codec_build), '-DFP_TEXTURE_TEST=ON'], check=True, capture_output=True)
    subprocess.run([str(cmake), '--build', str(codec_build), '--target', 'texture_conversion_test', '--parallel', '6'], check=True, capture_output=True)
    files = {'classes.dex': dex / 'classes.dex', library.name: library, 'texture_conversion_test': codec_build / 'texture_conversion_test'}
    adb = [str(build.SDK / 'platform-tools/adb.exe'), '-s', args.serial]
    remote = '/data/local/tmp/frostpunk-pixel10-startup-fix/test-' + out.name
    subprocess.run(adb + ['shell', 'mkdir', '-p', remote], check=True)
    for name, path in files.items():
        subprocess.run(adb + ['push', str(path), remote + '/' + name], check=True, capture_output=True)
    subprocess.run(adb + ['shell', 'chmod', '700', remote + '/texture_conversion_test'], check=True)
    native = subprocess.run(adb + ['shell', remote + '/texture_conversion_test'], check=True, capture_output=True, text=True, timeout=30)
    result = subprocess.run(adb + ['shell', 'env', 'CLASSPATH=' + remote + '/classes.dex', 'app_process',
                                  '-Djava.library.path=' + remote, '/system/bin', 'com.frostpunk.staticrecomp.launcher.FrostpunkLauncherTest',
                                  remote + '/texture-test-fixture'], capture_output=True, text=True, timeout=90)
    (out / 'stdout.txt').write_text(native.stdout + result.stdout, encoding='utf-8')
    (out / 'stderr.txt').write_text(native.stderr + result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    import texture2ddecoder
    decoded = 0
    for fmt in ('31545844', '33545844', '35545844'):
        for kind in ('raw', 'gzip'):
            data = subprocess.check_output(adb + ['exec-out', 'cat', remote + '/texture-test-fixture/' + fmt + '-' + kind + '/textures-s3.dat'])
            pixels = texture2ddecoder.decode_astc(data[144:208], 8, 8, 6, 6)
            assert len(pixels) == 8 * 8 * 4
            assert max(abs(value - (0, 0, 255, 255)[i % 4]) for i, value in enumerate(pixels)) <= 1
            decoded += 1
    receipt = dict(success=True, serial=args.serial, native=native.stdout.strip(), java=result.stdout.strip(),
                   independent_astc_pixel_checks=decoded, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                   files_sha256={name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in files.items()})
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(dict(receipt, proof=str(out)), indent=2))


if __name__ == '__main__':
    main()
