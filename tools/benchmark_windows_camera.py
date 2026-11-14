"""Measure an already running, verified Frostpunk with external PresentMon.

Each group has its own complete CSV; before/after window photos are outside it.
This script never launches or terminates the game and does not prove gameplay.
"""
import argparse
import ctypes as C
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'build/windows'))
import camera_owned as camera
import drive_owned as D
import run_windows
from analyze_windows_frames import analyze

PRESENTMON = ROOT / 'build/windows-benchmark-tools/PresentMon-2.6.0-x64.exe'
PRESENTMON_SHA256 = 'b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af'


class ProcessEntry(C.Structure):
    _fields_ = [('size', D.W.DWORD), ('usage', D.W.DWORD), ('pid', D.W.DWORD),
                ('heap', C.c_size_t), ('module', D.W.DWORD), ('threads', D.W.DWORD),
                ('parent', D.W.DWORD), ('priority', D.W.LONG), ('flags', D.W.DWORD),
                ('name', D.W.WCHAR * 260)]


snapshot_processes = D.api(D.K, 'CreateToolhelp32Snapshot', D.W.HANDLE, D.W.DWORD, D.W.DWORD)
process_first = D.api(D.K, 'Process32FirstW', D.W.BOOL, D.W.HANDLE, C.POINTER(ProcessEntry))
process_next = D.api(D.K, 'Process32NextW', D.W.BOOL, D.W.HANDLE, C.POINTER(ProcessEntry))


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stamp():
    return {'utc': datetime.now(timezone.utc).isoformat(), 'monotonic_s': time.monotonic()}


def primary_display_mode():
    # DEVMODEW display fields; this observation never changes the display mode.
    data = C.create_string_buffer(220)
    struct.pack_into('<H', data, 68, len(data))
    query = D.api(D.U, 'EnumDisplaySettingsW', D.W.BOOL, D.W.LPCWSTR, D.W.DWORD, C.c_void_p)
    if not query(None, 0xffffffff, data):
        raise C.WinError(C.get_last_error())
    return dict(width=struct.unpack_from('<I', data, 172)[0],
                height=struct.unpack_from('<I', data, 176)[0],
                reported_hz=struct.unpack_from('<I', data, 184)[0])


def require_exclusive(pid, processes):
    if type(pid) is not int or not 1 <= pid <= 0xffffffff or processes != [pid]:
        raise ValueError(f'Exactly one Frostpunk.exe must exist, with PID {pid}; found {processes}')


def frostpunk_pids():
    handle = snapshot_processes(2, 0)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    found = []
    try:
        row = ProcessEntry();row.size = C.sizeof(row)
        available = process_first(handle, C.byref(row))
        if not available:
            raise C.WinError(C.get_last_error())
        while available:
            if row.name.casefold() == 'frostpunk.exe':
                found.append(int(row.pid))
            available = process_next(handle, C.byref(row))
        if C.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise C.WinError(C.get_last_error())
    finally:
        D.close(handle)
    return sorted(found)


