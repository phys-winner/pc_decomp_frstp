"""Sample only this workspace's verified recompiled Windows process."""
import argparse
import bisect
from collections import Counter
import ctypes
from ctypes import wintypes as w
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/windows'
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
INVALID_HANDLE = ctypes.c_void_p(-1).value


class ThreadEntry(ctypes.Structure):
    _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('tid', w.DWORD), ('pid', w.DWORD),
                ('base_priority', w.LONG), ('delta_priority', w.LONG), ('flags', w.DWORD)]


class ModuleEntry(ctypes.Structure):
    _fields_ = [('size', w.DWORD), ('module_id', w.DWORD), ('pid', w.DWORD), ('global_usage', w.DWORD),
                ('process_usage', w.DWORD), ('base', ctypes.c_void_p), ('bytes', w.DWORD),
                ('module', w.HMODULE), ('name', w.WCHAR * 256), ('path', w.WCHAR * 260)]


def api(name, result, *arguments):
    function = getattr(kernel, name)
    function.restype = result
    function.argtypes = list(arguments)
    return function


close = api('CloseHandle', w.BOOL, w.HANDLE)
snapshot = api('CreateToolhelp32Snapshot', w.HANDLE, w.DWORD, w.DWORD)
thread_first = api('Thread32First', w.BOOL, w.HANDLE, ctypes.POINTER(ThreadEntry))
thread_next = api('Thread32Next', w.BOOL, w.HANDLE, ctypes.POINTER(ThreadEntry))
module_first = api('Module32FirstW', w.BOOL, w.HANDLE, ctypes.POINTER(ModuleEntry))
module_next = api('Module32NextW', w.BOOL, w.HANDLE, ctypes.POINTER(ModuleEntry))
open_process = api('OpenProcess', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
query_path = api('QueryFullProcessImageNameW', w.BOOL, w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD))
wait = api('WaitForSingleObject', w.DWORD, w.HANDLE, w.DWORD)
open_thread = api('OpenThread', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
thread_owner = api('GetProcessIdOfThread', w.DWORD, w.HANDLE)
thread_times = api('GetThreadTimes', w.BOOL, w.HANDLE, ctypes.POINTER(w.FILETIME),
                   ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME))
suspend = api('SuspendThread', w.DWORD, w.HANDLE)
resume = api('ResumeThread', w.DWORD, w.HANDLE)
context = api('GetThreadContext', w.BOOL, w.HANDLE, ctypes.c_void_p)
read_memory = api('ReadProcessMemory', w.BOOL, w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))


def entries(flags, pid, kind, first, following):
    handle = snapshot(flags, pid)
    if handle == INVALID_HANDLE:
        return []
    rows = []
    try:
        row = kind()
        row.size = ctypes.sizeof(row)
        available = first(handle, ctypes.byref(row))
        while available:
            rows.append(kind.from_buffer_copy(row))
            row.size = ctypes.sizeof(row)
            available = following(handle, ctypes.byref(row))
    finally:
        close(handle)
    return rows


def sample(tid, pid, process):
    handle = open_thread(0x004a, False, tid)  # SUSPEND_RESUME, GET_CONTEXT, QUERY_INFORMATION.
    if not handle:
        return None
    try:
        if thread_owner(handle) != pid:
            return None
        times = [w.FILETIME() for _ in range(4)]
        if not thread_times(handle, *[ctypes.byref(value) for value in times]):
            return None
        cpu = sum((int(value.dwHighDateTime) << 32) | value.dwLowDateTime for value in times[2:])
        storage = ctypes.create_string_buffer(1232 + 15)
        aligned = (ctypes.addressof(storage) + 15) & ~15
        ctypes.c_uint32.from_address(aligned + 48).value = 0x100001  # AMD64 CONTEXT_CONTROL.
        previous = 0xffffffff
        try:
            previous = suspend(handle)
            if previous == 0xffffffff:
                return None
            if not context(handle, aligned):
                return None
            rip = ctypes.c_uint64.from_address(aligned + 248).value
            rsp = ctypes.c_uint64.from_address(aligned + 152).value
            stack = ctypes.create_string_buffer(512)
            got = ctypes.c_size_t()
            read_memory(process, rsp, stack, len(stack), ctypes.byref(got))
            stack_bytes = stack.raw[:got.value]
        finally:
            if previous != 0xffffffff and resume(handle) == 0xffffffff:
                raise ctypes.WinError(ctypes.get_last_error())
        words = [int.from_bytes(stack_bytes[i:i + 8], 'little') for i in range(0, len(stack_bytes) - 7, 8)]
        return cpu, rip, words
    finally:
        close(handle)


def parse_map(text):
    preferred = re.search(r'Preferred load address is ([0-9a-fA-F]+)', text)
    if not preferred:
        raise ValueError('Missing preferred load address in linker map')
    symbols = sorted((int(address, 16), name) for name, address in re.findall(
        r'^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\b', text, re.M))
    if not symbols:
        raise ValueError('No function symbols in linker map')
    return int(preferred[1], 16), symbols


def describe(pc, modules, image_path, preferred, symbols, addresses):
    for module in modules:
        if module.base <= pc < module.base + module.bytes:
            offset = pc - module.base
            if Path(module.path).resolve() == image_path:
                address = offset + preferred
                index = bisect.bisect_right(addresses, address) - 1
                if index >= 0:
                    symbol_address, name = symbols[index]
                    return f'{module.name}!{name}+0x{address - symbol_address:x}'
            return f'{module.name}+0x{offset:x}'
    return f'unknown+0x{pc:x}'


