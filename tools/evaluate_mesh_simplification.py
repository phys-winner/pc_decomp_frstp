"""How far automatic simplification of the PC meshes goes, against the hand-made Switch meshes.

    python -I -X utf8 tools/evaluate_mesh_simplification.py PC_TEMPLATES_STEM SWITCH_TEMPLATES_STEM MESHOPT_DLL
              [--errors 0.03 0.06 0.12 0.25] [--options 44] [--attributes] [--normal-weight 0.5] [--uv-weight 1.0]
              [--grep TEXT] [--limit N]

For every mesh template present on both sides the first rendering-data block of the PC archive
(vertex buffer + 16-bit triangle list, tools/inspect_mesh_templates.py) is simplified with
meshoptimizer (reference/meshoptimizer v0.25; absolute error in object units) and the resulting
triangle count is printed next to the PC and the Switch counts.  The whole index buffer is one range
here; the port simplifies every draw range on its own, so real numbers are a little higher.

--options are meshopt_Simplify* bits: 4 absolute error, 8 prune small disconnected parts, 32 permissive
(collapses across attribute seams).  These models are made of many separate small parts with hard
edges everywhere; without prune and permissive the simplifier stops at about 60 % of the triangles
even with a 25 cm error.  --attributes passes normals and texture coordinates as attributes, so a
collapse that would move a texture seam or flip shading costs error and is avoided.
"""
import argparse
import ctypes
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from liquid_archive import Archive  # noqa: E402
from inspect_mesh_templates import CANDIDATE  # noqa: E402

SPARSE, ABSOLUTE, PRUNE, PERMISSIVE = 2, 4, 8, 32
# vertex declaration usages of the engine (meshtemplaterenderingdataserialization.cpp)
POSITION, NORMAL, TEXCOORD = 0, 3, 5


def half(value):
    return struct.unpack('<e', struct.pack('<H', value))[0]


def first_block(data, with_layout=False):
    """(vertex bytes, stride, vertices, indices tuple[, layout]) of the first rendering-data block."""
    for match in CANDIDATE.finditer(data):
        at = match.start() - 4
        if at < 0 or at + 28 > len(data):
            continue
        vertices, stride, a, b, c, size, e = struct.unpack_from('<7I', data, at)
        if not vertices or a or b or c or e or size != vertices * stride or at + 28 + size + 20 > len(data):
            continue
        index_at = at + 28 + size
        triangles, x, y, z, index_bytes = struct.unpack_from('<5I', data, index_at)
        if x or y or z or index_at + 20 + index_bytes > len(data):
            continue
        indices = struct.unpack_from('<%dH' % (triangles * 3), data, index_at + 20)
        if indices and max(indices) >= vertices:
            continue
        block = [data[at + 28:at + 28 + size], stride, vertices, indices]
        if with_layout:
            block.append(layout_before(data, at))
        return block
    return None


def layout_before(data, header):
    """{usage: (offset, type)} of stream 0 from the two declarations stored in front of the vertex header."""
    for second in range(header - 12, max(0, header - 140), -8):
        count2, = struct.unpack_from('<I', data, second)
        if 0 < count2 <= 16 and second + 4 + 8 * count2 == header:
            for first in range(second - 12, max(0, second - 140), -8):
                count1, = struct.unpack_from('<I', data, first)
                if 0 < count1 <= 16 and first + 4 + 8 * count1 == second:
                    result = {}
                    for i in range(count1):
                        stream, offset, kind, usage, index, _ = struct.unpack_from('<HHBBBB', data, first + 4 + 8 * i)
                        if stream == 0 and index == 0:
                            result.setdefault(usage, (offset, kind))
                    return result
    return {}


