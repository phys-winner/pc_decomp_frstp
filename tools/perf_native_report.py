"""Per-thread CPU consumers of a recorded city profile in native Vulkan mode (all threads, all libraries).

    python -X utf8 tools/perf_native_report.py --label NAME --runtime UNSTRIPPED_RUNTIME --rdvk UNSTRIPPED_RDVK
                                               [--threads N] [--top N] [--inclusive SYMBOL ...]

Reads build/perf/fastlift/<label>/perf.data (tools/perf_city.py --simpleperf N).  The symbol root gets the
unstripped runtime under its device path and a build_id_list for every --lib (the native Vulkan library is
mapped straight from base.apk, so only its build id can find it).  Prints, per thread: share of the process,
library split, the hottest self symbols and (with --inclusive) the time under given symbols.
Writes <label>/native-report.json for later comparison.
"""
import argparse
import collections
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/perf/fastlift'
NDK = Path(r'C:\Users') / Path.home().name / r'AppData\Local\Android\Sdk\ndk\27.2.12479018'
sys.path.insert(0, str(NDK / 'simpleperf'))
from simpleperf_report_lib import ReportLib  # noqa: E402

READELF = NDK / r'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe'
DEVICE_RUNTIME = 'data/data/com.frostpunk.staticrecomp/files/native/libfrostpunk_runtime.so'


def build_id(path):
    text = subprocess.run([str(READELF), '-n', str(path)], capture_output=True, text=True).stdout
    for line in text.splitlines():
        if 'Build ID:' in line:
            return line.split('Build ID:')[1].strip()
    raise ValueError('no build id in %s' % path)


def symbol_root(label, runtime, libs):
    root = OUT / ('symfs-' + label)
    entries = [(DEVICE_RUNTIME, Path(runtime))] + [('libs/' + Path(lib).name, Path(lib)) for lib in libs]
    lines = []
    for relative, source in entries:
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.stat().st_size != source.stat().st_size or target.stat().st_mtime < source.stat().st_mtime:
            shutil.copyfile(source, target)
        lines.append('0x%s=%s\n' % (build_id(source), relative.replace('/', '\\') if sys.platform == 'win32' else relative))
    (root / 'build_id_list').write_bytes(''.join(lines).encode())
    return root


