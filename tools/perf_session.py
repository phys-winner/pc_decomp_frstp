"""Frostpunk device session: launch -> title -> menu -> city, then measure FPS, CPU, memory and hangs.

    python -X utf8 tools/perf_session.py --serial SERIAL --label NAME [options]

Stages are found from the runtime's own log markers (no screen references):
  title  = first PRESENT_SAMPLE with frames after the second swap chain (the title waits for a key),
  menu   = a few seconds after ENTER on the title,
  city   = FP_TOUCH_CAMERA (the city camera controller exists).
FPS comes from PRESENT_SAMPLE (accepted vkQueuePresent per CLOCK_MONOTONIC second); CPU per thread
from /proc/PID/task/*/stat deltas; memory from `dumpsys meminfo` (PSS/RSS, native heap, GL/EGL
mtrack, swap); GPU clock/busy only with root (Mali GED / KGSL sysfs).

If presents stop for --hang-seconds while the city is shown, all thread stacks are saved
(debuggerd -b with root, otherwise tools/fp_stackdump.c through run-as), and the run ends.
Results: build/perf/<label>/session.json (+ stacks, screenshots, native.log slice, simpleperf data).
"""
import argparse
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG = 'com.frostpunk.staticrecomp'
CONFIG = json.loads((ROOT / 'port.json').read_text())
NDK_BIN = Path(CONFIG['android_sdk']) / 'ndk' / CONFIG['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
# Taps on the 1280x720 menu canvas, mapped to the screen (the canvas is fitted and centred).
RESUME_CANVAS = (640, 217)
SAMPLE = re.compile(r'^PRESENT_SAMPLE .*?start_ns=(\d+) end_ns=(\d+) duration_ns=(\d+) count=(\d+) max_interval_ns=(\d+)')
MEM_ROWS = ('Native Heap', 'Dalvik Heap', 'Gfx dev', 'EGL mtrack', 'GL mtrack', '.so mmap', 'Unknown', 'TOTAL')


class Device:
    def __init__(self, serial):
        self.serial = serial
        self.adb = ['adb', '-s', serial]
        self.root = self.sh('su -c id', check=False).find('uid=0') >= 0
        size = self.sh('wm size').splitlines()[0].split(':')[1].strip()
        self.width, self.height = sorted(map(int, size.split('x')), reverse=True)
        self.pid = None

    def run(self, args, binary=False, timeout=60, check=True, input=None):
        p = subprocess.run(self.adb + [str(a) for a in args], capture_output=True, timeout=timeout, input=input)
        if check and p.returncode:
            raise RuntimeError('adb %s failed: %s' % (args, p.stderr.decode(errors='replace')))
        return p.stdout if binary else p.stdout.decode(errors='replace')

    def sh(self, command, timeout=60, check=True, binary=False):
        return self.run(['shell', command], timeout=timeout, check=check, binary=binary)

    def su(self, command, timeout=60):
        return self.sh("su -c '%s'" % command.replace("'", "'\\''"), timeout=timeout, check=False)

    def app(self, *args, binary=False, timeout=120):
        return self.run(['exec-out', 'run-as', PKG, *args], binary=binary, timeout=timeout, check=False)

    def screen(self, x, y):
        """Canvas (1280x720) point -> screen point for a fitted, centred canvas."""
        scale = min(self.width / 1280, self.height / 720)
        return round(self.width / 2 + (x - 640) * scale), round(self.height / 2 + (y - 360) * scale)


class Log:
    """Follow the shared or private native log written by this launch."""

    def __init__(self, dev):
        self.dev = dev
        self.offsets = {path: self.size(path) for path in ('/sdcard/Frostpunk/native.log', 'files/native.log')}
        self.path = None
        self.data = b''
        self.lock = threading.Lock()

    def size(self, path):
        size = self.dev.app('stat', '-c', '%s', path).strip()
        return int(size) if size.isdecimal() else 0

    def poll(self):
        paths = (self.path,) if self.path else self.offsets
        for path in paths:
            offset = self.offsets[path]
            if not self.path:
                size = self.size(path)
                if not size or size == offset:
                    continue
                if size < offset:
                    offset = 0
            chunk = self.dev.app('tail', '-c', '+%d' % (offset + len(self.data) + 1), path, binary=True)
            if chunk:
                self.path = path
                self.offsets[path] = offset
                with self.lock:
                    self.data += chunk
                return chunk
        return b''

    def text(self):
        with self.lock:
            return self.data.decode(errors='replace')

    def presents(self):
        rows = []
        for line in self.text().splitlines():
            m = SAMPLE.match(line)
            if m:
                start, end, duration, count, worst = map(int, m.groups())
                rows.append(dict(start_ns=start, end_ns=end, fps=count * 1e9 / duration, max_ms=worst / 1e6))
        return rows


def summary(rows):
    if not rows:
        return None
    f = sorted(r['fps'] for r in rows)
    return dict(seconds=len(rows), mean_fps=round(sum(f) / len(f), 2), min_fps=round(f[0], 2),
                p10_fps=round(f[len(f) // 10], 2), median_fps=round(f[len(f) // 2], 2), max_fps=round(f[-1], 2),
                worst_interval_ms=round(max(r['max_ms'] for r in rows), 2))


def meminfo(dev):
    text = dev.sh('dumpsys meminfo %s' % PKG, timeout=60, check=False)
    out = {}
    for line in text.splitlines():
        s = line.strip()
        for name in MEM_ROWS:
            if s.startswith(name + ' ') or s == name:
                nums = [int(v) for v in re.findall(r'\b\d+\b', s[len(name):])]
                if nums:
                    # Pss Total, Private Dirty, Private Clean, SwapPss Dirty, Rss Total, [Heap Size, Alloc, Free]
                    out[name] = dict(pss=nums[0], swap=nums[3] if len(nums) > 3 else None, rss=nums[4] if len(nums) > 4 else None,
                                     heap_size=nums[5] if len(nums) > 5 else None, heap_alloc=nums[6] if len(nums) > 6 else None)
                break
    m = re.search(r'TOTAL PSS:\s+(\d+)\s+TOTAL RSS:\s+(\d+)\s+TOTAL SWAP PSS:\s+(\d+)', text)
    if m:
        out['summary'] = dict(pss=int(m[1]), rss=int(m[2]), swap=int(m[3]))
    g = re.search(r'Graphics:\s+(\d+)', text)
    if g:
        out['graphics'] = int(g[1])
    return out


def thread_ticks(dev, pid):
    out = dev.sh('for t in /proc/%s/task/*; do cat $t/stat; done 2>/dev/null' % pid, check=False)
    rows = {}
    for line in out.splitlines():
        m = re.match(r'(\d+) \((.*)\) (\S) (.*)', line)
        if m:
            f = m[4].split()
            rows[int(m[1])] = (m[2], int(f[10]) + int(f[11]))
    return rows, time.monotonic()


def cpu_delta(a, b):
    (ra, ta), (rb, tb) = a, b
    rows = sorted((((ticks - ra[tid][1]) / (tb - ta), tid, name) for tid, (name, ticks) in rb.items() if tid in ra), reverse=True)
    return dict(total_pct=round(sum(r[0] for r in rows), 1),
                threads=[dict(name=n, tid=t, pct=round(p, 1)) for p, t, n in rows if p >= 0.5])


class Sampler(threading.Thread):
    """Memory + CPU + GPU every `interval` seconds while the app runs."""

    def __init__(self, dev, pid, interval, folder):
        super().__init__(daemon=True)
        self.dev, self.pid, self.interval, self.folder = dev, pid, interval, folder
        self.rows, self.stop, self.phase = [], threading.Event(), 'launch'

    def temps(self):
        """Thermal zones in degrees C (readable as shell on the tested phones; su on the tablet)."""
        script = 'for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type) $(cat $z/temp)"; done 2>/dev/null'
        text = self.dev.su(script) if self.dev.root else self.dev.sh(script, check=False)
        zones = {}
        for line in text.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[1].lstrip('-').isdigit():
                zones[parts[0]] = int(parts[1]) / 1000
        pick = lambda prefixes: max((v for k, v in zones.items() if k.startswith(prefixes)), default=None)
        skin = zones.get('skin-therm-usr', zones.get('skin-msm-therm-usr', zones.get('main_board_ntc')))
        return dict(cpu=pick(('cpu-', 'soc_max', 'cpuss-')), gpu=pick(('gpuss-', 'gpu')), skin=skin,
                    battery=zones.get('battery'))

    def gpu(self):
        if not self.dev.root:
            return None
        text = self.dev.su('cat /sys/kernel/ged/hal/gpu_utilization /sys/kernel/ged/hal/current_freqency 2>/dev/null;'
                           ' cat /sys/class/kgsl/kgsl-3d0/gpubusy /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null')
        return text.split()

    def run(self):
        last = thread_ticks(self.dev, self.pid)
        t0 = time.monotonic()
        while not self.stop.wait(self.interval):
            try:
                now = thread_ticks(self.dev, self.pid)
                if not now[0]:
                    break
                row = dict(t=round(time.monotonic() - t0, 1), phase=self.phase, cpu=cpu_delta(last, now), mem=meminfo(self.dev), gpu=self.gpu(), temps=self.temps())
                last = now
                self.rows.append(row)
                s = row['mem'].get('summary', {})
                tz = row['temps'] or {}
                print('  t=%5.0fs %-6s pss=%5d MB rss=%5d MB swap=%5d MB gl=%5d MB native=%5d MB cpu=%5.1f%% temp=%s/%s' % (
                    row['t'], self.phase, s.get('pss', 0) // 1024, s.get('rss', 0) // 1024, s.get('swap', 0) // 1024,
                    (row['mem'].get('GL mtrack') or {}).get('pss', 0) // 1024,
                    ((row['mem'].get('Native Heap') or {}).get('heap_alloc') or 0) // 1024, row['cpu']['total_pct'],
                    tz.get('cpu'), tz.get('skin')), flush=True)
            except Exception as error:  # a slow dumpsys must not kill the session
                print('  sampler error:', error, flush=True)


def stackdump(dev, folder, name):
    pid = dev.pid
    if dev.root:
        text = dev.su('debuggerd -b %s' % pid, timeout=180)
    else:
        tool = ROOT / 'build/perf/tools/fp-stackdump'
        if not tool.exists():
            tool.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(NDK_BIN / 'clang.exe'), '--target=aarch64-linux-android30', '-O2', '-Wl,-z,max-page-size=16384',
                            str(ROOT / 'tools/fp_stackdump.c'), '-o', str(tool)], check=True)
        dev.run(['push', str(tool), '/data/local/tmp/fp-stackdump'])
        dev.sh('chmod 755 /data/local/tmp/fp-stackdump; run-as %s cp /data/local/tmp/fp-stackdump ./fp-stackdump; run-as %s chmod 700 ./fp-stackdump' % (PKG, PKG))
        text = dev.sh('run-as %s ./fp-stackdump %s 48' % (PKG, pid), timeout=180, check=False)
    path = folder / ('stacks-%s.txt' % name)
    path.write_text(text, encoding='utf-8')
    return str(path)


def key(dev, name, ms=None):
    if ms:
        dev.sh('input keyboard keyevent --duration %d KEYCODE_%s' % (ms, name), check=False)
    else:
        dev.sh('input keyevent KEYCODE_%s' % name, check=False)


def swipe(dev, x0, y0, x1, y1, ms):
    dev.sh('input touchscreen swipe %d %d %d %d %d' % (x0, y0, x1, y1, ms), check=False)


def route(dev):
    """Camera route with one-finger drags (direct camera pan) and keyboard rotate/zoom keys."""
    w, h = dev.width, dev.height
    cx, cy = w // 2, h // 2
    steps = []
    for name, action in (
            ('pan_left', lambda: swipe(dev, cx - w // 5, cy, cx + w // 5, cy, 1500)),
            ('pan_right', lambda: swipe(dev, cx + w // 5, cy, cx - w // 5, cy, 1500)),
            ('pan_up', lambda: swipe(dev, cx, cy - h // 5, cx, cy + h // 5, 1500)),
            ('pan_down', lambda: swipe(dev, cx, cy + h // 5, cx, cy - h // 5, 1500)),
            ('rotate_Q', lambda: key(dev, 'Q', 3000)), ('rotate_E', lambda: key(dev, 'E', 3000)),
            ('move_W', lambda: key(dev, 'W', 3000)), ('move_S', lambda: key(dev, 'S', 3000)),
            ('pan_diag', lambda: swipe(dev, cx - w // 5, cy - h // 6, cx + w // 5, cy + h // 6, 2000)),
            ('pan_back', lambda: swipe(dev, cx + w // 5, cy + h // 6, cx - w // 5, cy - h // 6, 2000))):
        t = time.monotonic()
        action()
        steps.append(dict(name=name, t0=t, t1=time.monotonic()))
        time.sleep(0.3)
    return steps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--label', required=True)
    ap.add_argument('--city-seconds', type=int, default=30, help='paused-city measurement window')
    ap.add_argument('--soak', type=int, default=0, help='extra seconds in the running city (memory growth)')
    ap.add_argument('--route', type=int, default=1, help='number of camera route repetitions (0 = none)')
    ap.add_argument('--unpaused', action='store_true', help='keep the simulation running during the windows')
    ap.add_argument('--interval', type=float, default=5.0, help='memory/CPU sample interval')
    ap.add_argument('--simpleperf', type=int, default=0, help='seconds of simpleperf (call graph) in the paused city')
    ap.add_argument('--hang-seconds', type=float, default=12.0)
    ap.add_argument('--timeout', type=int, default=240, help='launch-to-city limit')
    ap.add_argument('--keep-running', action='store_true')
    ap.add_argument('--gpu-trace', type=float, default=0,
                    help='seconds of perfetto gpu.renderstages after the paused window (sets debug.graphics.gpu.profiler.perfetto)')
    args = ap.parse_args()
    dev = Device(args.serial)
    folder = ROOT / 'build/perf' / args.label
    folder.mkdir(parents=True, exist_ok=True)
    report = dict(label=args.label, serial=args.serial, root=dev.root, screen=[dev.width, dev.height], stages={}, windows={})
    apk = dev.sh('pm path %s' % PKG).splitlines()[0].removeprefix('package:').strip()
    report['apk_sha256'] = dev.sh('sha256sum %s' % apk).split()[0]
    report['port_settings'] = dev.app('cat', 'files/port-settings.txt')
    report['battery'] = {k: v for k, v in re.findall(r'(level|temperature): (\d+)', dev.sh('dumpsys battery'))}

    def save():
        (folder / 'session.json').write_text(json.dumps(report, indent=1) + '\n')

    dev.sh('am force-stop %s' % PKG)
    if args.gpu_trace:
        dev.sh('setprop debug.graphics.gpu.profiler.perfetto 1', check=False)
    time.sleep(1)
    log = Log(dev)
    dev.sh('input keyevent KEYCODE_WAKEUP', check=False)
    t0 = time.monotonic()
    dev.sh('am start -n %s/.MainActivity' % PKG)
    for _ in range(50):
        pid = dev.sh('pidof %s' % PKG, check=False).strip()
        if pid:
            break
        time.sleep(0.2)
    dev.pid = pid.split()[0]
    sampler = Sampler(dev, dev.pid, args.interval, folder)
    sampler.start()
    stage, presenters, menu_at, city_at = 'start', 0, None, None
    last_present = time.monotonic()
    try:
        while True:
            now = time.monotonic() - t0
            if now > args.timeout:
                report['error'] = 'timeout in stage ' + stage
                report['stacks'] = stackdump(dev, folder, 'timeout')
                break
            log.poll()
            text = log.text()
            presenters = text.count('info:  Presenter: Actual swap chain properties') + text.count('RDVK_SWAPCHAIN ')
            rows = log.presents()
            if stage == 'start' and presenters >= 2 and rows and rows[-1]['fps'] > 5:
                stage = 'title'
                report['stages']['title'] = round(now, 2)
                print('title %.1fs' % now, flush=True)
                time.sleep(1.0)
                key(dev, 'ENTER')
                menu_at = time.monotonic()
            elif stage == 'title' and time.monotonic() - menu_at > 4.0:
                stage = 'menu'
                report['stages']['menu'] = round(now, 2)
                (folder / 'menu.png').write_bytes(dev.run(['exec-out', 'screencap', '-p'], binary=True))
                x, y = dev.screen(*RESUME_CANVAS)
                print('menu  %.1fs -> tap resume %d,%d' % (now, x, y), flush=True)
                dev.sh('input tap %d %d' % (x, y))
                report['resume_tap'] = round(time.monotonic() - t0, 2)
                sampler.phase = 'load'
            elif stage == 'menu' and 'FP_TOUCH_CAMERA' in text:
                stage = 'city'
                city_at = time.monotonic()
                report['stages']['city'] = round(now, 2)
                report['menu_to_city_s'] = round(now - report['resume_tap'], 2)
                print('city  %.1fs (load %.1fs)' % (now, report['menu_to_city_s']), flush=True)
                break
            time.sleep(0.5)
        if stage == 'city':
            time.sleep(4.0)
            (folder / 'city.png').write_bytes(dev.run(['exec-out', 'screencap', '-p'], binary=True))
            if not args.unpaused:
                key(dev, 'SPACE')
            time.sleep(1.5)

            def window(name, seconds, action=None):
                sampler.phase = name
                log.poll()
                lo = len(log.presents())
                start = time.monotonic()
                if action:
                    action()
                while time.monotonic() - start < seconds:
                    time.sleep(1.0)
                    log.poll()
                    if check_hang(name):
                        return False
                log.poll()
                rows = log.presents()[lo:]
                report['windows'][name] = summary(rows)
                print('%-8s %s' % (name, json.dumps(report['windows'][name])), flush=True)
                save()
                return True

            hang_state = dict(count=None, since=time.monotonic())

            def check_hang(name):
                """Presents emit one PRESENT_SAMPLE per second; none for hang_seconds means the frame loop stopped."""
                count = len(log.presents())
                if count != hang_state['count']:
                    hang_state['count'], hang_state['since'] = count, time.monotonic()
                    return False
                if time.monotonic() - hang_state['since'] < args.hang_seconds:
                    return False
                report['hang'] = dict(window=name, at=round(time.monotonic() - t0, 1), presents=count)
                print('HANG detected in %s' % name, flush=True)
                report['stacks'] = stackdump(dev, folder, 'hang')
                (folder / 'hang.png').write_bytes(dev.run(['exec-out', 'screencap', '-p'], binary=True))
                save()
                return True

            ok = window('paused' if not args.unpaused else 'idle', args.city_seconds)
            if ok and args.gpu_trace:
                cfg = ('buffers { size_kb: 131072 fill_policy: DISCARD }' + chr(10) + 'duration_ms: %d' % int(args.gpu_trace * 1000) + chr(10) +
                       'data_sources { config { name: "gpu.renderstages" } }' + chr(10))
                remote = '/data/misc/perfetto-traces/fp-session-stages.pftrace'
                runner = (['shell', 'su', '-c', "'perfetto --txt -c - -o %s; chmod 644 %s'" % (remote, remote)] if dev.root
                          else ['shell', 'perfetto --txt -c - -o %s' % remote])
                dev.run(runner, input=cfg.encode(), timeout=120, check=False)
                dev.run(['pull', remote, str(folder / 'gpu-stages.pftrace')], timeout=120, check=False)
                print('gpu trace saved', flush=True)
            if ok and args.simpleperf:
                remote = '/data/local/tmp/fp-session.data'
                perf = subprocess.Popen(dev.adb + ['shell', 'simpleperf record --app %s -e cpu-clock -f 2000 --call-graph fp --duration %d -o %s'
                                                   % (PKG, args.simpleperf, remote)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                ok = window('perf', args.simpleperf + 2)
                perf.wait(timeout=120)
                dev.run(['pull', remote, str(folder / 'perf.data')], timeout=300, check=False)
                dev.sh('rm -f %s' % remote, check=False)
            for i in range(args.route if ok else 0):
                steps = []
                ok = window('route%d' % i, 0, lambda: steps.extend(route(dev)))
                if not ok:
                    break
            if ok and args.soak:
                if not args.unpaused:
                    key(dev, 'SPACE')  # resume the simulation for the soak
                window('soak', args.soak)
    finally:
        sampler.stop.set()
        sampler.join(timeout=60)
        report['samples'] = sampler.rows
        log.poll()
        (folder / 'native.log').write_bytes(log.data)
        if not args.keep_running and 'hang' not in report:
            dev.sh('am force-stop %s' % PKG, check=False)
        save()
        brief = {k: v for k, v in report.items() if k not in ('samples', 'port_settings')}
        print(json.dumps(brief, indent=1))


if __name__ == '__main__':
    main()
