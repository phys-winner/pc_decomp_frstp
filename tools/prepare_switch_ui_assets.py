"""Adapt Switch UI geometry to the PC recipe schema, preserving its controllers.

Run after extracting the updated NSP RomFS to build/switch-ui-20261112/romfs.
The engine caches recipes, so templates-switch.idx is selected at process start.
Both indexes address one data file; desktop resources remain byte exact.
"""
import ctypes
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct

from inspect_mobile_layouts import NODE
from prepare_raw_archives import RECORD, entries
from prepare_ui_astc_archives import packed_key

ROOT = Path(__file__).resolve().parents[1]
SWITCH = ROOT / 'build/switch-ui-20261112/romfs'
PC = ROOT / 'build/raw-archives/templates'
OUT = ROOT / 'build/switch-ui-20261112/assets-v6'
ZSTD = Path('C:/Portable/Total.Commander/Utilites/imhex/libzstd.dll')
PINS = {'templates.idx': '0c34c5a42fe5da479f162e08784bad799ddeca4738025bae26f89445f41dfb01',
        'templates.dat': '2782d1243437370e1f576ef9e0dd167bf185eb5fe66654ed2abb7fc824d87544'}
PRESET = re.compile(rb'\x01(UI[A-Za-z0-9_]*Preset)\x00\x01([^\x00]{0,256})\x00')
RECIPE_TAG = re.compile(rb'\x01(UI[A-Za-z0-9_]*Recipe)\x00')
PRESET_TAG = re.compile(rb'\x01(UI[A-Za-z0-9_]*Preset)\x00')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def names(data):
    if len(data) < 11 or data[:3] != b'\0\5\2':
        raise ValueError('Unexpected Switch name table')
    count, = struct.unpack_from('<Q', data, 3)
    result, at = {}, 11
    for _ in range(count):
        key, size = struct.unpack_from('<II', data, at)
        at += 8
        if not size or size > len(data) - at:
            raise ValueError('Truncated Switch resource name')
        name = data[at:at + size].decode('ascii')
        at += size
        if key in result or packed_key(name) != key:
            raise ValueError('Duplicate or noncanonical Switch resource key')
        result[key] = name
    if at != len(data):
        raise ValueError('Tail in Switch name table')
    return result


def nodes(payload):
    matches = list(RECIPE_TAG.finditer(payload))
    result, stack, roots = {}, [], 0
    for number, match in enumerate(matches):
        end = matches[number + 1].start() if number + 1 < len(matches) else len(payload)
        at = match.end()
        if payload[at] == 0:
            name, at = b'', at + 1
        elif payload[at] == 1:
            stop = payload.index(0, at + 1, end)
            name, at = payload[at + 1:stop], stop + 1
        else:
            raise ValueError('Invalid UI recipe name marker')
        if at + 121 > end:
            raise ValueError('Truncated UI recipe base')
        values = struct.unpack_from('<16f', payload, at)
        valid = all(math.isfinite(v) and abs(v) <= 1e6 for v in values)
        presets, = struct.unpack_from('<I', payload, at + 117)
        if presets != len(list(PRESET_TAG.finditer(payload, at + 121, end))):
            raise ValueError('UI preset boundary differs from its declared count')
        while stack and not stack[-1][1]:
            stack.pop()
        if stack:
            parent = stack[-1][0]
            stack[-1][1] -= 1
        else:
            parent = ()
            roots += 1
        key = parent + ((match[1], name),)
        result.setdefault(key, []).append((at, end, valid))
        # A nonempty child array immediately precedes its first polymorphic recipe.
        children = struct.unpack_from('<I', payload, end - 4)[0] if number + 1 < len(matches) else 0
        if 0 < children <= 512:
            stack.append([key, children])
    while stack and not stack[-1][1]:
        stack.pop()
    if stack:
        raise ValueError('UI child tree ends before its declared children')
    if matches and matches[0].start() >= 4:
        declared_roots, = struct.unpack_from('<I', payload, matches[0].start() - 4)
        if roots != declared_roots:
            raise ValueError('UI root array count differs from the decoded hierarchy')
    return result


