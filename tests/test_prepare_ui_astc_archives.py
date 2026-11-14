"""Small real-codec geometry and fail-closed resume checks; no original asset writes."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_ui_astc_archives as u


def texture(width=7,height=5,mips=3,faces=1):
    header=bytearray(144)
    struct.pack_into('<4I',header,0,width,0xffffffff if faces==6 else height,21,mips)
    data=bytearray()
    for n in range(mips):
        shift=mips-1-n
        w,h=max(1,width>>shift),max(1,height>>shift)
        struct.pack_into('<HHI',header,16+8*n,w,h,w*h*4)
        for face in range(faces):
            data.extend(bytes((face*31+10,n*31+20,face*17+30,255))*(w*h))
    return bytes(header+data)


class UIArchiveTests(unittest.TestCase):
    def test_original_hash_vectors(self):
        self.assertEqual(u.packed_key('1.1.2/templates/graphics/effects/00sequences/box_publicpenance.binarytemplate'),0x75366)
        self.assertEqual(u.packed_key('ui/expedition/animate/nansen_base_remake_atlas_big.texture'),239392142)
        self.assertEqual(u.packed_key('environment/terrain/substances/base_snowmud_nor.texture'),110917852)
        for s in ('/ui/a.texture','UI/a.texture','ui\\a.texture'):
            with self.assertRaises(ValueError):u.packed_key(s)

    def test_real_codec_small_mip_and_cube_face_order(self):
        for raw in (texture(),texture(4,4,3,6),texture(1,1,1)):
            shape=u.inspect_header(raw[:144],len(raw))
            encoded,records,accepted=u.convert_ui(raw,shape)
            self.assertTrue(accepted)
            self.assertEqual(struct.unpack_from('<I',encoded,8)[0],u.PRIVATE_FORMAT)
            self.assertEqual(encoded[:8],raw[:8])
            self.assertEqual(encoded[12:16],raw[12:16])
            self.assertEqual(len(records),shape['mip_count']*shape['faces'])
            at=144
            for r in records:
                self.assertEqual(r['output_offset'],at)
                self.assertEqual(r['source_offset'],shape['mips'][r['canonical_mip']]['source_offset']+r['face']*r['source_bytes'])
                self.assertLessEqual(max(r['independent_decoder_max_RGBA_LSB']),1)
                packed=encoded[at:at+r['output_bytes']]
                decoded=u.bgra_rgba(u.decoder.decode_astc(packed,r['width'],r['height'],6,6))
                self.assertLessEqual(max(abs(a-b) for a,b in zip(decoded,bytes((r['face']*17+30,r['canonical_mip']*31+20,r['face']*31+10,255))*(r['width']*r['height']))),2)
                at+=r['output_bytes']
            self.assertEqual(at,len(encoded))

    def test_bad_geometry_and_quality_reject_are_byte_exact(self):
        raw=texture()
        for position,value in ((0,0),(12,17),(20,999)):
            bad=bytearray(raw);struct.pack_into('<I',bad,position,value)
            with self.assertRaises(ValueError):u.inspect_header(bad[:144],len(bad))
        for bad in (raw[:-1],raw+b'X'):
            with self.assertRaises(ValueError):u.inspect_header(bad[:144],len(bad))
        with patch.object(u,'quality_gate',return_value={'accepted':False,'failures':['forced test rejection']}):
            result,attempts,accepted=u.convert_ui(raw,u.inspect_header(raw[:144],len(raw)))
        self.assertFalse(accepted)
        self.assertEqual(result,raw)
        self.assertTrue(attempts)
        same=u.error_metrics(bytes((1,2,3,255))*4,bytes((1,2,3,255))*4)
        self.assertTrue(u.quality_gate(same)['accepted'])
        bad=copy.deepcopy(same);bad['RGBA']['PSNR_dB'][3]=44.9
        self.assertFalse(u.quality_gate(bad)['accepted'])

    def test_receipt_preimage_and_whitelist_name_reject(self):
        with tempfile.TemporaryDirectory()as folder:
            p=Path(folder)/'receipt.json';p.write_text('{}')
            with patch.object(u,'SOURCE_RECEIPT',p):
                with self.assertRaisesRegex(ValueError,'receipt changed'):u.source()
            manifest=json.loads(u.WHITELIST.read_text());meta={r['key']:(None,{'new_format':21,'output_bytes':r['source_bytes']})for r in manifest['entries']}
            manifest['entries'][0]['name']='ui/menu/not-the-same.texture'
            p.write_text(json.dumps(manifest))
            with patch.object(u,'WHITELIST',p):
                with self.assertRaisesRegex(ValueError,'Name hash differs'):u.selected(meta)

    def test_resume_prefix_unknown_target_and_cache_mutation(self):
        with tempfile.TemporaryDirectory()as folder:
            root=Path(folder);output=root/'candidate';output.mkdir();(output/'textures-s3').mkdir();(output/'chunks').mkdir()
            source=root/'source.dat';source.write_bytes(b'abcDEFghi')
            rows=[(1,3,3,0,0),(2,3,3,3,0),(3,3,3,6,0)]
            (output/'chunks/2.bin').write_bytes(b'WXYZ')
            def records():
                return [{'key':key,'converted':key==2,'output_bytes':len(data),'output_sha256':u.sha(data)}for key,data in ((1,b'abc'),(2,b'WXYZ'),(3,b'ghi'))]
            partial=output/'textures-s3/textures-s3.dat.partial';partial.write_bytes(b'abcWX')
            with patch.object(u,'ROOT',root):
                a=u.assemble(output,records(),source,rows,b'\0\2\1'+struct.pack('<Q',3))
                self.assertEqual((output/'textures-s3/textures-s3.dat').read_bytes(),b'abcWXYZghi')
                b=u.assemble(output,records(),source,rows,b'\0\2\1'+struct.pack('<Q',3))
                self.assertEqual(a,b)
                (output/'textures-s3/textures-s3.dat').write_bytes(b'unknown!!!')
                with self.assertRaises(ValueError):u.assemble(output,records(),source,rows,b'\0\2\1'+struct.pack('<Q',3))
            other=root/'bad';other.mkdir();(other/'textures-s3').mkdir();(other/'textures-s3/textures-s3.dat.partial').write_bytes(b'BAD')
            with patch.object(u,'ROOT',root):
                with self.assertRaisesRegex(ValueError,'Unrecognized partial'):u.assemble(other,records(),source,rows,b'\0\2\1'+struct.pack('<Q',3))
            (output/'entries').mkdir()
            record={'key':2,'identity_sha256':'identity','source_sha256':u.sha(b'DEF'),'source_bytes':3,
                    'converted':True,'output_bytes':4,'output_sha256':u.sha(b'WXYZ')}
            p=output/'entries/2.json';p.write_text(json.dumps(record))
            expected={'output_sha256':u.sha(b'DEF'),'output_bytes':3}
            self.assertEqual(u.cached_record(output,2,'identity',expected),record)
            (output/'chunks/2.bin').write_bytes(b'BAD!')
            with self.assertRaisesRegex(ValueError,'chunk changed'):u.cached_record(output,2,'identity',expected)
            with self.assertRaisesRegex(ValueError,'identity changed'):u.cached_record(output,2,'different',expected)


if __name__=='__main__':unittest.main()
