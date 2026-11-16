"""Large-city A/B measurement on any attached device (one process per device, run them in parallel).

    python -X utf8 tools/perf_city.py --serial SERIAL --label NAME [--apk FILE] [options]

A dedicated QA profile (/sdcard/Frostpunk/<profile>) is created from an existing QA profile on the
device and re-seeded with the same developed PC city before every launch; the player's own profile
(/sdcard/Frostpunk/userdata) is never started or written.

Windows: `normal` (simulation at speed 1) and `paused`. FPS = accepted vkQueuePresent per second
(PRESENT_SAMPLE in files/native.log), CPU per thread from /proc, CPU clocks from cpufreq sysfs.
Results: build/perf/fastlift/<label>/result.json (+ native.log, screenshots, perf.data).
"""
import argparse
import io
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from perf_session import Device, Log, Sampler, summary, key, thread_ticks, cpu_delta, route  # noqa: E402

PKG = 'com.frostpunk.staticrecomp'
SEED = ROOT / 'build/perf/minimum60-20261113'
SAVE_SHA = '2fc45017ecd583cece1b891f20cb7f92c7a70a0bcf295c8e44eac142a608bb95'
OUT = ROOT / 'build/perf/fastlift'
# An existing QA profile per device that already carries a complete offline-services tree.
SEED_PROFILES = ('userdata-qa-comparison-large-20261112', 'userdata-qa-minimum60-20261113',
                 'userdata-qa-phone20261112')
PROPS = ('debug.fp.args', 'debug.fp.quality', 'debug.fp.extra_fx', 'debug.fp.detail', 'debug.frostpunk.ui_dump')


class RetryDevice(Device):
    """adb occasionally drops a command while several devices are driven at once: retry it."""

    def run(self, args, binary=False, timeout=60, check=True, input=None):
        for attempt in range(4):
            try:
                return super().run(args, binary=binary, timeout=timeout, check=check, input=input)
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                transient = any(word in str(error) for word in ('daemon not running', 'daemon still not running', 'device offline', 'closed',
                                                                 'timed out', 'protocol fault', 'cannot connect to daemon', 'failed to start daemon'))
                if attempt == 3 or not transient or (args and args[0] == 'install'):
                    raise
                time.sleep(2 + attempt * 2)


def shot(dev, path=None):
    png = dev.run(['exec-out', 'screencap', '-p'], binary=True, timeout=60)
    if path:
        Path(path).write_bytes(png)
    return png


def image(png):
    from PIL import Image
    im = Image.open(io.BytesIO(png)).convert('RGB')
    if im.size[0] < im.size[1]:
        im = im.rotate(90, expand=True)
    return im


def pixels(im):
    return (im.get_flattened_data if hasattr(im, 'get_flattened_data') else im.getdata)()


def same_screen(a, b):
    """Two screenshots show the same screen (animated backgrounds allowed)."""
    small = [image(png).convert('L').resize((64, 36)) for png in (a, b)]
    return sum(abs(x - y) for x, y in zip(pixels(small[0]), pixels(small[1]))) / (64 * 36) < 10


