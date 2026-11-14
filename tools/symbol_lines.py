"""Hot source lines of one function in a simpleperf recording (llvm-symbolizer on sample addresses).

    python tools/symbol_lines.py PERF_DATA SYMFS FUNCTION LOCAL_ELF [THREAD]
"""
import collections
import json
import subprocess
import sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
_CFG = json.loads((ROOT / 'port.json').read_text())
NDK = Path(_CFG['android_sdk']) / 'ndk' / _CFG['android_ndk_version']
sys.path.insert(0, str(NDK / 'simpleperf'))
from simpleperf_report_lib import ReportLib  # noqa: E402

data, symfs, function, elf = sys.argv[1:5]
thread = sys.argv[5] if len(sys.argv) > 5 else None
lib = ReportLib(); lib.SetRecordFile(data); lib.SetSymfs(symfs)
addrs = collections.Counter()
while True:
    s = lib.GetNextSample()
    if s is None:
        break
    if thread and s.thread_comm != thread:
        continue
    sym = lib.GetSymbolOfCurrentSample()
    if sym.symbol_name == function:
        addrs[sym.vaddr_in_file] += 1
SYM = str(NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-symbolizer.exe')
items = sorted(addrs.items())
out = subprocess.run([SYM, '--obj=' + elf, '--inlines'], input='\n'.join('0x%x' % a for a, _ in items), capture_output=True, text=True).stdout
blocks = [b for b in out.split('\n\n') if b.strip()]
lines = collections.Counter()
for (a, n), b in zip(items, blocks):
    loc = b.splitlines()[1] if len(b.splitlines()) > 1 else '?'
    lines[loc.replace(chr(92), '/').split('/')[-1]] += n
total = sum(addrs.values())
print(function, 'samples', total)
for k, v in lines.most_common(15):
    print('%5.1f%% %s' % (100.0 * v / total, k))
