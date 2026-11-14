"""Analyze one PID/swapchain's PresentMon v1 CSV application Present cadence."""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path


def analyze(text, pid):
    if type(pid) is not int or pid <= 0:
        raise ValueError('Target PID must be a positive integer')
    reader = csv.reader(io.StringIO(text.removeprefix('\ufeff'), newline=''), strict=True)
    header = next(reader, [])
    required = {'Application', 'ProcessID', 'SwapChainAddress', 'Runtime', 'Dropped', 'TimeInSeconds'}
    if len(set(header)) != len(header) or not required <= set(header):
        raise ValueError('Missing or duplicate PresentMon v1 CSV columns')
    times, dropped, application, swapchain = [], 0, None, None
    for number, values in enumerate(reader, 2):
        if len(values) != len(header):
            raise ValueError(f'CSV row {number}: wrong number of columns')
        row = dict(zip(header, values))
        try:
            row_pid = int(row['ProcessID'])
            chain = int(row['SwapChainAddress'], 16)
            timestamp = float(row['TimeInSeconds'])
        except ValueError as error:
            raise ValueError(f'CSV row {number}: invalid PID, swapchain or timestamp') from error
        if row_pid != pid:
            raise ValueError(f'CSV row {number}: target PID differs')
        if row['Runtime'] != 'DXGI':
            raise ValueError(f'CSV row {number}: Runtime must be DXGI')
        if not row['Application'] or not 0 < chain <= 0xffffffffffffffff:
            raise ValueError(f'CSV row {number}: invalid application or swapchain')
        if application is not None and (row['Application'] != application or chain != swapchain):
            raise ValueError(f'CSV row {number}: mixed applications or swapchains')
        if not math.isfinite(timestamp) or timestamp < 0 or (times and timestamp <= times[-1]):
            raise ValueError(f'CSV row {number}: timestamps must be finite, nonnegative and strictly increasing')
        if row['Dropped'] not in ('0', '1'):
            raise ValueError(f'CSV row {number}: Dropped must be 0 or 1')
        application, swapchain = row['Application'], chain
        times.append(timestamp)
        dropped += int(row['Dropped'])
    if len(times) < 2:
        raise ValueError('At least two frames are required')
    intervals = sorted((right - left) * 1000 for left, right in zip(times, times[1:]))
    duration = times[-1] - times[0]
    fps = len(intervals) / duration
    if not math.isfinite(fps) or not all(math.isfinite(value) and value > 0 for value in intervals):
        raise ValueError('Frame intervals or cadence exceed finite numeric range')
    # shortcut: fewer than 100 intervals cannot supply a full 1% tail; report null.
    tail_count = math.ceil(len(intervals) / 100) if len(intervals) >= 100 else 0
    tail_mean = sum(value / tail_count for value in intervals[-tail_count:]) if tail_count else None
    return dict(metric='application_Present_cadence', process_id=pid, application=application,
                swapchain_address=hex(swapchain), frames=len(times), intervals=len(intervals),
                dropped_count=dropped, first_present_s=times[0], last_present_s=times[-1],
                capture_duration_s=duration, average_fps=fps, average_frame_ms=duration / len(intervals) * 1000,
                p99_frame_ms=intervals[(99 * len(intervals) + 99) // 100 - 1],
                p99_method='nearest rank: ceil(0.99 * interval_count)', max_frame_ms=intervals[-1],
                slowest_1_percent_average_fps=1000 / tail_mean if tail_mean else None,
                slowest_1_percent_interval_count=tail_count,
                slowest_1_percent_method='1000 / mean(slowest ceil(interval_count / 100) frame intervals); requires at least 100 intervals',
                display_cadence_computed=False, rows_filtered=0, workload_equivalence_proven=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('csv', type=Path)
    args = parser.parse_args()
    try:
        raw = args.csv.read_bytes()
        report = analyze(raw.decode('utf-8-sig'), args.pid)
    except (OSError, UnicodeError, ValueError, csv.Error) as error:
        parser.exit(2, f'{error}\n')
    report.update(csv=str(args.csv.resolve()), csv_sha256=hashlib.sha256(raw).hexdigest())
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
