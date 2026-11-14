"""Vertex clustering of the PC meshes on a uniform grid, against the hand-made Switch meshes.

    python -I -X utf8 tools/evaluate_vertex_clustering.py PC_TEMPLATES_STEM SWITCH_TEMPLATES_STEM
                                                          [--cells 0.05 0.1 0.2 0.4] [--grep TEXT] [--limit N]

Every vertex goes to the grid cell of its position; vertices of one cell whose normals point into
the same octant collapse to one of them, and triangles that lose a corner disappear.  The error of
the surface is bounded by the cell size, whatever the topology: these models are made of many
separate small parts, where an edge-collapse simplifier that keeps borders stops early
(tools/evaluate_mesh_simplification.py).
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from liquid_archive import Archive  # noqa: E402
from evaluate_mesh_simplification import first_block  # noqa: E402


def cluster(vertex_data, stride, indices, cell, normal_offset=12):
    """(triangles left, vertices referenced) after clustering; normals are four signed 16-bit values."""
    scale = 1.0 / cell
    representative = {}
    remap = {}
    for vertex in set(indices):
        base = vertex * stride
        x, y, z = struct.unpack_from('<3f', vertex_data, base)
        nx, ny, nz = struct.unpack_from('<3h', vertex_data, base + normal_offset)
        key = (int(x * scale // 1), int(y * scale // 1), int(z * scale // 1), nx >= 0, ny >= 0, nz >= 0)
        remap[vertex] = representative.setdefault(key, vertex)
    kept = set()
    for at in range(0, len(indices), 3):
        a, b, c = remap[indices[at]], remap[indices[at + 1]], remap[indices[at + 2]]
        if a != b and b != c and a != c:
            # the same triangle may now appear several times: one copy is enough
            low = min(a, b, c)
            kept.add((a, b, c) if a == low else (b, c, a) if b == low else (c, a, b))
    used = {v for triangle in kept for v in triangle}
    return len(kept), len(used)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pc')
    ap.add_argument('switch')
    ap.add_argument('--cells', nargs='+', type=float, default=[0.05, 0.1, 0.2, 0.4])
    ap.add_argument('--grep', action='append')
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()
    pc, switch = Archive(args.pc), Archive(args.switch)
    totals = {'pc': 0, 'switch': 0, 'pcv': 0, 'switchv': 0}
    totals.update({cell: 0 for cell in args.cells})
    totals.update({('v', cell): 0 for cell in args.cells})
    shown = 0
    print('%-58s %7s %7s  %s' % ('template (triangles/vertices)', 'pc', 'switch', '  '.join('c=%-10g' % c for c in args.cells)))
    for key, name in sorted(switch.names.items(), key=lambda kv: kv[1]):
        if not name.endswith('.binarytemplate') or key not in pc.rows or key not in switch.rows:
            continue
        if args.grep and not any(text in name for text in args.grep):
            continue
        a, b = first_block(pc.read(key)), first_block(switch.read(key))
        if not a or not b or len(a[3]) < 300 or a[1] < 20:
            continue
        vertex_data, stride, vertices, indices = a
        results = [cluster(vertex_data, stride, indices, cell) for cell in args.cells]
        for cell, (triangles, used) in zip(args.cells, results):
            totals[cell] += triangles
            totals[('v', cell)] += used
        totals['pc'] += len(indices) // 3
        totals['switch'] += len(b[3]) // 3
        totals['pcv'] += len(set(indices))
        totals['switchv'] += len(set(b[3]))
        shown += 1
        if not args.limit or shown <= args.limit:
            print('%-58s %7d %7d  %s' % (name[-58:], len(indices) // 3, len(b[3]) // 3, '  '.join('%-5d/%-6d' % r for r in results)))
    print('%-58s %7d %7d  %s' % ('TOTAL triangles over %d meshes' % shown, totals['pc'], totals['switch'], '  '.join('%-12d' % totals[c] for c in args.cells)))
    print('%-58s %7d %7d  %s' % ('TOTAL vertices referenced', totals['pcv'], totals['switchv'], '  '.join('%-12d' % totals[('v', c)] for c in args.cells)))
    print('%-58s %7s %7.3f  %s' % ('triangles, ratio to PC', '1', totals['switch'] / max(1, totals['pc']),
                                   '  '.join('%-12.3f' % (totals[c] / max(1, totals['pc'])) for c in args.cells)))
    print('%-58s %7s %7.3f  %s' % ('vertices, ratio to PC', '1', totals['switchv'] / max(1, totals['pcv']),
                                   '  '.join('%-12.3f' % (totals[('v', c)] / max(1, totals['pcv'])) for c in args.cells)))


if __name__ == '__main__':
    main()
