"""Cold launch to the title screen N times; report time-to-title or a hang (no title within LIMIT s).

usage: python -X utf8 tools/title_probe.py LABEL COUNT [LIMIT]
On a hang the native log slice and `debuggerd -b` stacks are saved under build/perf/LABEL-hangN/.
"""
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_ha22 as h  # noqa: E402

label, count = sys.argv[1], int(sys.argv[2])
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
apk = h.adb(['shell', 'sha256sum', h.adb(['shell', 'pm', 'path', h.PKG]).removeprefix('package:')]).split()[0]
print('apk', apk[:16], flush=True)
times, hangs = [], 0
for i in range(1, count + 1):
    h.adb(['shell', 'am', 'force-stop', h.PKG])
    time.sleep(1.0)
    offset = len(h.native_log())
    h.adb(['shell', 'am', 'start', '-n', h.PKG + '/.MainActivity'])
    t0 = time.monotonic()
    seen = None
    while time.monotonic() - t0 < limit:
        label_now, _ = h.classify(h.adb(['exec-out', 'screencap', '-p'], binary=True))
        if label_now == 'title':
            seen = time.monotonic() - t0
            break
        time.sleep(0.4)
    if seen is None:
        hangs += 1
        folder = h.ROOT / 'build/perf' / f'{label}-hang{hangs}'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'native.log').write_bytes(h.native_log(offset))
        pid = h.adb(['shell', 'pidof', h.PKG], check=False).strip()
        if pid:
            (folder / 'hang-stacks.txt').write_text(h.adb(['shell', 'su', '-c', 'debuggerd -b ' + pid], check=False, timeout=120), encoding='utf-8')
        print(f'{label}-{i} HANG', flush=True)
    else:
        times.append(seen)
        print(f'{label}-{i} title {seen:.1f}', flush=True)
h.adb(['shell', 'am', 'force-stop', h.PKG])
mean = sum(times) / len(times) if times else 0
print(f'TITLE_LOOP DONE runs={count} hangs={hangs} mean_title={mean:.1f}', flush=True)
