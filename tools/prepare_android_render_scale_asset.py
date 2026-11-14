"""Append the one native Graphics setting to a private raw common archive; never ADB."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct

from prepare_raw_archives import entries, RECORD

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/android-render-scale-asset'
KEY = 3500207806
SOURCE = {
    'common.idx': ('083801924af8c002caf072100bc46f059ca576835cc0938b4e9dbc78bf9ad704', 207651),
    'common.dat': ('7981e060d0bfe449373974d2148f9f7845053ef1bfdf97227e7dda7f5c4a833f', 1557206477),
}
ORIGINAL = 'e4d348c7af318bdcea2fe2bbfd4d5ed758885b798f7f5a5025605f09f675cad7'
REPLACEMENT = '7a3483e51f4cc1d47d30dfcdf93c1ca87c29b3ccd95ada90449320d8a003347b'
CHUNK = 1024 * 1024


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pin(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError('Expected regular archive input: ' + str(path))
    before = path.stat()
    digest = sha(path)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError('Archive input changed while hashing')
    return dict(path=str(path), bytes=before.st_size, sha256=digest, mtime_ns=before.st_mtime_ns)


def append_setting(index, data, replacement, output, expected):
    inputs = {name: pin(path) for name, path in [('common.idx', index), ('common.dat', data), ('setting', replacement)]}
    if any((inputs[name]['sha256'], inputs[name]['bytes']) != value for name, value in expected.items()):
        raise ValueError('Archive/replacement input differs from the qualified bytes')
    raw = Path(index).read_bytes()
    old_size = inputs['common.dat']['bytes']
    rows = entries(raw, old_size)
    selected = [(i, row) for i, row in enumerate(rows) if row[0] == KEY]
    if len(selected) != 1:
        raise ValueError('Expected one SettingsConfig key')
    number, old = selected[0]
    if old[1:3] != (4811, 4811) or old[4] != 0:
        raise ValueError('Expected original raw SettingsConfig geometry')
    with Path(data).open('rb') as source:
        source.seek(old[3]); original = source.read(old[1])
    if hashlib.sha256(original).hexdigest() != ORIGINAL:
        raise ValueError('Original SettingsConfig payload differs')
    extra = Path(replacement).read_bytes()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    new_data, new_index = output / 'common.dat', output / 'common.idx'
    with Path(data).open('rb') as source, new_data.open('xb') as target:
        while block := source.read(CHUNK):
            target.write(block)
        target.write(extra)
    packed = bytearray(raw)
    at = 11 + number * RECORD.size
    packed[at:at + RECORD.size] = RECORD.pack(KEY, len(extra), len(extra), old_size, 0)
    with new_index.open('xb') as stream:
        stream.write(packed)
    actual = entries(new_index.read_bytes(), new_data.stat().st_size)
    if actual[:number] != rows[:number] or actual[number + 1:] != rows[number + 1:]:
        raise ValueError('Unselected index records changed')
    restored = bytearray(packed)
    restored[at:at + RECORD.size] = raw[at:at + RECORD.size]
    if restored != raw or actual[number] != (KEY, len(extra), len(extra), old_size, 0):
        raise ValueError('Index changed outside the one selected record')
    digest = hashlib.sha256()
    with new_data.open('rb') as derived:
        remaining = old_size
        while remaining:
            block = derived.read(min(CHUNK, remaining))
            if not block:
                raise ValueError('Truncated original archive prefix')
            digest.update(block); remaining -= len(block)
        if derived.read() != extra or digest.hexdigest() != inputs['common.dat']['sha256']:
            raise ValueError('Derived archive prefix or appended setting differs')
    if any(pin(Path(item['path'])) != item for item in inputs.values()):
        raise ValueError('Input changed during preparation')
    return dict(success=True, key=KEY, entry_index=number, entries=len(rows),
                original_offset=old[3], appended_offset=old_size, original_setting_sha256=ORIGINAL,
                setting_sha256=inputs['setting']['sha256'], setting_bytes=len(extra),
                original_payload_prefix_byte_exact=True, unselected_index_records_byte_exact=True,
                unselected_entries=len(rows) - 1, original_inputs_read_only=True, device_actions=False,
                game_boot_proven=False, inputs=inputs,
                outputs={p.name: pin(p) for p in [new_index, new_data]})


def main():
    source = ROOT / 'build/raw-archives/common'
    setting = ROOT / 'build/render-scale-ui-candidate/settings-render-scale.bin'
    # Rebuild the row from its pinned original, so the file cannot authorize a different UI change.
    from test_android_render_scale import resource
    helper = ROOT / 'tools/test_android_render_scale.py'
    source_pins = {str(p): sha(p) for p in [Path(__file__), ROOT / 'tools/prepare_raw_archives.py', helper]}
    with (source / 'common.dat').open('rb') as stream:
        stream.seek(1284302108); original = stream.read(4811)
    rebuilt, _ = resource(original)
    if rebuilt != setting.read_bytes() or len(rebuilt) != 4963:
        raise ValueError('Setting is not the deterministic one-row addition')
    report = append_setting(source / 'common.idx', source / 'common.dat', setting, OUT,
                            {**SOURCE, 'setting': (REPLACEMENT, 4963)})
    if any(sha(p) != value for p, value in source_pins.items()):
        raise ValueError('Asset producer changed during preparation')
    report.update(timestamp_utc=datetime.now(timezone.utc).isoformat(), source_pins=source_pins,
                  producer=dict(path=str(Path(__file__).resolve()), sha256=sha(__file__)))
    with (OUT / 'result.json').open('x') as stream:
        stream.write(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['success', 'entries', 'unselected_entries', 'outputs']}, indent=2))


if __name__ == '__main__':
    main()
