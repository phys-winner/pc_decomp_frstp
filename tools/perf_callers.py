"""Who calls a hot symbol in a recorded city profile (frame-pointer call chains of simpleperf).

    python -X utf8 tools/perf_callers.py --label NAME --runtime build/fastlift/NAME/libfrostpunk_runtime.so
                                         --thread Renderer --symbol syscall [--symbol __aarch64_cas4_acq_rel] [--depth 4]

For every sample of the thread whose innermost frame is one of the symbols it prints the busiest call
chains (innermost first, `depth` callers) with their share of the thread's samples.  With --inclusive
the symbol may sit anywhere in the chain (time spent under it); with --callees the time under the
symbol is split by the `depth` frames below it (what it calls) instead of by its callers.
"""
import argparse
import collections
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/perf/fastlift'
NDK = Path(r'C:\Users') / Path.home().name / r'AppData\Local\Android\Sdk\ndk\27.2.12479018'
sys.path.insert(0, str(NDK / 'simpleperf'))
sys.path.insert(0, str(ROOT / 'tools'))
from simpleperf_report_lib import ReportLib  # noqa: E402
from perf_lifted_report import symbol_root  # noqa: E402


def short(name):
    return name if len(name) <= 72 else name[:69] + '...'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', required=True)
    ap.add_argument('--runtime', required=True)
    ap.add_argument('--thread', required=True)
    ap.add_argument('--symbol', action='append', required=True)
    ap.add_argument('--depth', type=int, default=4)
    ap.add_argument('--rows', type=int, default=14)
    ap.add_argument('--inclusive', action='store_true')
    ap.add_argument('--callees', action='store_true')
    args = ap.parse_args()
    lib = ReportLib()
    lib.SetSymfs(str(symbol_root(args.label, args.runtime, OUT / args.label / 'perf.data')))
    lib.SetRecordFile(str(OUT / args.label / 'perf.data'))
    total = 0
    chains = {name: collections.Counter() for name in args.symbol}
    hits = collections.Counter()
    while True:
        sample = lib.GetNextSample()
        if sample is None:
            break
        if sample.thread_comm != args.thread:
            continue
        total += sample.period
        symbol = lib.GetSymbolOfCurrentSample()
        chain = lib.GetCallChainOfCurrentSample()
        frames = [symbol.symbol_name] + [chain.entries[i].symbol.symbol_name for i in range(chain.nr)]
        for name in args.symbol:
            if args.callees:
                if name in frames:
                    at = frames.index(name)
                    hits[name] += sample.period
                    below = frames[max(0, at - args.depth):at]
                    chains[name][tuple(reversed(below)) or ('(self)',)] += sample.period
            elif args.inclusive:
                if name in frames:
                    at = frames.index(name)
                    hits[name] += sample.period
                    chains[name][tuple(frames[at + 1:at + 1 + args.depth])] += sample.period
            elif frames[0] == name:
                hits[name] += sample.period
                chains[name][tuple(frames[1:1 + args.depth])] += sample.period
    lib.Close()
    for name in args.symbol:
        mode = ' (time under it, by callee)' if args.callees else ' (inclusive)' if args.inclusive else ''
        print('== %s: %.2f%% of %s%s' % (name, 100.0 * hits[name] / max(1, total), args.thread, mode))
        for chain, period in chains[name].most_common(args.rows):
            print('   %5.2f%%  %s' % (100.0 * period / max(1, total), (' -> ' if args.callees else ' <- ').join(short(f) for f in chain) or '(no callers)'))


if __name__ == '__main__':
    main()
