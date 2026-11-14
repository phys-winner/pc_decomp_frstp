"""Read entries of LiquidEngine archives (.idx/.dat, optional .str name table): PC and Switch.

    python -I -X utf8 tools/liquid_archive.py list  ARCHIVE_STEM [--grep TEXT]
    python -I -X utf8 tools/liquid_archive.py get   ARCHIVE_STEM NAME_OR_KEY OUTPUT_FILE

ARCHIVE_STEM is the path without extension (…/templates).  PC archives compress an entry with gzip,
the Switch ones with zstd; the index header tells them apart (00 02 01 against 00 05 02).  An entry
key is the MurmurHash2 of the canonical lower-case path, so a name resolves in an archive that has
no name table as well.  Run with -I when the archive comes from an untrusted source.
"""
import argparse
import ctypes
import struct
import sys
import zlib
from pathlib import Path

RECORD = struct.Struct('<IQQQB')
ZSTD = Path('C:/Portable/Total.Commander/Utilites/imhex/libzstd.dll')


def packed_key(name):
    """Original 0x140E5A8E0: MurmurHash2 of the canonical path with seed 0."""
    data = name.encode('ascii')
    h, at, multiplier = len(data), 0, 0x5bd1e995
    while at + 4 <= len(data):
        k = int.from_bytes(data[at:at + 4], 'little') * multiplier & 0xffffffff
        k ^= k >> 24
        k = k * multiplier & 0xffffffff
        h = (h * multiplier & 0xffffffff) ^ k
        at += 4
    if at < len(data):
        h = (h ^ int.from_bytes(data[at:], 'little')) * multiplier & 0xffffffff
    h ^= h >> 13
    h = h * multiplier & 0xffffffff
    return h ^ (h >> 15)


class Archive:
    def __init__(self, stem):
        self.stem = Path(stem)
        raw = self.stem.with_suffix('.idx').read_bytes()
        self.header = raw[:3]
        if self.header not in (b'\x00\x02\x01', b'\x00\x05\x02'):
            raise ValueError('unknown archive header %s' % self.header.hex())
        count, = struct.unpack_from('<Q', raw, 3)
        if len(raw) != 11 + count * RECORD.size:
            raise ValueError('index length does not match its entry count')
        self.rows = {key: (stored, decoded, offset, flag) for key, stored, decoded, offset, flag in RECORD.iter_unpack(raw[11:])}
        self.names = {}
        table = self.stem.with_suffix('.str')
        if table.exists() and table.stat().st_size > 11:
            data = table.read_bytes()
            total, = struct.unpack_from('<Q', data, 3)
            at = 11
            for _ in range(total):
                key, size = struct.unpack_from('<II', data, at)
                at += 8
                self.names[key] = data[at:at + size].decode('ascii', 'replace')
                at += size
        self._zstd = None

    def key(self, name_or_key):
        if isinstance(name_or_key, int):
            return name_or_key
        if name_or_key.startswith('0x'):
            return int(name_or_key, 16)
        return packed_key(name_or_key.lower().replace('\\', '/'))

    def read(self, name_or_key):
        stored, decoded, offset, flag = self.rows[self.key(name_or_key)]
        with self.stem.with_suffix('.dat').open('rb') as stream:
            stream.seek(offset)
            packed = stream.read(stored)
        if len(packed) != stored:
            raise ValueError('truncated archive payload')
        if not flag:
            return packed
        if self.header == b'\x00\x02\x01':
            inflater = zlib.decompressobj(31)
            data = inflater.decompress(packed) + inflater.flush()
        else:
            if self._zstd is None:
                self._zstd = ctypes.CDLL(str(ZSTD))
                self._zstd.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
                self._zstd.ZSTD_decompress.restype = ctypes.c_size_t
            buffer = ctypes.create_string_buffer(decoded)
            if self._zstd.ZSTD_decompress(buffer, decoded, packed, len(packed)) != decoded:
                raise ValueError('zstd payload does not decode to its declared size')
            data = buffer.raw
        if len(data) != decoded:
            raise ValueError('decoded entry length mismatch')
        return data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('list')
    p.add_argument('stem')
    p.add_argument('--grep')
    p = sub.add_parser('get')
    p.add_argument('stem')
    p.add_argument('name')
    p.add_argument('output')
    args = ap.parse_args()
    archive = Archive(args.stem)
    if args.command == 'list':
        for key, (stored, decoded, offset, flag) in sorted(archive.rows.items(), key=lambda kv: archive.names.get(kv[0], '')):
            name = archive.names.get(key, '')
            if not args.grep or args.grep in name:
                print('%08x %10d %10d %d %s' % (key, stored, decoded, flag, name))
    else:
        Path(args.output).write_bytes(archive.read(args.name))
        print(args.output, Path(args.output).stat().st_size)


if __name__ == '__main__':
    main()