def groups():
    planned = [('stationary', [], 20)]
    for name, actions in camera.plan():
        if not name or any(character not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_' for character in name):
            raise ValueError('Unsafe camera group name')
        for action in actions:
            D.validate(action)
        seconds = sum(action.get('seconds', 0) for action in actions) + max(0, len(actions) - 1) * 0.5
        planned.append((name, actions, max(8, math.ceil(seconds + 2))))
    if len({name for name, _, _ in planned}) != len(planned):
        raise ValueError('Duplicate camera group name')
    return planned


def collector_command(pid, seconds, csv_path, session):
    return [str(PRESENTMON), '--process_id', str(pid), '--output_file', str(csv_path),
            '--session_name', session, '--timed', str(seconds), '--terminate_after_timed',
            '--terminate_on_proc_exit', '--v1_metrics', '--no_track_gpu', '--no_track_input',
            '--no_console_stats']


def require_recording(collector, deadline):
    if collector.poll() is not None or time.monotonic() >= deadline:
        raise RuntimeError('PresentMon stopped or its recording deadline passed before camera input completed')


def require_coverage(statistics, seconds):
    if statistics['capture_duration_s'] < seconds - 0.5:
        raise ValueError('CSV Present span is shorter than nominal recording duration minus 0.5 seconds')
    if statistics['first_present_s'] > 0.5 or statistics['last_present_s'] < seconds - 0.5:
        raise ValueError('CSV Present timestamps do not cover the nominal recording boundaries')


def capture(pid, path):
    result = {'started': stamp(), 'path': str(path)}
    box = {}
    def worker():
        previous = None
        try:
            previous = D.dpi_thread(C.c_void_p(-4))
            if not previous:
                raise C.WinError(C.get_last_error())
            hwnd = D.choose_window(pid)
            box['capture'] = run_windows.capture_window(hwnd, pid, path, flags=3)
        except BaseException as error:
            box['error'] = error
        finally:
            if previous:
                D.dpi_thread(C.c_void_p(previous))
    worker_thread = threading.Thread(target=worker, daemon=True)
    worker_thread.start();worker_thread.join(30)
    if worker_thread.is_alive():
        raise TimeoutError('Owned window capture did not finish in 30 seconds')
    if 'error' in box:
        raise box['error']
    result.update(finished=stamp(), capture=box['capture'], sha256=sha(path), gameplay_proven=False)
    return result


def run(pid, original, output):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {'success': False, 'pid': pid, 'original': original, 'started': stamp(),
              'output': str(output), 'groups': [], 'game_launched': False, 'game_terminated': False,
              'gameplay_proven': False, 'workload_equivalence_proven': False,
              'metric': 'application Present cadence, whole per-group CSV; path/hash/focus preparation outside capture',
              'photos_outside_collectors': True, 'collector_warmup_before_actions_s': 1}
    input_session = None
    try:
        config = json.loads((ROOT / 'port.json').read_text())
        receipt_path = ROOT / ('port.json' if original else 'research/windows-build.json')
        if original:
            expected = (Path(config['game_directory']) / 'Frostpunk.exe').resolve()
            expected_hash = config['original_exe_sha256']
        else:
            receipt = json.loads(receipt_path.read_text())
            expected = D.checked_path(receipt)
            expected_hash = receipt['binary_sha256']
        require_exclusive(pid, frostpunk_pids())
        if sha(expected) != expected_hash or sha(PRESENTMON) != PRESENTMON_SHA256:
            raise ValueError('Game or pinned PresentMon executable hash differs')
        input_session = D.VerifiedInput(pid, original)
        handle = input_session.handle
        created = camera.creation_ns(handle)
        source_paths = [Path(__file__).resolve(), Path(D.__file__).resolve(), Path(camera.__file__).resolve(),
                        Path(run_windows.__file__).resolve(), ROOT / 'tools/analyze_windows_frames.py',
                        ROOT / 'port.json', receipt_path,
                        ROOT / 'build/windows-benchmark-tools/provenance.json']
        pins = {str(path): sha(path) for path in source_paths}
        report.update(binary=str(expected), binary_sha256=expected_hash, process_created_ns=created,
                      presentmon=str(PRESENTMON), presentmon_sha256=PRESENTMON_SHA256, source_sha256=pins)

        def verify():
            require_exclusive(pid, frostpunk_pids())
            if D.wait(handle, 0) != 258 or camera.creation_ns(handle) != created:
                raise ProcessLookupError('Verified game exited or its identity changed')
            checked = D.process_handle(pid, expected);D.close(checked)
            if sha(expected) != expected_hash or sha(PRESENTMON) != PRESENTMON_SHA256:
                raise ValueError('Pinned executable changed')
            if any(sha(Path(path)) != digest for path, digest in pins.items()):
                raise ValueError('Benchmark source or receipt changed')

        for index, (name, actions, seconds) in enumerate(groups()):
            verify()
            folder = output / f'{index:02d}_{name}';folder.mkdir()
            row = {'name': name, 'seconds': seconds, 'actions': [], 'success': False}
            report['groups'].append(row)
            focus_started = stamp()
            input_session.prepare()
            row['focus'] = {'started': focus_started, 'finished': stamp(), 'pid': pid,
                            'hwnd': input_session.hwnd, 'success': True}
            row['before_capture'] = capture(pid, folder / 'before.png')
            row['primary_display_before'] = primary_display_mode()
            verify()
            session = 'FrostpunkCamera-' + uuid.uuid4().hex
            csv_path = folder / 'presents.csv'
            command = collector_command(pid, seconds, csv_path, session)
            row.update(session=session, command=command, collector_started=stamp())
            deadline = row['collector_started']['monotonic_s'] + seconds
            row['actions_deadline_monotonic_s'] = deadline
            collector = None
            with (folder / 'collector.log').open('xb') as log:
                try:
                    collector = subprocess.Popen(command, stdout=log, stderr=log,
                                                 creationflags=subprocess.CREATE_NO_WINDOW)
                    row['collector_pid'] = collector.pid
                    time.sleep(1)
                    if collector.poll() is not None:
                        raise RuntimeError('PresentMon exited before recording; inspect collector.log')
                    row['actions_started'] = stamp()
                    for action_index, action in enumerate(actions):
                        require_recording(collector, deadline)
                        event = {'action': dict(action), 'started': stamp()};row['actions'].append(event)
                        event['result'] = D.drive_verified(input_session, dict(action))
                        event['finished'] = stamp()
                        if not event['result'].get('success'):
                            raise RuntimeError('Owned input failed: ' + json.dumps(event['result']))
                        require_recording(collector, deadline)
                        if action_index + 1 < len(actions):
                            time.sleep(0.5)
                    row['actions_finished'] = stamp()
                    if collector.wait(timeout=seconds + 10) != 0:
                        raise RuntimeError('PresentMon failed; inspect collector.log')
                finally:
                    if collector is not None:
                        if collector.poll() is None:
                            # Let this timed collector flush and close its own ETW session on failure.
                            try:
                                collector.wait(timeout=seconds + 10)
                            except subprocess.TimeoutExpired:
                                collector.terminate();collector.wait(timeout=5)
                                cleanup = [str(PRESENTMON), '--session_name', session, '--terminate_existing_session']
                                row['forced_collector_cleanup'] = cleanup
                                cleanup_result = subprocess.run(cleanup, stdout=log, stderr=log, timeout=10,
                                                                creationflags=subprocess.CREATE_NO_WINDOW)
                                row['forced_collector_cleanup_returncode'] = cleanup_result.returncode
                        row.update(collector_finished=stamp(), collector_returncode=collector.returncode)
            verify()
            row['primary_display_after'] = primary_display_mode()
            row['after_capture'] = capture(pid, folder / 'after.png')
            raw = csv_path.read_bytes()
            row['statistics'] = analyze(raw.decode('utf-8-sig'), pid)
            row['minimum_present_span_s'] = seconds - 0.5
            require_coverage(row['statistics'], seconds)
            row.update(csv=str(csv_path), csv_sha256=hashlib.sha256(raw).hexdigest(),
                       collector_log_sha256=sha(folder / 'collector.log'), success=True)
            with (folder / 'result.json').open('x', encoding='utf-8') as stream:
                json.dump(row, stream, indent=2, allow_nan=False);stream.write('\n')
            print(json.dumps({'group': name, 'statistics': row['statistics']}), flush=True)
        report['success'] = True
    except Exception as error:
        report['error'] = f'{type(error).__name__}: {error}'
    finally:
        if input_session is not None:
            input_session.close()
        report['finished'] = stamp()
        with (output / 'result.json').open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, allow_nan=False);stream.write('\n')
    return report


