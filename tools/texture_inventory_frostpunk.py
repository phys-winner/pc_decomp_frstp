"""Inventory of the texture formats in Frostpunk's LiquidEngine archives.

    python -I -X utf8 tools/texture_inventory_frostpunk.py [GAME_DIR] [--out FILE.json] [--no-alpha]

Reads every *.idx/*.dat pair of the game folder (port.json game_directory by default).  A texture
entry is a 144-byte header (width, height or 0xFFFFFFFF for a cube, D3DFORMAT, mip count, 16 rows of
u16 width, u16 height, u32 bytes, smallest mip first) followed by the mips.  textures-s3 also holds
"TextureArrayDescriptor" XML entries (the "Arra" entries of the launcher audit): they are not binary
containers, they name the textures the engine stacks into one Texture2DArray, so every element of
one array must keep the same converted format.  With alpha analysis the tool decodes each DXT entry
and reports whether its alpha channel is used (needs numpy).
"""
import argparse
import collections
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from liquid_archive import Archive, packed_key  # noqa: E402

D3DFMT = {21: 'A8R8G8B8', 22: 'X8R8G8B8', 28: 'A8', 50: 'L8', 111: 'R16F', 113: 'A16B16G16R16F', 114: 'R32F',
          0x31545844: 'DXT1', 0x32545844: 'DXT2', 0x33545844: 'DXT3', 0x34545844: 'DXT4', 0x35545844: 'DXT5',
          0x31495441: 'ATI1/BC4', 0x32495441: 'ATI2/BC5', 0x55344342: 'BC4U', 0x55354342: 'BC5U', 0x30315844: 'DX10',
          0x36545341: 'ASTC6 (converted)', 0x34545341: 'ASTC4 (converted)', 0x32435445: 'ETC2 RGB (converted)',
          0x41435445: 'ETC2 RGBA (converted)'}
BLOCK_BYTES = {0x31545844: 8, 0x33545844: 16, 0x35545844: 16}
PIXEL_BYTES = {21: 4, 22: 4, 28: 1, 50: 1, 111: 2, 113: 8, 114: 4}


