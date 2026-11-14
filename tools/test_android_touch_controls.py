"""Run extracted production gestures on the JVM and JNI validation on the selected device."""
import hashlib
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/android-touch-controls/checks'


def extract(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def run(command):
    result = subprocess.run(list(map(str, command)), check=True, capture_output=True, text=True, timeout=60)
    return result.stdout.strip()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial',required=True)
    args=parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    sources = ['android/app/TouchControls.java', 'android/app/app.c',
               'tests/TouchControlsCheck.java', 'tests/android_touch_jni_fixture.c', __file__, 'android/port-help-localization.json']
    pins = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources}
    java_text = (ROOT / sources[0]).read_text(encoding='utf-8')
    methods = ''.join(extract(java_text, signature) for signature in (
        'void presented()', 'private void updateLoading()', 'private void showSpeedHint()', 'private void camera(', 'private void zoomWheel(', 'private void releasePointer()', 'private void click(', 'void pulse(', 'void cancel()',
        'private void timeStart(', 'private void timeMove(', 'private void timeEnd()', 'private final class Gesture'))
    methods = methods.replace('android.os.SystemClock.uptimeMillis()', 'now')
    methods = methods.replace('android.widget.Toast', 'Toast')
    help_text=json.loads((ROOT / 'android/port-help-localization.json').read_text(encoding='utf-8'))
    assert all(len(row[0].split('\n\n'))==7 and row[0].split('\n\n')[4] for row in help_text.values())
    # The first cancel is the outer lifecycle method; the inner one is included with Gesture.
    fixture = (ROOT / sources[2]).read_text(encoding='utf-8').replace('/* PRODUCTION_METHODS */', methods)
    (OUT / 'TouchControls.java').write_text(fixture, encoding='utf-8')
    cfg = json.loads((ROOT / 'port.json').read_text())
    import shutil
    java = Path(shutil.which('javac')).parent
    run([java / 'javac.exe', '-encoding', 'UTF-8', '-d', OUT, OUT / 'TouchControls.java'])
    host = json.loads(run([java / 'java.exe', '-ea', '-cp', OUT, 'TouchControls']))
    native = (ROOT / sources[1]).read_text(encoding='utf-8')
    start = native.index('static jboolean touch_submit(')
    end = native.index('\n#endif', start)
    (OUT / 'touch_jni_production.inc').write_text(native[start:end] + '\n', encoding='utf-8')
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary = OUT / 'touch-jni-arm64'
    run([clang, '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wall', '-Wextra',
         '-I' + str(ROOT / 'android/app'), '-I' + str(OUT), ROOT / sources[3], '-pthread', '-lm', '-Wl,-z,max-page-size=16384', '-o', binary])
    adb = [Path(cfg['android_sdk']) / 'platform-tools/adb.exe', '-s', args.serial]
    remote = '/data/local/tmp/frostpunk-touch-jni-check'
    run([*adb, 'push', binary, remote])
    try:
        run([*adb, 'shell', 'chmod', '700', remote])
        arm = json.loads(run([*adb, 'shell', remote]))
    finally:
        run([*adb, 'shell', 'rm', '-f', remote])
    assert host['success'] and arm['success']
    assert pins == {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources}
    report = dict(success=True, serial=args.serial, host=host, arm=arm, sources=pins)
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
