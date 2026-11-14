"""Per-frame GPU job breakdown of a Mali gpu.renderstages trace (tools/perf_session.py --gpu-trace).

    python tools/gpu_frame_stats.py TRACE [TRACE...]

Frames start at the long G-buffer vertex job (> 3 ms); prints medians over complete frames:
frame span, busy union, the G-buffer vertex/fragment jobs, the five largest other jobs and totals.
"""
import statistics
import sys
from perfetto.trace_processor import TraceProcessor


def frames(path):
    tp = TraceProcessor(trace=path)
    rows = list(tp.query("select ts, dur, name from gpu_slice where name in ('vertex','fragment','compute','copy image') order by ts"))
    tp.close()
    starts = [i for i, r in enumerate(rows) if r.name == 'vertex' and r.dur > 3e6]
    out = []
    for a, b in zip(starts, starts[1:]):
        f = rows[a:b]
        spans, busy, cur = sorted((r.ts, r.ts + r.dur) for r in f), 0, None
        for x, y in spans:
            if cur is None or x > cur[1]:
                busy += (cur[1] - cur[0]) if cur else 0
                cur = [x, y]
            else:
                cur[1] = max(cur[1], y)
        busy += cur[1] - cur[0]
        tot = {}
        for r in f:
            tot[r.name] = tot.get(r.name, 0) + r.dur
        rest = sorted((r.dur for r in f[2:]), reverse=True)[:5]
        out.append(dict(span=(rows[b].ts - rows[a].ts) / 1e6, busy=busy / 1e6, gb_vertex=f[0].dur / 1e6,
                        gb_fragment=f[1].dur / 1e6 if f[1].name == 'fragment' else 0,
                        top5=[x / 1e6 for x in rest], **{k: v / 1e6 for k, v in tot.items()}))
    return out


for path in sys.argv[1:]:
    fr = frames(path)
    med = lambda k: round(statistics.median(x.get(k, 0) for x in fr), 2)
    top = [round(statistics.median(x['top5'][i] for x in fr if len(x['top5']) > i), 2) for i in range(5)]
    print('%s frames=%d span=%s busy=%s gbuffer_vertex=%s gbuffer_fragment=%s vertex=%s fragment=%s compute=%s top5=%s' % (
        path, len(fr), med('span'), med('busy'), med('gb_vertex'), med('gb_fragment'), med('vertex'), med('fragment'), med('compute'), top))
