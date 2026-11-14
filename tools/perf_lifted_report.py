"""Where a recorded city profile spends its CPU time: lifted guest code against host code, per thread.

    python -X utf8 tools/perf_lifted_report.py --label NAME --runtime build/fastlift/NAME/libfrostpunk_runtime.so
                                               [--threads Thread-22 Renderer] [--top 24] [--function L_000140F8F530]

Reads build/perf/fastlift/<label>/perf.data (recorded by `tools/perf_city.py --simpleperf N`), puts the
unstripped runtime of exactly that build under a symbol root and prints, per thread, the share of
lifted guest functions, host C of the runtime and other libraries, the busiest symbols of each group
and how many lifted functions cover the time.  --function prints the hottest instruction addresses
of one symbol (offsets inside the runtime) to tell a cache-missing load from spread-out work.
"""
import argparse
import collections
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/perf/fastlift'
NDK = Path(r'C:\Users') / Path.home().name / r'AppData\Local\Android\Sdk\ndk\27.2.12479018'
SIMPLEPERF = NDK / r'simpleperf\bin\windows\x86_64\simpleperf.exe'
NM = NDK / r'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe'
DEVICE_LIB = 'data/data/com.frostpunk.staticrecomp/files/native/libfrostpunk_runtime.so'
# The APK carries this library stripped; this file has the same .text and a symbol table.
DXVK_SYMBOLS = ROOT / 'build/qa-ha22-20261113/dxvk-candidate/libdxvk_d3d11.so'
LIFTED = re.compile(r'^(L_[0-9A-F]{12}|LJ_[0-9A-F]{12}|fp_original_L_|fp_quality_original|fp_bounds_original|fp_frustum_original)')


def symbol_root(label, runtime, data=None):
    """A --symfs tree with the unstripped runtime of the profiled build and, when the profile is
    given, the unstripped DXVK (same code as the APK copy) under the install path the profile names."""
    root = OUT / ('symfs-' + label)
    wanted = {DEVICE_LIB: Path(runtime)}
    if data is not None and DXVK_SYMBOLS.exists():
        listing = subprocess.run([str(SIMPLEPERF), 'report', '-i', str(data), '--sort', 'dso'],
                                 capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
        for line in listing.splitlines():
            m = re.match(r'^\d+\.\d+%\s+(/\S+/libdxvk_d3d11\.so)$', line.rstrip())
            if m:
                wanted[m[1].lstrip('/')] = DXVK_SYMBOLS
    for relative, source in wanted.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.stat().st_size != source.stat().st_size or target.stat().st_mtime < source.stat().st_mtime:
            shutil.copyfile(source, target)
    return root


def report(data, root, *extra):
    done = subprocess.run([str(SIMPLEPERF), 'report', '-i', str(data), '--symfs', str(root), *extra],
                          capture_output=True, text=True, encoding='utf-8', errors='replace')
    return done.stdout


def thread_rows(data, root, thread):
    rows = []
    for line in report(data, root, '--comms', thread, '--sort', 'dso,symbol').splitlines():
        m = re.match(r'^(\d+\.\d+)%\s+(\S+)\s+(.*)$', line.rstrip())
        if m:
            rows.append((float(m[1]), m[2].rsplit('/', 1)[-1], m[3].strip()))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--label', required=True)
    ap.add_argument('--runtime', required=True, help='unstripped libfrostpunk_runtime.so of the profiled build')
    ap.add_argument('--threads', nargs='+', default=['Thread-22', 'Renderer'])
    ap.add_argument('--top', type=int, default=24)
    ap.add_argument('--function', action='append', default=[], help='hottest addresses inside this symbol')
    args = ap.parse_args()
    folder = OUT / args.label
    data = folder / 'perf.data'
    root = symbol_root(args.label, args.runtime, data)
    for thread in args.threads:
        rows = thread_rows(data, root, thread)
        (folder / ('report-%s.txt' % thread)).write_text(''.join('%6.2f%%  %s  %s\n' % r for r in rows), encoding='utf-8')
        groups, hosts = collections.Counter(), collections.Counter()
        for share, dso, symbol in rows:
            if dso == 'libfrostpunk_runtime.so':
                if LIFTED.match(symbol):
                    groups['runtime: lifted'] += share
                else:
                    groups['runtime: host C'] += share
                    hosts[symbol] += share
            else:
                groups[dso] += share
                hosts[dso + ':' + symbol] += share
        print('==', thread)
        for name, share in groups.most_common(8):
            print('   %6.2f%%  %s' % (share, name))
        print('   -- top non-lifted:', ', '.join('%s %.2f' % (k[:52], v) for k, v in hosts.most_common(args.top)))
        lifted = sorted(((s, n) for s, d, n in rows if d == 'libfrostpunk_runtime.so' and LIFTED.match(n)), reverse=True)
        print('   -- top lifted:', ', '.join('%s %.2f' % (n, s) for s, n in lifted[:args.top]))
        total = 0.0
        for index, (share, _) in enumerate(lifted, 1):
            total += share
            if index in (10, 30, 100, 300, 1000):
                print('      top %d lifted cover %.1f%%' % (index, total))
    if args.function:
        symbols = subprocess.run([str(NM), '-n', '--defined-only', str(args.runtime)], capture_output=True, text=True).stdout.splitlines()
        starts = {}
        for index, line in enumerate(symbols):
            f = line.split()
            if len(f) == 3 and f[2] in args.function:
                following = symbols[index + 1].split()[0] if index + 1 < len(symbols) else f[0]
                starts[f[2]] = (int(f[0], 16), int(following, 16))
        for name in args.function:
            if name not in starts:
                print('== %s: not in the runtime' % name)
                continue
            begin, end = starts[name]
            print('== %s at 0x%x, %d bytes' % (name, begin, end - begin))
            text = report(data, root, '--comms', *args.threads, '--symbols', name, '--sort', 'vaddr_in_file')
            shown = 0
            for line in text.splitlines():
                m = re.match(r'^(\d+\.\d+)%\s+(0x[0-9a-f]+)', line)
                if m and shown < 28:
                    print('   %6.2f%%  +0x%x' % (float(m[1]), int(m[2], 16) - begin))
                    shown += 1


if __name__ == '__main__':
    main()
