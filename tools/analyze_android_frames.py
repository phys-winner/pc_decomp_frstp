"""Offline analysis of accepted unique DXVK queue presents, not display delivery."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys

SECOND_NS = 1_000_000_000
METRIC = 'accepted_unique_vkQueuePresent'
NUMERIC = ('generation', 'start_ns', 'end_ns', 'duration_ns', 'count',
           'max_interval_ns', 'idle_ns', 'cumulative', 'first_frame_id',
           'last_frame_id', 'complete', 'compositor_delivery_proven')
MARKER = re.compile(r'(?:^|\s)(PRESENT_SAMPLE|PRESENT_PROFILE_INVALID)(?:\s|$)')


def positive_integer(value, label):
    if type(value) is not int or value <= 0:
        raise ValueError(label+' must be a positive integer device CLOCK_MONOTONIC timestamp')
    return value


def camera_segments(document, log_sha256):
    if not isinstance(document, dict):
        raise ValueError('Camera segments document must be a JSON object')
    clock = document.get('clock')
    if isinstance(clock, dict):
        clock = clock.get('clock_id')
    if clock != 'CLOCK_MONOTONIC':
        raise ValueError('Camera timestamps must use device CLOCK_MONOTONIC')
    if 'segments' in document and 'groups' in document:
        raise ValueError('Ambiguous segments/groups arrays')
    rows = document.get('groups', document.get('segments'))
    if not isinstance(rows, list) or not rows:
        raise ValueError('Camera groups/segments array is missing or empty')
    native_log = document.get('native_log')
    if native_log is not None:
        if not isinstance(native_log, dict) or native_log.get('sha256') != log_sha256:
            raise ValueError('Camera native_log SHA-256 does not match the analyzed log')
    result, names = [], set()
    for i, row in enumerate(rows):
        if not isinstance(row, dict):
            raise ValueError('Invalid camera segment '+str(i))
        name = row.get('name')
        if not isinstance(name, str) or not name.strip() or name in names:
            raise ValueError('Camera segment names must be nonempty and unique')
        names.add(name)
        start = positive_integer(row.get('start_ns'), name+'.start_ns')
        end = positive_integer(row.get('end_ns'), name+'.end_ns')
        if end <= start:
            raise ValueError(name+': end_ns must be after start_ns')
        result.append(dict(name=name, start_ns=start, end_ns=end))
    ordered = sorted(result, key=lambda s: s['start_ns'])
    if any(a['end_ns'] > b['start_ns'] for a, b in zip(ordered, ordered[1:])):
        raise ValueError('Camera segments overlap')
    return result


def parse_samples(text):
    lines = text.splitlines()
    # NativeActivity appends native.log across launches. Do not mix an earlier
    # process's generation counters with the latest APK session.
    starts = [i for i, line in enumerate(lines) if 'APK_NATIVE_MAIN ' in line]
    session = starts[-1] if starts else 0
    profiles = {}
    for index in range(session, len(lines)):
        line = lines[index]
        marker = MARKER.search(line)
        if not marker:
            continue
        if marker.group(1) == 'PRESENT_PROFILE_INVALID':
            raise ValueError('Invalid presenter profile at log line '+str(index+1)+': '+line.strip())
        fields = {}
        for word in line[marker.end():].split():
            key, separator, value = word.partition('=')
            if not separator or not key or key in fields:
                raise ValueError('Malformed/duplicate sample field at log line '+str(index+1))
            fields[key] = value
        if fields.get('method') != METRIC:
            raise ValueError('Unsupported sample method at log line '+str(index+1))
        for key in NUMERIC:
            value = fields.get(key, '')
            if not re.fullmatch(r'[0-9]+', value):
                raise ValueError('Invalid '+key+' at log line '+str(index+1))
            fields[key] = int(value)
            if fields[key] > 0xffffffffffffffff:
                raise ValueError('Out-of-range '+key+' at log line '+str(index+1))
        pointer = fields.get('presenter', '')
        if not re.fullmatch(r'(?:0x)?[0-9a-fA-F]+', pointer) or int(pointer, 16) == 0:
            raise ValueError('Invalid presenter identity at log line '+str(index+1))
        fields['presenter'] = '0x'+format(int(pointer, 16), 'x')
        fields['log_line'] = index+1
        start, end, duration = (fields[k] for k in ('start_ns', 'end_ns', 'duration_ns'))
        if not start or end < start or end-start != duration:
            raise ValueError('Invalid sample duration at log line '+str(index+1))
        if fields['complete'] not in (0, 1) or fields['compositor_delivery_proven'] != 0:
            raise ValueError('Invalid sample scope at log line '+str(index+1))
        if ((fields['complete'] and duration != SECOND_NS)
                or (not fields['complete'] and duration >= SECOND_NS)):
            raise ValueError('Invalid full/partial bin length at log line '+str(index+1))
        count, first, last = (fields[k] for k in ('count', 'first_frame_id', 'last_frame_id'))
        if (not fields['generation'] or fields['cumulative'] < count
                or (count == 0 and (first or last))
                or (count and (not first or last < first or last-first+1 < count))):
            raise ValueError('Invalid sample counts/unique IDs at log line '+str(index+1))
        key = fields['presenter'], fields['generation']
        profiles.setdefault(key, []).append(fields)
    if not profiles:
        raise ValueError('No accepted unique Present samples in latest APK session')
    for key, rows in profiles.items():
        validate_profile(key, rows)
    return profiles, session+1


def validate_profile(key, rows):
    seen, previous, previous_full = set(), None, None
    last_full_success_id = 0
    for row in rows:
        bounds = row['start_ns'], row['end_ns']
        if bounds in seen:
            raise ValueError('Duplicate bin for presenter '+str(key)+': '+str(bounds))
        seen.add(bounds)
        if previous:
            if (row['start_ns'] < previous['start_ns'] or row['end_ns'] < previous['end_ns']
                    or row['cumulative'] < previous['cumulative']):
                raise ValueError('Regressing profile samples for presenter '+str(key))
            if row['start_ns'] == previous['start_ns']:
                # A snapshot may precede the eventual full bin, but may not
                # lose already counted frames while extending the same bin.
                if previous['complete'] or row['count'] < previous['count']:
                    raise ValueError('Inconsistent overlapping snapshots for presenter '+str(key))
            elif row['start_ns'] != previous['start_ns']+SECOND_NS:
                raise ValueError('Missing bin/gap for presenter '+str(key))
            elif not previous['complete']:
                raise ValueError('Missing end of partial bin/gap for presenter '+str(key))
        if previous_full and row['start_ns'] == previous_full['end_ns']:
            if row['cumulative'] != previous_full['cumulative']+row['count']:
                raise ValueError('Inconsistent cumulative count for presenter '+str(key))
            if row['count'] and row['first_frame_id'] <= last_full_success_id:
                raise ValueError('Nonunique/regressing frame IDs for presenter '+str(key))
        if row['complete']:
            if previous_full:
                if row['start_ns'] != previous_full['end_ns']:
                    raise ValueError('Missing full bin/gap for presenter '+str(key))
            if row['count']:
                last_full_success_id = row['last_frame_id']
            previous_full = row
        previous = row


def coverage(rows, start, end):
    intervals = sorted((r['start_ns'], r['end_ns']) for r in rows if r['end_ns'] > start and r['start_ns'] < end)
    position = start
    for left, right in intervals:
        if left > position:
            return False
        position = max(position, right)
        if position >= end:
            return True
    return False


def analyze(text, document, log_sha256=None):
    if log_sha256 is None:
        log_sha256 = hashlib.sha256(text.encode('utf-8')).hexdigest()
    segments = camera_segments(document, log_sha256)
    profiles, session_line = parse_samples(text)
    groups = []
    for segment in segments:
        start, end = segment['start_ns'], segment['end_ns']
        candidates = [(key, rows) for key, rows in profiles.items() if coverage(rows, start, end)]
        if not candidates:
            raise ValueError(segment['name']+': missing continuous coverage, including live tail through end_ns')
        if len(candidates) != 1:
            raise ValueError(segment['name']+': multiple presenter generations cover the segment; cannot aggregate chains')
        key, rows = candidates[0]
        bins = [r for r in rows if r['complete'] and r['start_ns'] >= start and r['end_ns'] <= end]
        if not bins:
            raise ValueError(segment['name']+': no complete 1s bin fully inside camera segment')
        counts = [r['count'] for r in bins]
        groups.append(dict(**segment, presenter=key[0], generation=key[1],
            observation_coverage_verified=True, selected_start_ns=bins[0]['start_ns'],
            selected_end_ns=bins[-1]['end_ns'], selected_duration_ns=len(bins)*SECOND_NS,
            trim_start_ns=bins[0]['start_ns']-start, trim_end_ns=end-bins[-1]['end_ns'],
            complete_1s_bins=len(bins), count=sum(counts),
            rate=dict(min=min(counts), median=statistics.median(counts), mean=statistics.mean(counts), max=max(counts)),
            max_interval_ns=max(r['max_interval_ns'] for r in bins),
            max_idle_ns=max(r['idle_ns'] for r in bins), bins=bins))
    counts = [row['count'] for group in groups for row in group['bins']]
    return dict(success=True, schema_version=1, metric=METRIC,
        rate_unit='accepted unique queue presents per second', clock='CLOCK_MONOTONIC',
        compositor_delivery_proven=False, game_execution_proven=False, gameplay_evaluated=False,
        goal_evaluated=False, scope='Offline submission cadence only; camera/gameplay and compositor evidence are separate',
        max_interval_scope='Adjacent accepted-success gap ending in selected bins; idle_ns exposes unfinished gaps',
        session_start_log_line=session_line, groups=groups,
        aggregate=dict(complete_1s_bins=len(counts), count=sum(counts),
            min=min(counts), median=statistics.median(counts), mean=statistics.mean(counts), max=max(counts),
            max_interval_ns=max(g['max_interval_ns'] for g in groups),
            max_idle_ns=max(g['max_idle_ns'] for g in groups)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', required=True, type=Path)
    parser.add_argument('--segments', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        log = args.log.read_bytes()
        segments = args.segments.read_bytes()
        report = analyze(log.decode('utf-8', errors='strict'), json.loads(segments), hashlib.sha256(log).hexdigest())
        report['inputs'] = dict(native_log=dict(path=str(args.log.resolve()), bytes=len(log), sha256=hashlib.sha256(log).hexdigest()),
            camera_segments=dict(path=str(args.segments.resolve()), sha256=hashlib.sha256(segments).hexdigest()),
            analyzer=dict(path=str(Path(__file__).resolve()), sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()))
    except (OSError, ValueError, TypeError) as error:
        report = dict(success=False, metric=METRIC, compositor_delivery_proven=False,
            game_execution_proven=False, gameplay_evaluated=False, goal_evaluated=False, errors=[str(error)])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('groups', 'inputs')}, indent=2))
    return 0 if report['success'] else 2


if __name__ == '__main__':
    sys.exit(main())