def parse_header(data, decoded):
    """Returns a dict for a texture entry or None."""
    if len(data) < 144:
        return None
    width, height, fmt, count = struct.unpack_from('<IIII', data)
    faces = 6 if height == 0xffffffff else 1
    true_height = width if faces == 6 else height
    if not (0 < width <= 16384 and 0 < true_height <= 16384 and 0 < count <= 16):
        return None
    if fmt not in BLOCK_BYTES and fmt not in PIXEL_BYTES:
        return None
    total, mips = 144, []
    for n in range(count):
        w, h, size = struct.unpack_from('<HHI', data, 16 + 8 * n)
        shift = count - 1 - n
        if w != max(1, width >> shift) or h != max(1, true_height >> shift):
            return None
        want = ((w + 3) // 4) * ((h + 3) // 4) * BLOCK_BYTES[fmt] if fmt in BLOCK_BYTES else w * h * PIXEL_BYTES[fmt]
        if size != want:
            return None
        mips.append((w, h, size))
        total += size * faces
    if total != decoded:
        return None
    return dict(width=width, height=true_height, format=fmt, mips=mips, faces=faces,
                full_chain=(max(width, true_height) >> (count - 1)) == 1)


def alpha_use(fmt, payload, np):
    """'opaque', 'punch-through' (DXT1 only) or 'alpha' for the block data of all mips."""
    if fmt == 0x31545844:
        blocks = np.frombuffer(payload, dtype='<u2').reshape(-1, 4)
        three = blocks[:, 0] <= blocks[:, 1]
        if not three.any():
            return 'opaque'
        idx = np.frombuffer(payload, dtype='<u4').reshape(-1, 2)[:, 1][three]
        used = ((idx & (idx >> 1)) & 0x55555555) != 0
        return 'punch-through' if used.any() else 'opaque'
    blocks = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 16)
    if fmt == 0x33545844:
        return 'opaque' if (blocks[:, :8] == 255).all() else 'alpha'
    a0, a1 = blocks[:, 0], blocks[:, 1]
    plain = (a0 == 255) & (a1 == 255)
    if plain.all():
        return 'opaque'
    rest = blocks[~plain]
    # Not every block is 255/255: decode the 3-bit indices of the rest exactly.
    bits = np.zeros(len(rest), dtype=np.uint64)
    for i in range(6):
        bits |= rest[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    r0, r1 = rest[:, 0].astype(np.int32), rest[:, 1].astype(np.int32)
    for texel in range(16):
        code = ((bits >> np.uint64(3 * texel)) & np.uint64(7)).astype(np.int32)
        eight = r0 > r1
        value = np.where(code == 0, r0, np.where(code == 1, r1,
                 np.where(eight, ((8 - code) * r0 + (code - 1) * r1) // 7,
                          np.where(code == 6, 0, np.where(code == 7, 255, ((6 - code) * r0 + (code - 1) * r1) // 5)))))
        if (value != 255).any():
            return 'alpha'
    return 'opaque'


def scan(game, alpha=True):
    np = None
    if alpha:
        import numpy as np
    report = dict(game=str(game), archives={}, unread=[])
    for index in sorted(game.glob('*.idx')):
        try:
            archive = Archive(index.with_suffix(''))
        except Exception as failure:  # noqa: BLE001 - listed, not hidden
            report['unread'].append(dict(archive=index.name, reason=str(failure)))
            continue
        formats = collections.Counter()
        details = collections.defaultdict(lambda: collections.Counter())
        other = collections.Counter()
        entries, arrays = {}, {}
        with index.with_suffix('.dat').open('rb') as stream:
            for key, (stored, decoded, offset, flag) in archive.rows.items():
                stream.seek(offset)
                packed = stream.read(stored)
                try:
                    data = zlib.decompressobj(31).decompress(packed) if flag else packed
                except zlib.error:
                    other['undecodable'] += 1
                    continue
                if data.startswith(b'<TextureArrayDescriptor>'):
                    arrays[key] = re.findall(rb'Name="Texture file name" Value="([^"]*)"', data)
                    continue
                texture = parse_header(data, decoded)
                if texture is None:
                    kind = 'DDS file' if data[:4] == b'DDS ' else 'PNG file' if data[:4] == b'\x89PNG' else \
                        'TGA file' if index.stem.startswith('textures') and data[:3] == b'\x00\x00\x02' else 'not a texture'
                    other[kind] += 1
                    continue
                name = D3DFMT.get(texture['format'], hex(texture['format']))
                formats[name] += 1
                d = details[name]
                d['cube'] += texture['faces'] == 6
                d['mipmapped'] += len(texture['mips']) > 1
                d['full mip chain'] += texture['full_chain']
                d['non power of two'] += bool(texture['width'] & (texture['width'] - 1) or texture['height'] & (texture['height'] - 1))
                d['not multiple of 4'] += bool(texture['width'] % 4 or texture['height'] % 4)
                d['max edge'] = max(d['max edge'], texture['width'], texture['height'])
                d['decoded bytes'] += decoded
                entries[key] = dict(format=name, width=texture['width'], height=texture['height'], mips=len(texture['mips']),
                                    faces=texture['faces'])
                if np is not None and texture['format'] in BLOCK_BYTES:
                    use = alpha_use(texture['format'], data[144:], np)
                    d['alpha: ' + use] += 1
                    entries[key]['alpha'] = use
        array_rows, in_arrays = [], set()
        for key, names in arrays.items():
            members = []
            for raw in names:
                name = raw.decode('ascii', 'replace')
                found = None
                for candidate in (name.rsplit('.', 1)[0] + '.texture', name):   # the key is the path with .texture
                    k = packed_key(candidate.lower().replace('\\', '/'))
                    if k in entries:
                        found = k
                        break
                members.append(dict(name=name, key=None if found is None else '%08x' % found,
                                    **(entries[found] if found is not None else {})))
                if found is not None:
                    in_arrays.add(found)
            sets = {(m.get('format'), m.get('width'), m.get('height'), m.get('mips')) for m in members}
            array_rows.append(dict(key='%08x' % key, elements=len(members), unresolved=sum(m['key'] is None for m in members),
                                   uniform=len(sets) == 1, formats=sorted({str(m.get('format')) for m in members}),
                                   alpha=sorted({str(m.get('alpha')) for m in members}), members=members))
        if not formats and not arrays:
            report['archives'][index.name] = dict(entries=len(archive.rows), textures=0, other=dict(other))
            continue
        element_formats = collections.Counter(entries[k]['format'] + ('' if 'alpha' not in entries[k] else ' ' + entries[k]['alpha'])
                                              for k in in_arrays)
        report['archives'][index.name] = dict(
            entries=len(archive.rows), textures=sum(formats.values()), formats=dict(formats),
            details={name: dict(counter) for name, counter in details.items()}, other=dict(other),
            texture_arrays=dict(descriptors=len(arrays), elements=sum(r['elements'] for r in array_rows),
                                unresolved=sum(r['unresolved'] for r in array_rows),
                                distinct_element_textures=len(in_arrays), element_formats=dict(element_formats),
                                non_uniform=[r['key'] for r in array_rows if not r['uniform']],
                                mixed_alpha=[r['key'] for r in array_rows if len(r['alpha']) > 1]),
            arrays=array_rows)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', nargs='?')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--no-alpha', action='store_true')
    args = parser.parse_args()
    game = Path(args.game or json.loads((ROOT / 'port.json').read_text())['game_directory'])
    report = scan(game, not args.no_alpha)
    text = json.dumps(report, indent=1)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + '\n', encoding='utf-8')
    for name, row in report['archives'].items():
        print(name, row['entries'], 'entries;', row.get('formats', {}), row.get('other', {}),
              {k: v for k, v in row.get('texture_arrays', {}).items()})
        for fmt, d in row.get('details', {}).items():
            print('   ', fmt, d)
    print('unread:', report['unread'])


if __name__ == '__main__':
    main()
