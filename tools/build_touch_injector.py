"""Build the shell-UID Android multi-touch injector. No APK install or device action."""
import json
from pathlib import Path
import subprocess
import zipfile
from build_android_game import ROOT, portable_apk_tools

def main():
    out = ROOT / 'build/android-touch-controls/injector'
    out.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    platform = Path(cfg['android_sdk']) / 'platforms/android-36/android.jar'
    portable = portable_apk_tools()
    java = portable['java_exe'].parent
    def run(args):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60)
        if result.returncode: raise RuntimeError(result.stdout + result.stderr)
    classes, dex = out / 'classes', out / 'dex'
    classes.mkdir(exist_ok=True); dex.mkdir(exist_ok=True)
    run([java / 'javac.exe', '-encoding', 'UTF-8', '--release', '8', '-classpath', platform,
         '-d', classes, ROOT / 'tests/TouchInjector.java'])
    run([java / 'java.exe', '-cp', portable['build_tools'] / 'lib/d8.jar', 'com.android.tools.r8.D8',
         '--min-api', '30', '--lib', platform, '--output', dex, *sorted(classes.rglob('*.class'))])
    jar = out / 'touch-injector.jar'
    with zipfile.ZipFile(jar, 'w') as z: z.write(dex / 'classes.dex', 'classes.dex')
    print(jar)

if __name__ == '__main__': main()
