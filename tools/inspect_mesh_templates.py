"""Geometry of LiquidEngine mesh templates: vertex and index counts per rendering-data block,
PC archive against the Nintendo Switch one.

    python -I -X utf8 tools/inspect_mesh_templates.py PC_TEMPLATES_STEM SWITCH_TEMPLATES_STEM OUTPUT_JSON [--prefix graphics/buildings/]

A template (meshtemplate.cpp) stores up to four rendering-data blocks (levels of detail 0..3), each
with its own vertex declaration, vertex buffer and index buffer
(meshtemplaterenderingdataserialization.cpp, 0x1410AFF90):

    u32 elements; elements x 8 bytes      declaration of the vertex stream
    u32 elements; elements x 8 bytes      declaration with the instance stream
    u32 vertices; u32 stride
    20 bytes: 0, 0, 0, bytes = vertices * stride, 0;  vertex data
    u32 triangles
    16 bytes: 0, 0, 0, bytes;  16-bit index data

The blocks are found by that fixed shape of the vertex-buffer header, so nothing else of the
template has to be understood.  Names come from the Switch name table; the key of a resource is
the hash of its path, the same in both archives.
"""
import argparse
import collections
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from liquid_archive import Archive  # noqa: E402

# stride (4..128) as u32, then twelve zero bytes: the start of every vertex-buffer header candidate
CANDIDATE = re.compile(rb'[\x04-\x80]\x00\x00\x00\x00{12}')


def blocks(data):
    """(vertices, stride, triangles, index_bytes, max_index) of every rendering-data block."""
    result, end = [], 0
    for match in CANDIDATE.finditer(data):
        at = match.start() - 4
        if at < end or at < 0 or at + 28 > len(data):
            continue
        vertices, stride, a, b, c, size, e = struct.unpack_from('<7I', data, at)
        if not vertices or a or b or c or e or size != vertices * stride or at + 28 + size + 20 > len(data):
            continue
        index_at = at + 28 + size
        triangles, x, y, z, index_bytes = struct.unpack_from('<5I', data, index_at)
        if x or y or z or index_bytes % 2 or index_at + 20 + index_bytes > len(data):
            continue
        # The declaration in front: its last element belongs to stream 0 or 1 and ends before the stride.
        count = index_bytes // 2
        indices = struct.unpack_from('<%dH' % count, data, index_at + 20) if count else ()
        top = max(indices) if indices else 0
        if top >= vertices:
            continue
        result.append(dict(vertices=vertices, stride=stride, triangles=triangles, indices=count, max_index=top))
        end = index_at + 20 + index_bytes
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pc')
    ap.add_argument('switch')
    ap.add_argument('output')
    ap.add_argument('--prefix', default='graphics/')
    args = ap.parse_args()
    pc, switch = Archive(args.pc), Archive(args.switch)
    rows = {}
    for key, name in sorted(switch.names.items(), key=lambda kv: kv[1]):
        if not name.startswith(args.prefix) or not name.endswith('.binarytemplate') or key not in pc.rows or key not in switch.rows:
            continue
        a, b = blocks(pc.read(key)), blocks(switch.read(key))
        if a or b:
            rows[name] = dict(pc=a, switch=b, pc_bytes=pc.rows[key][1], switch_bytes=switch.rows[key][1])
    Path(args.output).write_text(json.dumps(rows, indent=1) + '\n', encoding='utf-8')
    total = collections.Counter()
    levels = collections.Counter()
    for name, row in rows.items():
        levels['pc blocks %d' % len(row['pc'])] += 1
        levels['switch blocks %d' % len(row['switch'])] += 1
        if row['pc'] and row['switch']:
            total['meshes'] += 1
            total['pc_vertices'] += row['pc'][0]['vertices']
            total['pc_indices'] += row['pc'][0]['indices']
            total['switch_vertices'] += row['switch'][0]['vertices']
            total['switch_indices'] += row['switch'][0]['indices']
            total['identical'] += row['pc'][0]['vertices'] == row['switch'][0]['vertices'] and row['pc'][0]['indices'] == row['switch'][0]['indices']
    print('templates with geometry: %d' % len(rows))
    print('blocks per template:', dict(sorted(levels.items())))
    print('first block, both sides present: %(meshes)d meshes; PC %(pc_vertices)d vertices / %(pc_indices)d indices; '
          'Switch %(switch_vertices)d vertices / %(switch_indices)d indices; identical geometry size in %(identical)d' % total)
    if total['pc_indices']:
        print('Switch / PC: vertices %.3f, indices %.3f' % (total['switch_vertices'] / total['pc_vertices'], total['switch_indices'] / total['pc_indices']))


if __name__ == '__main__':
    main()