def adapt(original, source):
    """Only common fields of uniquely named widgets; never copy newer classes/events."""
    before, incoming = nodes(original), nodes(source)
    output = bytearray(original)
    ranges, changed_nodes = [], []
    for key, locations in before.items():
        # Law nodes use PC template cameras and controller cell metrics; migrate those together.
        if any(name == b'SOCIOTECHTREE' for _, name in key) and any(name == b'TREE_PANEL' for _, name in key):
            continue
        other = incoming.get(key, [])
        if len(locations) != 1 or len(other) != 1 or any(not name for _, name in key):
            continue
        (at, end, valid), (src, src_end, source_valid) = locations[0], other[0]
        # Some animated decorations have authored NaN positions; retain their PC recipe.
        if not valid or not source_valid:
            continue
        # Geometry, docking, color and anchor fields share their wire schema.
        # Preserve the five input/visibility flags, child arrays and all callbacks.
        copies = [(at, src, 112)]
        left, right = {}, {}
        for payload, start, stop, target in ((original, at + 121, end, left),
                                             (source, src + 121, src_end, right)):
            for preset in PRESET.finditer(payload, start, stop):
                start = preset.end() + 1
                if start + 48 > stop or payload[preset.end()] not in (0, 1):
                    raise ValueError('Truncated UI preset transform')
                if all(math.isfinite(v) and abs(v) <= 1e6 for v in struct.unpack_from('<12f', payload, start)):
                    target.setdefault((preset[1], preset[2]), []).append(start)
        for preset, positions in left.items():
            counterparts = right.get(preset, [])
            if len(positions) == len(counterparts) == 1:
                copies.append((positions[0], counterparts[0], 48))
        changed = False
        for dest, origin, size in copies:
            block = source[origin:origin + size]
            if output[dest:dest + size] != block:
                output[dest:dest + size] = block
                ranges.append((dest, size))
                changed = True
        if changed:
            changed_nodes.append('/'.join(kind.decode() + ':' + name.decode('utf-8') for kind, name in key))
    restored = bytearray(output)
    for at, size in ranges:
        restored[at:at + size] = original[at:at + size]
    if restored != original or len(output) != len(original):
        raise ValueError('UI adaptation changed nonselected bytes')
    return bytes(output), changed_nodes, ranges


def main():
    if any(sha(SWITCH / name) != digest for name, digest in PINS.items()):
        raise ValueError('Updated Switch template archives differ from the inspected NSP')
    lib = ctypes.CDLL(str(ZSTD))
    lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    lib.ZSTD_decompress.restype = ctypes.c_size_t
    table = names((SWITCH / 'templates.str').read_bytes())
    raw = (SWITCH / 'templates.idx').read_bytes()
    # Its record geometry is unchanged; only its compression/version header differs.
    switch_rows = entries(b'\0\2\1' + raw[3:], (SWITCH / 'templates.dat').stat().st_size)
    # shortcut: PC menus, city root and notification controller share fixed cameras/coordinates;
    # migrate those controllers together before replacing their layouts.
    selected = {r[0]: r for r in switch_rows if table[r[0]].startswith('ui/')
                and not table[r[0]].startswith('ui/menu/')
                and table[r[0]] not in ('ui/mainscreen.binarytemplate', 'ui/panels/general/notificationpanel.binarytemplate')}
    pc_index = (PC / 'templates.idx').read_bytes()
    pc_rows = entries(pc_index, (PC / 'templates.dat').stat().st_size)
    report, payloads, skipped = [], [], []
    with (PC / 'templates.dat').open('rb') as pc, (SWITCH / 'templates.dat').open('rb') as switch:
        for number, row in enumerate(pc_rows):
            if row[0] not in selected:
                continue
            key, stored, decoded, offset, compressed = selected[row[0]]
            if not compressed or not 0 < decoded <= 64 * 1024 * 1024:
                raise ValueError('Unexpected Switch recipe encoding/size')
            switch.seek(offset)
            packed = switch.read(stored)
            buffer = ctypes.create_string_buffer(decoded)
            if lib.ZSTD_decompress(buffer, decoded, packed, len(packed)) != decoded:
                raise ValueError('Switch recipe decompression failed')
            if row[4]:
                raise ValueError('Expected verified raw PC templates')
            pc.seek(row[3])
            original = pc.read(row[1])
            try:
                changed, widgets, ranges = adapt(original, buffer.raw)
            except ValueError as error:
                skipped.append(dict(key=key, path=table[key], reason=str(error)))
                continue
            if widgets:
                payloads.append((number, key, changed))
                report.append(dict(key=key, path=table[key], widgets=widgets, ranges=ranges,
                                   original_sha256=hashlib.sha256(original).hexdigest(),
                                   adapted_sha256=hashlib.sha256(changed).hexdigest()))
    OUT.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(PC / 'templates.dat', OUT / 'templates.dat')
    (OUT / 'templates.idx').write_bytes(pc_index)
    alternate = bytearray(pc_index)
    with (OUT / 'templates.dat').open('ab') as stream:
        for number, key, payload in payloads:
            offset = stream.tell()
            stream.write(payload)
            RECORD.pack_into(alternate, 11 + number * RECORD.size, key, len(payload), len(payload), offset, 0)
    (OUT / 'templates-switch.idx').write_bytes(alternate)
    actual = entries(alternate, (OUT / 'templates.dat').stat().st_size)
    changed_keys = {key for _, key, _ in payloads}
    if any(old != new for old, new in zip(pc_rows, actual, strict=True) if old[0] not in changed_keys):
        raise ValueError('Nonselected archive record changed')
    result = dict(success=True, producer_sha256=sha(__file__), switch_inputs=PINS,
                  source='Switch v65536, title 0100B1C0249B4000',
                  original_schema_preserved=True, original_index_byte_exact=True, parent_paths_verified=True,
                  skipped_schemas=skipped,
                  resources=len(report), widgets=sum(len(r['widgets']) for r in report), changes=report,
                  outputs={name: dict(bytes=(OUT / name).stat().st_size, sha256=sha(OUT / name))
                           for name in ('templates.dat', 'templates.idx', 'templates-switch.idx')})
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('success', 'resources', 'widgets', 'outputs')}, indent=2))


if __name__ == '__main__':
    main()
