"""Build guard over the lifted C that a build compiles: fails on two known wrong lowerings.

    python -X utf8 tools/lowering_guard.py [--overlay DIR] [PATH ...]

1. a `lock` read-modify-write or `xchg [mem]` lowered as a plain load/op/store, or MSVC's
   `lock or dword ptr [rsp], 0` without a fence (tools/atomic_sites.py has the inventory and repair);
2. a packed compare (`cmp<pred>ps` / `cmp<pred>pd`) lowered as lift64's float compare of lane 0:
   `{ float _a = c->xmm[N].f32[0], _b = ...; c->xmm[N].u32[0] = ... }` (lanes 1-3 keep the old bits).

Default paths: what the Android runtime links (build/fastlift/src, the payload and msvcp140 trees);
--overlay DIR takes a side-tree file from DIR/<same relative path> when it exists there (the
byte-proven copies of tools/atomic_sites.py overlay).  Exit status 1 when anything is found.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import atomic_sites  # noqa: E402

DEFAULT = ['build/fastlift/src', 'generated/payload/recomp', 'generated/dependencies/msvcp140/recomp']
PACKED = re.compile(r'/\* ([0-9A-F]{12}): (cmp(?:eq|lt|le|unord|neq|nlt|nle|ord)p[sd]) [^*]*\*/')
LANE0 = re.compile(r'^\s*\{ (?:float|double) _a = c->xmm\[\d+\]\.f(?:32|64)\[0\], _b = [^;]*; c->xmm\[\d+\]\.u(?:32|64)\[0\] = [^;]*; \}')


def files(paths, overlay):
    for item in paths:
        path = ROOT / item
        for f in sorted(path.glob('recomp_*.c') if path.is_dir() else [path]):
            if overlay:
                alt = Path(overlay) / f.relative_to(ROOT)
                if alt.exists():
                    f = alt
            yield f


def scan(paths, overlay=None):
    atomic, packed, counts = [], [], dict(files=0, packed_lines=0, lock_sites=0)
    for f in files(paths, overlay):
        counts['files'] += 1
        text = f.read_text(encoding='utf-8', errors='replace')
        lines = text.splitlines()
        for number, line in enumerate(lines, 1):
            m = PACKED.search(line)
            if m:
                counts['packed_lines'] += 1
                if LANE0.match(line):
                    packed.append('%s:%d %s %s' % (f, number, m[1], m[2]))
        # The lock/xchg check reuses atomic_sites' grouping of multi-line instructions.
        rows = scan_locks(f, lines)
        counts['lock_sites'] += len(rows)
        atomic += ['%s:%d %s %s' % (f, r[0], r[1], r[2]) for r in rows if r[3] == 'plain']
    return atomic, packed, counts


def scan_locks(path, lines):
    out = []
    for number, line in enumerate(lines, 1):
        if ': lock ' not in line and ': xchg ' not in line:
            continue
        m = atomic_sites.LINE.match(line)
        if not m:
            continue
        form = atomic_sites.classify(m[4])
        if form in ('xchg-reg',):
            continue
        if number >= 2 and atomic_sites.LINE.match(lines[number - 2] or '') and ('/* %s:' % m[3]) in lines[number - 2]:
            continue   # a continuation line of the same instruction
        code = [m[2]]
        depth, extra = m[2].count('{') - m[2].count('}'), number
        while extra < len(lines) and extra - number < 8:
            nxt = lines[extra]
            same = atomic_sites.LINE.match(nxt)
            if same and same[3] == m[3]:
                code.append(same[2]); extra += 1; continue
            if depth > 0 and not same:
                code.append(nxt); depth += nxt.count('{') - nxt.count('}'); extra += 1; continue
            break
        out.append((number, '0x' + m[3], m[4], atomic_sites.status(form, ' '.join(code))))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--overlay', type=Path)
    ap.add_argument('paths', nargs='*', default=DEFAULT)
    args = ap.parse_args()
    atomic, packed, counts = scan(args.paths, args.overlay)
    for row in atomic[:20]:
        print('PLAIN_LOCK ' + row)
    for row in packed[:20]:
        print('LANE0_PACKED_COMPARE ' + row)
    print('lowering_guard: %d files, %d lock/xchg-mem sites (%d plain), %d packed-compare lines (%d lane-0)'
          % (counts['files'], counts['lock_sites'], len(atomic), counts['packed_lines'], len(packed)))
    raise SystemExit(1 if atomic or packed else 0)


if __name__ == '__main__':
    main()
