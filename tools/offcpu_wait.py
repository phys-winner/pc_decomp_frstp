"""Where a thread waits: off-CPU samples (simpleperf --trace-offcpu) grouped by blocking call chain.

    python tools/offcpu_wait.py PERF_DATA SYMFS THREAD [DEPTH]
"""
import collections
import json
import re
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
_CFG = json.loads((ROOT / 'port.json').read_text())
NDK = Path(_CFG['android_sdk']) / 'ndk' / _CFG['android_ndk_version']
sys.path.insert(0, str(NDK / 'simpleperf'))
from simpleperf_report_lib import ReportLib  # noqa: E402

data, symfs, thread = sys.argv[1], sys.argv[2], sys.argv[3]
depth = int(sys.argv[4]) if len(sys.argv) > 4 else 7
lib = ReportLib(); lib.SetRecordFile(data); lib.SetSymfs(symfs); lib.ShowIpForUnknownSymbol()
agg, total, oncpu = collections.Counter(), 0, 0
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    if s.thread_comm != thread:
        continue
    period = s.period
    total += period
    ev = lib.GetEventOfCurrentSample().name
    sym = lib.GetSymbolOfCurrentSample().symbol_name
    chain = lib.GetCallChainOfCurrentSample()
    frames = [sym] + [chain.entries[i].symbol.symbol_name for i in range(chain.nr)]
    # Skip kernel frames: the user-space caller is what blocked.
    user = [f for f in frames if not re.match(r'(__schedule|schedule|futex_|do_futex|__arm64_sys|invoke_syscall|el0|do_el0|_raw_spin|try_to_wake|wake_up|\[kernel)', f)]
    frames = user[:depth] if user else frames[:depth]
    agg[' <- '.join(f.split('(')[0][-60:] for f in frames)] += period
print('thread %s total %.2f s' % (thread, total / 1e9))
for k, v in agg.most_common(25):
    print('%5.1f%%  %s' % (100.0 * v / total, k))
