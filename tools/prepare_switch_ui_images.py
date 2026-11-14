"""Decode the Switch menu atlases and export their original radial-menu artwork."""
import ctypes
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image
import texture2ddecoder

from prepare_raw_archives import RECORD, entries
from prepare_switch_ui_assets import ROOT, SWITCH, ZSTD, sha

OUT = ROOT / 'android/switch-ui'


def unswizzle(data, width, height, bytes_per_block, block_height):
    """Tegra block-linear GOB addressing; dimensions here are compression blocks."""
    row_bytes = width * bytes_per_block
    columns = (row_bytes + 63) // 64
    x = np.arange(row_bytes, dtype=np.int64)
    output = np.empty((height, row_bytes), dtype=np.uint8)
    source = np.frombuffer(data, dtype=np.uint8)
    for y in range(height):
        address = ((y // (8 * block_height)) * 512 * block_height * columns +
                   (x // 64) * 512 * block_height + ((y % (8 * block_height)) // 8) * 512 +
                   ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 +
                   ((x % 32) // 16) * 32 + (y % 2) * 16 + x % 16)
        if int(address.max()) >= len(source):
            raise ValueError('Truncated Switch texture GOB')
        output[y] = source[address]
    return output.tobytes()


def texture(payload):
    if len(payload) < 28 or struct.unpack_from('<I', payload)[0] != 0x1c7865:
        raise ValueError('Unknown Switch texture header')
    fmt, = struct.unpack_from('<I', payload, 4)
    width, height = struct.unpack_from('<HH', payload, 12)
    if not width or not height or max(width, height) > 8192 or fmt not in (1, 72):
        raise ValueError('Unsupported Switch radial atlas format/shape')
    declared, = struct.unpack_from('<I', payload, 24)
    if declared != len(payload) - 28:
        raise ValueError('Switch texture size differs from its header')
    w, h, block = ((width + 3) // 4, (height + 3) // 4, 16) if fmt == 72 else (width, height, 1)
    block_height = min(16, 1 << max(0, ((h + 7) // 8 - 1).bit_length()))
    linear = unswizzle(payload[28:], w, h, block, block_height)
    if fmt == 72:
        return Image.frombytes('RGBA', (width, height), texture2ddecoder.decode_bc3(linear, width, height), 'raw', 'BGRA')
    rgba = np.full((height, width, 4), 255, dtype=np.uint8)
    rgba[:, :, 3] = np.frombuffer(linear, dtype=np.uint8).reshape(height, width)
    return Image.fromarray(rgba)


def main():
    lib = ctypes.CDLL(str(ZSTD))
    lib.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    lib.ZSTD_decompress.restype = ctypes.c_size_t
    index = (SWITCH / 'textures-s3.idx').read_bytes()
    rows = {r[0]: r for r in entries(b'\0\2\1' + index[3:], (SWITCH / 'textures-s3.dat').stat().st_size)}
    images = {}
    with (SWITCH / 'textures-s3.dat').open('rb') as stream:
        for key in (1011757691, 3290596657):
            _, stored, decoded, offset, compressed = rows[key]
            if not compressed or not 28 < decoded <= 16 * 1024 * 1024:
                raise ValueError('Unexpected radial atlas archive encoding')
            stream.seek(offset)
            packed = stream.read(stored)
            buffer = ctypes.create_string_buffer(decoded)
            if lib.ZSTD_decompress(buffer, decoded, packed, len(packed)) != decoded:
                raise ValueError('Switch radial atlas decompression failed')
            images[key] = texture(buffer.raw)
    if images[1011757691].size != (1024, 2048) or images[3290596657].size != (2048, 2048):
        raise ValueError('Original radial atlas dimensions changed')
    OUT.mkdir(exist_ok=True)
    crops = {'ring.png': images[1011757691].crop((0, 512, 512, 1024)),
             'halo.png': images[1011757691].crop((512, 512, 1024, 1024))}
    crops.update({'action%d.png' % i: images[3290596657].crop((768 + i * 256, 1536, 1024 + i * 256, 1792)) for i in range(5)})
    for name, image in crops.items():
        image.save(OUT / name, optimize=True)
    report = dict(source_title='0100B1C0249B4000', source_version=65536,
                  source_atlases=['UI/Tiles/CONSOLE_express_menu_atlas.dds', 'UI/Tiles/command_radial_atlas.dds'],
                  texture_index_sha256=sha(SWITCH / 'textures-s3.idx'), producer_sha256=sha(__file__),
                  outputs={name: sha(OUT / name) for name in crops})
    (OUT / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