def group_of(dso, symbol):
    name = dso.rsplit('/', 1)[-1]
    if name == 'libfrostpunk_runtime.so':
        if symbol.startswith(('L_', 'LJ_', 'fp_original_L_', 'fp_d3d11_L_')):
            return 'lifted game code'
        return 'runtime host C'
    if name.startswith('[kernel'):
        return 'kernel'
    return name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', required=True)
    ap.add_argument('--runtime', required=True)
    ap.add_argument('--lib', action='append', default=[], help='unstripped library with the build id of the profiled one')
    ap.add_argument('--threads', type=int, default=14)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--inclusive', action='append', default=[])
    ap.add_argument('--callers', action='append', default=[], help='print the call chains above this self symbol')
    ap.add_argument('--callees', action='append', default=[], help='split the time under this symbol by what it calls')
    ap.add_argument('--under', action='append', default=[], help='split the time under this symbol by its callers')
    ap.add_argument('--tree', help='top-down inclusive call tree of --thread from this symbol')
    ap.add_argument('--tree-depth', type=int, default=6)
    ap.add_argument('--min', type=float, default=1.0, help='smallest share of the thread a tree node needs')
    ap.add_argument('--thread', help='only this thread for --callers/--callees')
    ap.add_argument('--depth', type=int, default=4)
    ap.add_argument('--rows', type=int, default=12)
    args = ap.parse_args()
    folder = OUT / args.label
    root = symbol_root(args.label, args.runtime, args.lib)
    lib = ReportLib()
    lib.SetLogSeverity('error')
    lib.SetSymfs(str(root))
    lib.SetRecordFile(str(folder / 'perf.data'))
    total = 0
    per_thread = collections.Counter()
    groups = collections.defaultdict(collections.Counter)
    selfs = collections.defaultdict(collections.Counter)
    under = collections.defaultdict(collections.Counter)
    tids = collections.defaultdict(set)
    chains = {name: collections.Counter() for name in args.callers + args.callees + args.under}
    hits, thread_total = collections.Counter(), collections.Counter()
    tree = {}
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        name = sample.thread_comm
        tids[name].add(sample.tid)
        period = sample.period
        total += period
        per_thread[name] += period
        symbol = lib.GetSymbolOfCurrentSample()
        dso, sym = symbol.dso_name, symbol.symbol_name
        groups[name][group_of(dso, sym)] += period
        selfs[name][(dso.rsplit('/', 1)[-1], sym)] += period
        if args.tree and name == args.thread:
            chain = lib.GetCallChainOfCurrentSample()
            outer = [chain.entries[i].symbol.symbol_name for i in range(chain.nr - 1, -1, -1)] + [sym]
            thread_total[name] += 0
            if args.tree in outer:
                node = tree
                for frame in outer[outer.index(args.tree):][:args.tree_depth + 1]:
                    entry = node.setdefault(frame, [0, {}])
                    entry[0] += period
                    node = entry[1]
        if args.inclusive or chains:
            chain = lib.GetCallChainOfCurrentSample()
            ordered = [sym] + [chain.entries[i].symbol.symbol_name for i in range(chain.nr)]
            frames = set(ordered)
            for wanted in args.inclusive:
                if wanted in frames:
                    under[name][wanted] += period
            if chains and (not args.thread or name == args.thread):
                thread_total[name] += period
                for wanted in args.callers:
                    if ordered[0] == wanted:
                        hits[wanted] += period
                        chains[wanted][tuple(ordered[1:1 + args.depth])] += period
                for wanted in args.under:
                    if wanted in frames:
                        at = ordered.index(wanted)
                        hits[wanted] += period
                        chains[wanted][tuple(ordered[at + 1:at + 1 + args.depth])] += period
                for wanted in args.callees:
                    if wanted in frames:
                        at = ordered.index(wanted)
                        hits[wanted] += period
                        chains[wanted][tuple(reversed(ordered[max(0, at - args.depth):at])) or ('(self)',)] += period
    lib.Close()
    report = dict(label=args.label, total=total, threads={})
    print('process samples %d (period sum)' % total)
    for name, period in per_thread.most_common(args.threads):
        share = 100.0 * period / max(1, total)
        row = dict(share=round(share, 2), tids=sorted(tids[name]),
                   groups={g: round(100.0 * v / period, 2) for g, v in groups[name].most_common()},
                   self=[(d, s, round(100.0 * v / period, 2)) for (d, s), v in selfs[name].most_common(40)],
                   inclusive={k: round(100.0 * v / period, 2) for k, v in under[name].items()})
        report['threads'][name] = row
        print('== %-22s %5.1f%% of process  tids %s' % (name, share, row['tids'][:4]))
        print('   groups: ' + ', '.join('%s %.1f' % kv for kv in list(row['groups'].items())[:7]))
        print('   self:   ' + ', '.join('%s %.1f' % (s[:48], v) for d, s, v in row['self'][:args.top]))
        if row['inclusive']:
            print('   under:  ' + ', '.join('%s %.1f' % kv for kv in sorted(row['inclusive'].items(), key=lambda kv: -kv[1])))
    if report['threads']:
        (folder / 'native-report.json').write_text(json.dumps(report, indent=1) + '\n')
    base = sum(thread_total.values())
    if args.tree:
        whole = per_thread[args.thread]

        def show(node, indent):
            for frame, (period, children) in sorted(node.items(), key=lambda kv: -kv[1][0]):
                share = 100.0 * period / max(1, whole)
                if share < args.min:
                    continue
                print('%s%5.1f%%  %s' % ('  ' * indent, share, frame[:90]))
                show(children, indent + 1)
        print('== tree of %s from %s (share of the thread)' % (args.thread, args.tree))
        show(tree, 0)
    for wanted, counter in chains.items():
        kind = 'callees' if wanted in args.callees else 'under' if wanted in args.under else 'callers'
        print('== %s %s: %.2f%% of %s' % (kind, wanted, 100.0 * hits[wanted] / max(1, base), args.thread or 'all threads'))
        for chain, period in counter.most_common(args.rows):
            joint = ' -> ' if kind == 'callees' else ' <- '
            print('   %5.2f%%  %s' % (100.0 * period / max(1, base), joint.join(f[:60] for f in chain) or '(none)'))


if __name__ == '__main__':
    main()