def profile(pid, samples=20):
    if not isinstance(samples, int) or not 1 <= samples <= 400:
        raise ValueError('Sample count must be an integer from 1 to 400')
    receipt = json.loads((ROOT / 'research/windows-build.json').read_text())
    expected = (ROOT / receipt['binary']).resolve()
    if expected.parent != OUT.resolve() or expected.name != 'Frostpunk.exe':
        raise ValueError('Profiler target must be the own recompiled Windows executable')
    with expected.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != receipt['binary_sha256']:
            raise ValueError('Recompiled binary hash differs from build receipt')
    process = open_process(0x101010, False, pid)  # Query, synchronize and read only the verified own process.
    if not process:
        raise ProcessLookupError(f'Own process {pid} is no longer accessible')
    raw = {}
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = w.DWORD(len(buffer))
        if not query_path(process, 0, buffer, ctypes.byref(size)) or Path(buffer.value).resolve() != expected:
            raise ValueError('Refusing to sample a process with a different executable path')
        modules = entries(0x18, pid, ModuleEntry, module_first, module_next)
        began = time.monotonic()
        for iteration in range(samples):
            if wait(process, 0) == 0:
                break
            for thread in entries(4, 0, ThreadEntry, thread_first, thread_next):
                if thread.pid != pid:
                    continue
                result = sample(thread.tid, pid, process)
                if result is not None:
                    raw.setdefault(thread.tid, []).append(result)
            remaining = began + (iteration + 1) * 0.075 - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
        elapsed = time.monotonic() - began
    finally:
        close(process)
    preferred, symbols = parse_map((OUT / 'frostpunk-windows-x64.map').read_text(errors='replace'))
    addresses = [address for address, name in symbols]
    threads = []
    for tid, rows in raw.items():
        counters = Counter(describe(pc, modules, expected, preferred, symbols, addresses) for cpu, pc, stack in rows)
        candidates = Counter()
        for cpu, pc, stack in rows:
            for word in stack:
                label = describe(word, modules, expected, preferred, symbols, addresses)
                if not label.startswith('unknown+'):
                    candidates[label] += 1
        threads.append(dict(tid=tid, cpu_ms=(rows[-1][0] - rows[0][0]) / 10000,
                            samples=len(rows), locations=dict(counters.most_common()),
                            stack_call_candidates=dict(candidates.most_common(12)),
                            raw_rip=[hex(pc) for cpu, pc, stack in rows]))
    threads.sort(key=lambda row: row['cpu_ms'], reverse=True)
    report = dict(pid=pid, binary=str(expected), binary_sha256=receipt['binary_sha256'],
                  elapsed_seconds=elapsed, samples_per_thread_limit=samples, threads=threads,
                  method='Win64 control context; every successful SuspendThread paired with ResumeThread in finally',
                  stack_scan_is_not_an_unwind=True, process_stopped=False, gameplay_proven=False)
    target = OUT / 'profiles' / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '.json')
    target.parent.mkdir(exist_ok=True)
    target.write_text(json.dumps(report, indent=2) + '\n')
    summary = [{key: value for key, value in row.items() if key != 'raw_rip'} for row in threads[:8]]
    print(json.dumps(dict(profile=str(target), threads=summary), indent=2))


def self_test():
    from unittest.mock import patch
    for count in [0, -1, 401, 1.5]:
        try:
            profile(0, count)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid sample count accepted')
    text = 'Preferred load address is 0000000180000000\n 0001:00000000 L_000140001000 0000000180001000 f unit.obj\n 0001:00000020 dispatch 0000000180001020 f host.obj'
    preferred, symbols = parse_map(text)
    assert preferred == 0x180000000 and symbols == [(0x180001000, 'L_000140001000'), (0x180001020, 'dispatch')]
    module = ModuleEntry()
    module.base = 0x190000000
    module.bytes = 0x3000
    module.name = 'Frostpunk.exe'
    module.path = str((OUT / module.name).resolve())
    result = describe(0x190001025, [module], Path(module.path), preferred, symbols, [a for a, n in symbols])
    assert result.endswith('!dispatch+0x5'), result
    assert ctypes.sizeof(ThreadEntry) == 28 and ctypes.sizeof(ModuleEntry) == 1080
    events = []
    def fake_context(handle, aligned):
        assert aligned % 16 == 0
        assert ctypes.c_uint32.from_address(aligned + 48).value == 0x100001
        ctypes.c_uint64.from_address(aligned + 248).value = 0x180001025
        ctypes.c_uint64.from_address(aligned + 152).value = 0x123000
        return True
    def fake_read(process, address, buffer, length, got):
        assert address == 0x123000 and length == 512
        ctypes.memmove(buffer, (0x180001020).to_bytes(8, 'little'), 8)
        ctypes.cast(got, ctypes.POINTER(ctypes.c_size_t))[0] = 8
        return True
    replacements = dict(open_thread=lambda *a: 123, thread_owner=lambda h: 42,
                        thread_times=lambda *a: True, suspend=lambda h: 0,
                        resume=lambda h: events.append('resume') or 1,
                        close=lambda h: events.append('close') or True,
                        context=fake_context, read_memory=fake_read)
    with patch.dict(globals(), replacements):
        assert sample(7, 42, 456) == (0, 0x180001025, [0x180001020])
        assert events == ['resume', 'close']
        events.clear()
        with patch.dict(globals(), context=lambda *a: False):
            assert sample(7, 42, 456) is None
        assert events == ['resume', 'close']
    print('WINDOWS_PROFILER_SELF_TEST_PASS map_relocation=1 module_bounds=1 context_RIP248_RSP152=1 resume_after_context_failure=1')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--samples', type=int, default=20, help='Samples per thread, 1–400 at 75 ms intervals')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.pid and args.pid > 0:
        profile(args.pid, args.samples)
    else:
        parser.error('--pid must identify the own running recompiled game, or use --self-test')
