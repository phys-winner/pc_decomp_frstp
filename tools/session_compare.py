"""Compare perf_session.py runs: FPS per window, per-thread CPU (% of a core and per frame), memory.

    python tools/session_compare.py LABEL_A LABEL_B [...]
"""
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
THREADS = ('main', 'Renderer', 'SoundEngine', 'Animations', 'dxvk-cs', 'dxvk-queue')


def load(label):
    d = json.loads((ROOT / 'build/perf' / label / 'session.json').read_text())
    fps = {k: v['mean_fps'] for k, v in d['windows'].items() if v}
    out = dict(label=label, apk=d['apk_sha256'][:8], fps=fps, hang=d.get('hang'))
    for phase in ('paused', 'soak'):
        rows = [r for r in d['samples'] if r['phase'] == phase]
        if phase == 'soak':
            rows = rows[1:]  # skip the sample spanning the window switch
        if not rows:
            continue
        cpu = {}
        for r in rows:
            for t in r['cpu']['threads']:
                name = t['name']
                if name.startswith('Thread-') and t['pct'] > 25:
                    name = 'main'
                cpu.setdefault(name, []).append(t['pct'])
        f = fps.get(phase) or 1
        out[phase] = {n: (round(statistics.mean(cpu[n]), 1), round(statistics.mean(cpu[n]) / f, 3)) for n in THREADS if n in cpu}
        out[phase]['total'] = round(statistics.mean(r['cpu']['total_pct'] for r in rows), 1)
        mem = rows[-1]['mem']
        out[phase + '_mem'] = dict(pss=mem['summary']['pss'] // 1024, gl=(mem.get('GL mtrack') or {}).get('pss', 0) // 1024,
                                   native=((mem.get('Native Heap') or {}).get('heap_alloc') or 0) // 1024)
    return out


for label in sys.argv[1:]:
    r = load(label)
    print('== %s apk=%s hang=%s' % (r['label'], r['apk'], r['hang']))
    print('   fps', json.dumps(r['fps']))
    for phase in ('paused', 'soak'):
        if phase in r:
            print('   %-6s cpu%%/per-frame %s' % (phase, json.dumps(r[phase])))
            print('   %-6s mem MB %s' % (phase, json.dumps(r[phase + '_mem'])))
