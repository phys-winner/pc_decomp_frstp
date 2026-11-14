"""Archive conversion boundaries; uses independent expected payload bytes."""
import importlib.util,io,struct,unittest,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('raw_archives',ROOT/'tools/prepare_raw_archives.py')
raw=importlib.util.module_from_spec(spec);spec.loader.exec_module(raw)

class RawArchiveTests(unittest.TestCase):
    def test_sync_flush_and_raw(self):
        for payload in [b'',b'known archive bytes\x00',bytes(range(256))*10000]:
            compressor=zlib.compressobj(wbits=31)
            compressed=compressor.compress(payload)+compressor.flush(zlib.Z_SYNC_FLUSH)
            inflater=zlib.decompressobj(31)
            self.assertEqual(inflater.decompress(compressed),payload)
            self.assertFalse(inflater.eof)
            result=io.BytesIO()
            raw.decode(io.BytesIO(compressed),len(compressed),len(payload),True,result.write)
            self.assertEqual(result.getvalue(),payload)
            result=io.BytesIO()
            raw.decode(io.BytesIO(payload),len(payload),len(payload),False,result.write)
            self.assertEqual(result.getvalue(),payload)

    def test_malformed_payload(self):
        payload=b'asset'*1000
        compressor=zlib.compressobj(wbits=31)
        encoded=compressor.compress(payload)+compressor.flush(zlib.Z_SYNC_FLUSH)
        cases=[(encoded,len(encoded),1,True),(encoded,len(encoded),len(payload)+1,True),
               (encoded[:8],len(encoded),len(payload),True),(b'broken',6,6,True),
               (b'a',2,2,False),(b'ab',2,1,False)]
        for data,stored,decoded,flag in cases:
            with self.subTest(stored=stored,decoded=decoded,flag=flag):
                with self.assertRaises((ValueError,zlib.error)):
                    raw.decode(io.BytesIO(data),stored,decoded,flag,lambda block:None)

    def test_index_bounds(self):
        prefix=b'\x00\x02\x01'+struct.pack('<Q',1)
        valid=prefix+struct.pack('<IQQQB',123,4,4,8,0)
        self.assertEqual(raw.entries(valid,12),[(123,4,4,8,0)])
        for data,size in [(b'',0),(valid[:-1],12),(valid,11),
                          (prefix+struct.pack('<IQQQB',123,4,5,8,0),12),
                          (prefix+struct.pack('<IQQQB',123,4,5,8,2),12)]:
            with self.assertRaises(ValueError):raw.entries(data,size)

if __name__=='__main__':unittest.main()
