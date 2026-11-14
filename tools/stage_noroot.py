"""Push the verified game files (ui-astc6 + render-scale + port-settings + Switch UI) without root.

Use the matching archive pairs from research/game-host-data-port-settings.json.
Stop the game before staging; changes to UI recipes take effect after restarting it.

usage: python tools/stage_noroot.py SERIAL   (install the APK and start it once first)
Shell-created entries in Android/data are owned by uid 2000; the directory is made
2777 and the files 0666 so the app (not in ext_data_rw) can open them.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / 'research/game-host-data-port-settings.json'
PKG = 'com.frostpunk.staticrecomp'
DEST = '/sdcard/Android/data/%s/files/game' % PKG
serial = sys.argv[1]
ADB = ['adb', '-s', serial]


def sh(cmd, timeout=600):
    p = subprocess.run(ADB + ['shell', cmd], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout.strip(), p.stderr.strip()


files = json.loads(CONTRACT.read_text())['files']
rc, out, err = sh('mkdir -p %s && chmod 2777 %s && ls -lan %s/..' % (DEST, DEST, DEST))
print(rc, out, err, flush=True)
existing = {}
rc, out, _ = sh('cd %s && for f in *; do [ -f "$f" ] && echo "$(stat -c %%s "$f") $f"; done' % DEST)
for line in out.splitlines():
    size, name = line.split(' ', 1)
    existing[name] = int(size)
t0 = time.time()
total = 0
for name, item in sorted(files.items(), key=lambda kv: kv[1]['bytes']):
    src = Path(item['path'])
    src = src if src.is_absolute() else ROOT / src
    if existing.get(name) == item['bytes']:
        # Indexes keep their size when records are repointed: compare small files by content.
        if item['bytes'] >= 64 << 20 or sh('sha256sum %s/%s' % (DEST, name))[1].split()[:1] == [item['sha256']]:
            print('skip (same size)', name, flush=True)
            continue
    t = time.time()
    p = subprocess.run(ADB + ['push', str(src), DEST + '/' + name], capture_output=True, text=True, timeout=7200)
    if p.returncode:
        raise SystemExit('push failed %s: %s' % (name, p.stderr))
    total += item['bytes']
    dt = time.time() - t
    print('pushed %-28s %8.1f MB %6.1f s %6.1f MB/s' % (name, item['bytes'] / 1e6, dt, item['bytes'] / 1e6 / max(dt, 1e-3)), flush=True)
print('push total %.2f GB in %.0f s' % (total / 1e9, time.time() - t0), flush=True)
rc, out, err = sh('chmod 2777 %s && chmod 0666 %s/*' % (DEST, DEST))
print('chmod', rc, err, flush=True)
rc, out, err = sh('cd %s && sha256sum *' % DEST, timeout=3600)
device = {}
for line in out.splitlines():
    digest, name = line.split(None, 1)
    device[name.strip()] = digest
bad = [n for n, i in files.items() if device.get(n) != i['sha256']]
extra = sorted(set(device) - set(files))
print(json.dumps(dict(files=len(files), verified=len(files) - len(bad), mismatched=bad, extra=extra,
                      seconds=round(time.time() - t0)), indent=1), flush=True)
