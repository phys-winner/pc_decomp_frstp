"""Exercise driver cancellation, offline cache reuse and cleanup on a selected Android device."""
import argparse
from pathlib import Path
import subprocess
import uuid
import fastbuild as fb

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial', required=True)
parser.add_argument('--live', action='store_true', help='Also download and verify the pinned public T30 release')
args = parser.parse_args()
out = fb.ROOT / 'build/release-0.2-dev/driver-tests'
out.mkdir(parents=True, exist_ok=True)
dex = out / 'dex'
dex.mkdir(exist_ok=True)
platform = fb.SDK / 'platforms/android-37.0/android.jar'
classes = fb.ROOT / 'build/release-0.2-dev/java/classes'
source = fb.ROOT / 'tests/FrostpunkDriverDownloadTest.java'
subprocess.run([str(fb.JAVA.parent / 'javac.exe'), '--release', '8', '-encoding', 'UTF-8',
                '-cp', str(platform) + ';' + str(classes), '-d', str(out / 'test-classes'), str(source)], check=True)
subprocess.run([str(fb.JAVA), '-cp', str(fb.BUILD_TOOLS / 'lib/d8.jar'), 'com.android.tools.r8.D8',
                '--min-api', '30', '--lib', str(platform), '--output', str(dex),
                *map(str, (out / 'test-classes').rglob('*.class'))], check=True)
adb = [str(fb.SDK / 'platform-tools/adb.exe'), '-s', args.serial]
remote = '/data/local/tmp/fp-driver-test-' + uuid.uuid4().hex[:8]
subprocess.run(adb + ['shell', 'mkdir', '-p', remote], check=True)
for name, path in [('app.dex', fb.ROOT / 'build/release-0.2-dev/dex/classes.dex'), ('tests.dex', dex / 'classes.dex')]:
    subprocess.run(adb + ['push', str(path), remote + '/' + name], check=True, capture_output=True)
url = 'https://github.com/MrPurple666/purple-turnip/releases/download/vturnip_mrpurple_T30-toasted.adpkg/turnip_mrpurple_T30-toasted.adpkg.zip'
result = subprocess.run(adb + ['shell', 'env', 'CLASSPATH=' + remote + '/tests.dex:' + remote + '/app.dex',
    'app_process', '/system/bin', 'com.frostpunk.staticrecomp.launcher.FrostpunkDriverDownloadTest',
    remote + '/data', *([url] if args.live else [])], capture_output=True, text=True, encoding='utf-8', timeout=180 if args.live else 30)
(out / 'stdout.txt').write_text(result.stdout, encoding='utf-8')
(out / 'stderr.txt').write_text(result.stderr, encoding='utf-8')
print(result.stdout)
if result.stderr: print(result.stderr)
print('Isolated test files:', remote)
raise SystemExit(result.returncode)