def self_check():
    from tempfile import TemporaryDirectory
    from unittest.mock import patch
    prohibited = AssertionError('Self-check must not use foreground, input or capture')
    with patch.object(D, 'drive', side_effect=prohibited), patch.object(D, 'drive_verified', side_effect=prohibited), patch.object(run_windows, 'capture_window', side_effect=prohibited):
        assert C.sizeof(ProcessEntry) == 568
        planned = groups()
        assert len(planned) == 11 and planned[0] == ('stationary', [], 20)
        assert all(seconds == 8 for _, _, seconds in planned[1:])
        with patch.object(camera, 'plan', return_value=[('../escape', [])]):
            try:
                groups()
            except ValueError:
                pass
            else:
                raise AssertionError('Unsafe group filename accepted')
        require_exclusive(42, [42])
        for pids in ([], [41], [41, 42], [42, 42]):
            try:
                require_exclusive(42, pids)
            except ValueError:
                pass
            else:
                raise AssertionError('Mixed or missing game accepted')
        with TemporaryDirectory() as folder:
            existing = Path(folder)
            try:
                run(42, False, existing)
            except FileExistsError:
                pass
            else:
                raise AssertionError('Existing output directory accepted')
        command = collector_command(42, 8, Path('new-group/presents.csv'), 'FrostpunkCamera-test')
        assert command[command.index('--process_id') + 1] == '42'
        assert '--v1_metrics' in command and '--exclude_dropped' not in command
        assert '--stop_existing_session' not in command and '--terminate_existing_session' not in command
        require_coverage({'capture_duration_s': 7.8, 'first_present_s': 0.1, 'last_present_s': 7.9}, 8)
        for short in ({'capture_duration_s': 0.02, 'first_present_s': 0.1, 'last_present_s': 0.12},
                      {'capture_duration_s': 7.8, 'first_present_s': 1, 'last_present_s': 8.8}):
            try:
                require_coverage(short, 8)
            except ValueError:
                pass
            else:
                raise AssertionError('Partial or shifted CSV capture accepted')
        from unittest.mock import Mock
        collector = Mock();collector.poll.return_value = None
        with patch.object(time, 'monotonic', return_value=9):
            require_recording(collector, 10)
            try:
                require_recording(collector, 9)
            except RuntimeError:
                pass
            else:
                raise AssertionError('Action after recording deadline accepted')
    print(json.dumps({'success': True, 'groups': 11, 'stationary_seconds': 20, 'camera_group_seconds': 8,
                      'exclusive_pid_checked': True, 'existing_output_rejected': True,
                      'input_sent': False, 'capture_started': False, 'game_launched': False}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--original', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    if args.self_check:
        self_check();return
    if not args.pid or args.output is None:
        parser.error('--pid and a new --output directory are required')
    report = run(args.pid, args.original, args.output)
    print(json.dumps({'success': report['success'], 'output': report['output'], 'error': report.get('error')}))
    raise SystemExit(0 if report['success'] else 1)


if __name__ == '__main__':
    main()