def attributes(vertex_data, stride, vertices, layout):
    """Five floats per vertex: unit normal and the first texture coordinate (zeros where the format is unknown)."""
    values = [0.0] * (vertices * 5)
    normal, uv = layout.get(NORMAL), layout.get(TEXCOORD)
    for vertex in range(vertices):
        base = vertex * stride
        if normal and normal[1] == 10:      # four signed normalised 16-bit values
            x, y, z = struct.unpack_from('<3h', vertex_data, base + normal[0])
            length = (x * x + y * y + z * z) ** 0.5 or 1.0
            values[vertex * 5:vertex * 5 + 3] = (x / length, y / length, z / length)
        if uv and uv[1] == 15:              # two half floats
            u, v = struct.unpack_from('<2H', vertex_data, base + uv[0])
            values[vertex * 5 + 3:vertex * 5 + 5] = (half(u), half(v))
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pc')
    ap.add_argument('switch')
    ap.add_argument('dll')
    ap.add_argument('--errors', nargs='+', type=float, default=[0.03, 0.06, 0.12, 0.25])
    ap.add_argument('--options', type=int, default=ABSOLUTE | PRUNE | PERMISSIVE)
    ap.add_argument('--attributes', action='store_true')
    ap.add_argument('--normal-weight', type=float, default=0.5)
    ap.add_argument('--uv-weight', type=float, default=1.0)
    ap.add_argument('--grep', action='append')
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()
    lib = ctypes.CDLL(str(Path(args.dll).resolve()))
    lib.meshopt_simplify.restype = ctypes.c_size_t
    lib.meshopt_simplify.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t,
                                     ctypes.c_size_t, ctypes.c_float, ctypes.c_uint, ctypes.POINTER(ctypes.c_float)]
    lib.meshopt_simplifyWithAttributes.restype = ctypes.c_size_t
    lib.meshopt_simplifyWithAttributes.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t,
                                                   ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                                   ctypes.c_size_t, ctypes.c_float, ctypes.c_uint, ctypes.POINTER(ctypes.c_float)]
    weights = (ctypes.c_float * 5)(args.normal_weight, args.normal_weight, args.normal_weight, args.uv_weight, args.uv_weight)
    pc, switch = Archive(args.pc), Archive(args.switch)
    totals = {'pc': 0, 'switch': 0}
    totals.update({error: 0 for error in args.errors})
    shown = skipped = 0
    print('%-62s %8s %8s  %s' % ('template', 'pc tris', 'switch', '  '.join('e=%-5g' % e for e in args.errors)))
    for key, name in sorted(switch.names.items(), key=lambda kv: kv[1]):
        if not name.endswith('.binarytemplate') or key not in pc.rows or key not in switch.rows:
            continue
        if args.grep and not any(text in name for text in args.grep):
            continue
        a, b = first_block(pc.read(key), with_layout=True), first_block(switch.read(key))
        if not a or not b or len(a[3]) < 300:
            continue
        vertex_data, stride, vertices, indices, layout = a
        if layout.get(POSITION, (None, None)) != (0, 2):
            skipped += 1
            continue
        count = len(indices)
        source = (ctypes.c_uint * count)(*indices)
        target = (ctypes.c_uint * count)()
        positions = ctypes.create_string_buffer(bytes(vertex_data), len(vertex_data))
        extra = (ctypes.c_float * (vertices * 5))(*attributes(vertex_data, stride, vertices, layout)) if args.attributes else None
        results = []
        for error in args.errors:
            reached = ctypes.c_float(0)
            if args.attributes:
                kept = lib.meshopt_simplifyWithAttributes(target, source, count, positions, vertices, stride, extra, 20, weights, 5, None,
                                                          0, error, args.options, ctypes.byref(reached))
            else:
                kept = lib.meshopt_simplify(target, source, count, positions, vertices, stride, 0, error, args.options, ctypes.byref(reached))
            results.append(kept // 3)
            totals[error] += kept // 3
        totals['pc'] += count // 3
        totals['switch'] += len(b[3]) // 3
        shown += 1
        if not args.limit or shown <= args.limit:
            print('%-62s %8d %8d  %s' % (name[-62:], count // 3, len(b[3]) // 3, '  '.join('%-7d' % r for r in results)))
    print('%-62s %8d %8d  %s' % ('TOTAL over %d meshes (%d skipped)' % (shown, skipped), totals['pc'], totals['switch'],
                                 '  '.join('%-7d' % totals[e] for e in args.errors)))
    print('%-62s %8s %8.3f  %s' % ('ratio to PC', '1', totals['switch'] / max(1, totals['pc']),
                                   '  '.join('%-7.3f' % (totals[e] / max(1, totals['pc'])) for e in args.errors)))


if __name__ == '__main__':
    main()