def hud_ready(png):
    """The resource bar (amber icons on the top edge) is drawn."""
    im = image(png)
    w, h = im.size
    top = im.crop((w // 5, 0, w * 4 // 5, h // 10)).resize((360, 60))
    return sum(r > 120 and g > 95 and b < r * .55 and g > r * .55 for r, g, b in pixels(top)) > 60


def acknowledgement_visible(dev, png):
    """The white 'acknowledge' button of a story/scout popup under the screen centre."""
    im = image(png)
    x0, y0 = dev.screen(540, 506)
    x1, y1 = dev.screen(740, 530)
    data = list(pixels(im.crop((x0, y0, x1, y1))))
    return sum(min(p) > 200 and max(p) - min(p) < 35 for p in data) > len(data) * .5


def cpufreq(dev):
    script = ('for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "$(basename $p) $(cat $p/scaling_cur_freq) '
              '$(cat $p/scaling_max_freq) $(cat $p/cpuinfo_max_freq)"; done 2>/dev/null')
    text = dev.sh(script, check=False)
    if not text.strip() and dev.root:
        text = dev.su(script)
    rows = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) == 4 and all(v.isdigit() for v in f[1:]):
            rows[f[0]] = dict(cur=int(f[1]) // 1000, cap=int(f[2]) // 1000, max=int(f[3]) // 1000)
    return rows


def gpu_state(dev):
    """GPU clock in MHz and, where the kernel exposes it, load in percent: MediaTek GED through
    root on the tablet, the world-readable Mali devfreq node on the Pixel."""
    if dev.root:
        f = dev.su('cat /sys/kernel/ged/hal/gpu_utilization /sys/kernel/ged/hal/current_freqency 2>/dev/null').split()
        if len(f) >= 5 and all(v.isdigit() for v in f[:5]):
            state = dict(load=int(f[0]), mhz=int(f[4]) // 1000)
            # MediaTek thermal core: the real CPU ceiling per cluster (little, middle, big), which
            # cpufreq's scaling_max_freq does not show, and whether it is limiting at all.
            limit = dev.su('cat /sys/kernel/thermal/is_cpu_limit /sys/kernel/thermal/cpu_info 2>/dev/null').replace(',', ' ').split()
            if len(limit) >= 7 and all(v.lstrip('-').isdigit() for v in limit[:7]):
                state.update(cpu_limited=int(limit[0]), big_ceiling_mhz=int(limit[6]) // 1000, middle_ceiling_mhz=int(limit[5]) // 1000)
            return state
    f = dev.sh('cat /sys/devices/platform/*.mali/cur_freq 2>/dev/null', check=False).split()
    return dict(mhz=int(f[0]) // 1000) if f and f[0].isdigit() else {}


def gpu_trace(dev, folder, seconds):
    """Per-render-pass GPU timing (perfetto gpu.renderstages, needs root and
    debug.graphics.gpu.profiler.perfetto=1 before the game starts)."""
    config = ('buffers { size_kb: 131072 fill_policy: DISCARD }' + chr(10) + 'duration_ms: %d' % int(seconds * 1000) + chr(10) +
              'data_sources { config { name: "gpu.renderstages" } }' + chr(10))
    remote = '/data/misc/perfetto-traces/fp-city-stages.pftrace'
    dev.run(['shell', 'su', '-c', "'perfetto --txt -c - -o %s; chmod 644 %s'" % (remote, remote)], input=config.encode(), timeout=120, check=False)
    dev.run(['pull', remote, str(folder / 'gpu-stages.pftrace')], timeout=120, check=False)
    dev.su('rm -f ' + remote)


def battery(dev):
    return {k: int(v) for k, v in reversed(re.findall(r'^\s*(level|temperature): (\d+)', dev.sh('dumpsys battery'), re.M))}


def prepare(dev, profile):
    base = '/sdcard/Frostpunk/' + profile
    if dev.sh('test -e %s && echo yes || true' % base).strip() != 'yes':
        seeds = [p for p in SEED_PROFILES if dev.sh('test -e /sdcard/Frostpunk/%s && echo yes || true' % p).strip() == 'yes']
        if not seeds:
            raise RuntimeError('No QA seed profile on this device')
        dev.sh('cp -a /sdcard/Frostpunk/%s %s && rm -rf %s/.migration-backup' % (seeds[0], base, base), timeout=300)
        print('created %s from %s' % (base, seeds[0]), flush=True)
    return base


def seed(dev, base, scale, limit, highlight, game_settings='keep'):
    saves = base + '/data/323190/remote/saves'
    dev.sh('mkdir -p %s && rm -f %s/*.save %s/*.save.alt' % (saves, saves, saves))
    dev.run(['push', SEED / 'autosave.save', saves + '/autosave.save'], timeout=600)
    dev.run(['push', SEED / 'ProfileData.bin', base + '/data/323190/remote/ProfileData.bin'])
    assert dev.sh('sha256sum %s/autosave.save' % saves).split()[0] == SAVE_SHA
    text = ('output_scale=%d\nmobile_effects=1\nobject_highlight=%d\nui_scale=100\ndebug_menu=0\ntouch_offset=64\n'
            'upscaler=0\nframe_limit=%d\n' % (scale, highlight, limit))
    local = OUT / ('port-settings-%s.txt' % dev.serial.replace(':', '_'))
    local.write_text(text, newline='\n')
    dev.run(['push', local, base + '/port-settings.txt'])
    # The game's own graphics options (config.bin): without a copy the QA profile runs the engine
    # defaults, which are heavier than what the player set.  The player's profile is only read.
    settings = 'AppData/11bitstudios/Frostpunk'
    source = '/sdcard/Frostpunk/userdata/%s/config.bin' % settings
    if game_settings == 'player' and dev.sh('test -f %s && echo yes || true' % source).strip() == 'yes':
        dev.sh('mkdir -p %s/%s && cp %s %s/%s/config.bin' % (base, settings, source, base, settings))
    elif game_settings == 'default':
        dev.sh('rm -f %s/%s/config.bin' % (base, settings))
    return (dev.sh('sha256sum %s/%s/config.bin 2>/dev/null || true' % (base, settings)).split() or [''])[0]


def measure(dev, log, folder, name, seconds):
    log.poll()
    lo = len(log.presents())
    start = time.monotonic()
    ticks = thread_ticks(dev, dev.pid)
    freqs, gpus = [], []
    while time.monotonic() - start < seconds:
        time.sleep(2.0)
        log.poll()
        freqs.append(cpufreq(dev))
        gpus.append(gpu_state(dev))
        if not dev.sh('pidof %s || true' % PKG).strip():
            raise RuntimeError('Game stopped during ' + name)
    log.poll()
    rows = log.presents()[lo:]
    cpu = cpu_delta(ticks, thread_ticks(dev, dev.pid))
    clocks = {}
    for policy in (freqs[0] if freqs else {}):
        cur = sorted(f[policy]['cur'] for f in freqs if policy in f)
        cap = sorted(f[policy]['cap'] for f in freqs if policy in f)
        clocks[policy] = dict(cur_median=cur[len(cur) // 2], cap_min=cap[0], cap_median=cap[len(cap) // 2], max=freqs[0][policy]['max'])
    png = shot(dev, folder / (name + '-end.png'))
    modal = bool(acknowledgement_visible(dev, png) or not hud_ready(png))
    gpu = {}
    for field in ('mhz', 'load', 'cpu_limited', 'big_ceiling_mhz', 'middle_ceiling_mhz'):
        values = sorted(g[field] for g in gpus if field in g)
        if values:
            gpu[field] = dict(median=values[len(values) // 2], min=values[0], max=values[-1])
    result = dict(presents=summary(rows), cpu=cpu, clocks=clocks, gpu=gpu, battery=battery(dev), modal_at_end=modal)
    (folder / (name + '.json')).write_text(json.dumps(dict(result, rows=rows), indent=1) + '\n')
    top = ', '.join('%s %.0f%%' % (t['name'], t['pct']) for t in cpu['threads'][:5])
    print('[%s] %-7s fps mean %.2f median %.2f p10 %.2f | cpu %.0f%% (%s) | caps %s%s' % (
        dev.serial, name, result['presents']['mean_fps'], result['presents']['median_fps'], result['presents']['p10_fps'],
        cpu['total_pct'], top, {k: v['cap_min'] for k, v in clocks.items()}, ' | MODAL AT END' if modal else ''), flush=True)
    if gpu:
        print('[%s] %-7s gpu %s' % (dev.serial, name, ', '.join('%s median %d (min %d, max %d)' % (k, v['median'], v['min'], v['max'])
                                                                 for k, v in gpu.items())), flush=True)
    return result


def cpu_stat(dev, log, folder, name, seconds):
    """Cycles and instructions per presented frame for the busiest threads: unlike FPS this does
    not move with the thermal frequency caps, so builds can be compared across runs."""
    log.poll()
    lo = len(log.presents())
    started = time.monotonic()
    out = dev.sh('simpleperf stat --app %s -e cpu-cycles,instructions --per-thread --duration %d 2>&1' % (PKG, seconds),
                 timeout=seconds + 60, check=False)
    elapsed = time.monotonic() - started
    log.poll()
    rows = log.presents()[lo:]
    # Frames presented while the counters ran: whole one-second samples scaled to the counted time.
    frames = (sum(r['fps'] for r in rows) / max(1, len(rows))) * seconds if rows else 1.0
    (folder / (name + '-stat.txt')).write_text(out, encoding='utf-8')
    threads = {}
    # thread_name  pid  tid  count  event_name  # ...
    row = re.compile(r'^\s*(.+?)\s+(\d+)\s+(\d+)\s+([\d,]+)\s+(cpu-cycles|instructions)\b')
    for line in out.splitlines():
        m = row.match(line)
        if m:
            threads.setdefault(m[1] + ':' + m[3], {})[m[5]] = int(m[4].replace(',', ''))
    top = sorted(threads.items(), key=lambda kv: -kv[1].get('cpu-cycles', 0))[:6]
    del elapsed
    result = dict(seconds=seconds, frames=round(frames, 1), fps=round(frames / seconds, 2),
                  threads={k: dict(mcycles_per_frame=round(v.get('cpu-cycles', 0) / frames / 1e6, 2),
                                   minstr_per_frame=round(v.get('instructions', 0) / frames / 1e6, 2)) for k, v in top})
    (folder / (name + '-stat.json')).write_text(json.dumps(result, indent=1) + chr(10))
    print('[%s] %-7s fps %.1f | per frame: %s' % (dev.serial, name, result['fps'], ', '.join(
        '%s %.1fM cyc / %.1fM ins' % (k.split(':')[0], v['mcycles_per_frame'], v['minstr_per_frame']) for k, v in result['threads'].items())), flush=True)
    return result


def volume_note(dev):
    return OUT / ('media-volume-%s.txt' % dev.serial.replace(':', '_'))


def mute(dev):
    """Silence the media stream for an unattended run.  The level before the first mute is kept in a
    note next to the results and put back by restore_volume() when a run stops the game."""
    level = re.search(r'volume is (\d+) in range', dev.sh('cmd media_session volume --stream 3 --get', check=False) or '')
    if level and int(level[1]) > 0:
        volume_note(dev).write_text(level[1])
    dev.sh('cmd media_session volume --stream 3 --set 0', check=False)


def restore_volume(dev):
    note = volume_note(dev)
    if note.exists():
        dev.sh('cmd media_session volume --stream 3 --set %s' % note.read_text().strip(), check=False)
        note.unlink()


def cool_down(dev, limit, patience=900):
    """Wait until the battery thermistor is at or below `limit` (tenths of a degree C)."""
    started = time.monotonic()
    while True:
        temperature = battery(dev).get('temperature', 0)
        if temperature <= limit or time.monotonic() - started > patience:
            print('[%s] battery %.1f C after %.0fs of cooling' % (dev.serial, temperature / 10, time.monotonic() - started), flush=True)
            return temperature
        time.sleep(15)


def soak(dev, log, folder, seconds):
    """Running city with a camera route every 20 s: stability and frame-rate drift over time."""
    log.poll()
    lo = len(log.presents())
    start = time.monotonic()
    routes = 0
    while time.monotonic() - start < seconds:
        route(dev)
        routes += 1
        log.poll()
        if not dev.sh('pidof %s || true' % PKG).strip():
            raise RuntimeError('Game stopped during the soak after %.0fs' % (time.monotonic() - start))
        wait = 20 - (time.monotonic() - start) % 20
        time.sleep(max(1.0, min(wait, 8.0)))
    log.poll()
    rows = log.presents()[lo:]
    text = log.text()
    stops = [line for line in text.splitlines() if line.startswith('STOP ')]
    half = len(rows) // 2
    png = shot(dev, folder / 'soak-end.png')
    result = dict(presents=summary(rows), first_half=summary(rows[:half]), second_half=summary(rows[half:]), routes=routes,
                  stop_lines=stops[:5], modal_at_end=bool(acknowledgement_visible(dev, png) or not hud_ready(png)), battery=battery(dev))
    (folder / 'soak.json').write_text(json.dumps(dict(result, rows=rows), indent=1) + chr(10))
    print('[%s] soak %ds: fps mean %.2f (first half %.2f, second half %.2f) p10 %.2f, routes %d, STOP lines %d' % (
        dev.serial, len(rows), result['presents']['mean_fps'], result['first_half']['mean_fps'], result['second_half']['mean_fps'],
        result['presents']['p10_fps'], routes, len(stops)), flush=True)
    return result


def run(args):
    dev = RetryDevice(args.serial)
    folder = OUT / args.label
    folder.mkdir(parents=True, exist_ok=True)
    result = dict(label=args.label, serial=args.serial, screen=[dev.width, dev.height], scale=args.scale, limit=args.limit)
    dev.sh('am force-stop ' + PKG)
    if args.cool:
        result['cooled_to'] = cool_down(dev, args.cool)
    if args.apk:
        dev.run(['install', '-r', args.apk], timeout=900)
    apk = dev.sh('pm path ' + PKG).strip().splitlines()[0].removeprefix('package:')
    result['apk_sha256'] = dev.sh('sha256sum ' + apk).split()[0]
    base = prepare(dev, args.profile)
    result['game_settings_sha256'] = seed(dev, base, args.scale, args.limit, args.highlight, args.game_settings)
    props = PROPS + (('debug.graphics.gpu.profiler.perfetto',) if args.gpu_trace else ())
    previous = {p: dev.sh('getprop ' + p).strip() for p in props}
    overrides = {'debug.fp.args': args.args, 'debug.fp.quality': '', 'debug.fp.extra_fx': '', 'debug.fp.detail': '',
                 'debug.frostpunk.ui_dump': ''}
    if args.gpu_trace:
        overrides['debug.graphics.gpu.profiler.perfetto'] = '1'
    for item in args.prop or ():
        name, _, value = item.partition('=')
        previous.setdefault(name, dev.sh('getprop ' + name).strip())
        overrides[name] = value
    settings = {name: dev.sh('settings get %s %s' % (ns, name)).strip() for ns, name in
                (('system', 'screen_off_timeout'), ('system', 'screen_brightness'))}
    result['battery_start'] = battery(dev)
    log = None
    try:
        for p, v in overrides.items():
            dev.sh("setprop %s '%s'" % (p, v))
        dev.sh('input keyevent KEYCODE_WAKEUP', check=False)
        dev.sh('wm dismiss-keyguard', check=False)
        dev.sh('settings put system screen_off_timeout 1800000', check=False)
        mute(dev)
        time.sleep(1)
        log = Log(dev)
        t0 = time.monotonic()
        dev.sh('am start -W -n %s/.MainActivity --es userdata %s' % (PKG, args.profile))
        stage, menu_at, tapped_at, retries, before_tap = 'start', None, None, 0, None
        while True:
            now = time.monotonic() - t0
            if now > args.timeout:
                raise RuntimeError('timeout in stage ' + stage)
            if not dev.sh('pidof %s || true' % PKG).strip() and now > 5:
                raise RuntimeError('Game stopped in stage ' + stage)
            log.poll()
            text = log.text()
            rows = log.presents()
            if stage == 'start' and text.count('info:  Presenter: Actual swap chain properties') >= 2 and rows and rows[-1]['fps'] > 5:
                stage = 'title'
                result['title_s'] = round(now, 1)
                time.sleep(1.5)
                key(dev, 'ENTER', 300)
                menu_at = time.monotonic()
            elif stage == 'title' and time.monotonic() - menu_at > 5.0:
                stage = 'menu'
                before_tap = shot(dev, folder / 'menu.png')
                x, y = dev.screen(640, 217)
                dev.sh('input tap %d %d' % (x, y))
                result['resume_tap_s'] = round(time.monotonic() - t0, 1)
                tapped_at = time.monotonic()
            elif stage == 'menu' and 'FP_TOUCH_CAMERA' in text:
                png = shot(dev)
                if hud_ready(png):
                    result['city_s'] = round(now, 1)
                    result['load_s'] = round(now - result['resume_tap_s'], 1)
                    break
            elif stage == 'menu' and retries < 3 and time.monotonic() - tapped_at > 12 and same_screen(shot(dev), before_tap):
                # Twelve seconds after the tap the screen is still the one it was sent to: the key
                # press that leaves the title was lost (seen on the tablet).  Press and tap again.
                retries += 1
                result['title_retries'] = retries
                key(dev, 'ENTER', 300)
                time.sleep(5.0)
                before_tap = shot(dev)
                x, y = dev.screen(640, 217)
                dev.sh('input tap %d %d' % (x, y))
                result['resume_tap_s'] = round(time.monotonic() - t0, 1)
                tapped_at = time.monotonic()
            time.sleep(1.0)
        dev.pid = dev.sh('pidof ' + PKG).split()[0]
        result['mapped_runtime_sha256'] = (dev.sh('run-as %s sha256sum files/native/libfrostpunk_runtime.so' % PKG, check=False).split() or [''])[0]
        print('[%s] city after %.0fs (load %.0fs)' % (dev.serial, result['city_s'], result['load_s']), flush=True)
        key(dev, '1', 400)
        # The save queues a scout acknowledgement some seconds after the simulation starts
        # (later on faster devices): wait for it, close it, and only then measure.
        seen, deadline = 0, time.monotonic() + args.popup_wait
        while time.monotonic() < deadline:
            png = shot(dev)
            if acknowledgement_visible(dev, png):
                seen += 1
                x, y = dev.screen(640, 535)
                # A touch tap works on the phones, a mouse press on the tablet: alternate.
                dev.sh(('input tap %d %d' % (x, y)) if seen % 2 else ('input mouse swipe %d %d %d %d 300' % (x, y, x, y)))
                time.sleep(2.5)
            elif seen and hud_ready(png):
                break
            else:
                time.sleep(2)
        result['acknowledgements_closed'] = seen
        png = shot(dev)
        result['modal_before_windows'] = bool(acknowledgement_visible(dev, png) or not hud_ready(png))
        key(dev, '1', 400)
        time.sleep(max(2, args.warmup))
        shot(dev, folder / 'city-start.png')
        if args.load_only:
            result['success'] = True
            return result
        result['normal'] = measure(dev, log, folder, 'normal', args.seconds)
        key(dev, 'SPACE', 300)
        time.sleep(2)
        result['paused'] = measure(dev, log, folder, 'paused', args.seconds)
        if args.gpu_trace:
            gpu_trace(dev, folder, args.gpu_trace)
        if args.zoom:
            # Close-up of the paused city for picture comparisons: mouse wheel over the screen centre.
            x, y = dev.screen(640, 360)
            for _ in range(abs(args.zoom)):
                dev.sh('input mouse scroll %d %d --axis VSCROLL,%d' % (x, y, 1 if args.zoom > 0 else -1), check=False)
                time.sleep(0.15)
            time.sleep(3)
            shot(dev, folder / 'zoom.png')
            result['zoom'] = measure(dev, log, folder, 'zoom', 8)
        if args.soak:
            key(dev, 'SPACE', 300)
            time.sleep(2)
            result['soak'] = soak(dev, log, folder, args.soak)
            key(dev, 'SPACE', 300)
            time.sleep(2)
        if args.stat:
            key(dev, 'SPACE', 300)
            time.sleep(2)
            result['stat'] = cpu_stat(dev, log, folder, 'normal', args.stat)
            key(dev, 'SPACE', 300)
            time.sleep(2)
        if args.simpleperf:
            # The paused window is the one just measured. SPACE would leave it and record the running city.
            if args.perf_while != 'paused':
                key(dev, 'SPACE', 300)
                time.sleep(2)
            remote = '/data/local/tmp/fp-fastlift-%s.data' % args.label
            perf = subprocess.Popen(dev.adb + ['shell', 'simpleperf record --app %s -e cpu-clock -f %d --call-graph fp --duration %d -o %s'
                                               % (PKG, args.perf_hz, args.simpleperf, remote)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            result['perf'] = measure(dev, log, folder, 'perf', args.simpleperf + 3)
            out, err = perf.communicate(timeout=180)
            (folder / 'perf-status.txt').write_bytes(out + err)
            dev.run(['pull', remote, str(folder / 'perf.data')], timeout=600, check=False)
            dev.sh('rm -f ' + remote, check=False)
        shot(dev, folder / 'city-end.png')
        result['success'] = True
        return result
    except BaseException as error:
        result.update(success=False, error=repr(error))
        if dev.sh('pidof %s || true' % PKG).strip():
            try:
                shot(dev, folder / 'error.png')
            except Exception:
                pass
        raise
    finally:
        if log:
            log.poll()
            (folder / 'native.log').write_bytes(log.data)
        if not args.load_only:
            dev.sh('am force-stop ' + PKG, check=False)
            restore_volume(dev)
        for p, v in previous.items():
            dev.sh("setprop %s '%s'" % (p, v), check=False)
        if settings.get('screen_off_timeout', '').isdigit():
            dev.sh('settings put system screen_off_timeout ' + settings['screen_off_timeout'], check=False)
        result['battery_end'] = battery(dev)
        (folder / 'result.json').write_text(json.dumps(result, indent=1) + '\n')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--label', required=True)
    ap.add_argument('--apk', help='install this APK first')
    ap.add_argument('--profile', default='userdata-qa-fl14')
    ap.add_argument('--scale', type=int, default=20)
    ap.add_argument('--limit', type=int, default=60)
    ap.add_argument('--highlight', type=int, default=0)
    ap.add_argument('--args', default='', help='extra engine switches (debug.fp.args)')
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--warmup', type=int, default=6)
    ap.add_argument('--popup-wait', type=int, default=45)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--simpleperf', type=int, default=0)
    ap.add_argument('--perf-while', choices=('running', 'paused'), default='running',
                    help='record simpleperf in the running city (unpause first) or in the pause that was just measured')
    ap.add_argument('--game-settings', choices=('keep', 'player', 'default'), default='keep',
                    help="the game's own options in the QA profile: keep what is there, copy the player's config.bin (read only), or engine defaults")
    ap.add_argument('--zoom', type=int, default=0, help='mouse-wheel steps towards the city in the paused state, then zoom.png and a short window')
    ap.add_argument('--prop', action='append', help='NAME=VALUE system property for this run (restored afterwards), e.g. debug.fp.drawstats=1')
    ap.add_argument('--gpu-trace', type=float, default=0, help='seconds of perfetto gpu.renderstages in the paused city (rooted Mali)')
    ap.add_argument('--cool', type=int, default=0, help='wait until the battery is at or below this (tenths of a degree C)')
    ap.add_argument('--soak', type=int, default=0, help='seconds of running city with camera routes (stability)')
    ap.add_argument('--stat', type=int, default=0, help='seconds of per-thread cycle/instruction counting (running city)')
    ap.add_argument('--perf-hz', type=int, default=499)
    ap.add_argument('--load-only', action='store_true')
    run(ap.parse_args())
