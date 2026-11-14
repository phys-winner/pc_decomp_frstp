"""Small real conversion, opaque preservation, prefix resume, corruption and geometry gates."""
import json
from pathlib import Path
import struct
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
import prepare_astc_archives as converter


def rejects(function):
    try:function()
    except ValueError:return
    raise AssertionError('Expected malformed fixture rejection')


def texture(fmt,cube=False):
    width=13;levels=3;faces=6 if cube else 1
    header=bytearray(b'\xa5'*144);struct.pack_into('<4I',header,0,width,0xffffffff if cube else width,fmt,levels)
    payload=bytearray()
    for n in range(levels):
        w=width>>(levels-1-n);unit=converter.BC[fmt];size=((w+3)//4)**2*unit
        struct.pack_into('<HHI',header,16+n*8,w,w,size)
        for face in range(faces):
            color=struct.pack('<HHI',((face+1)*0x2104)&0xffff,0,0)
            if fmt==0x31545844:block=color
            elif fmt==0x33545844:block=b'\xff'*8+color
            else:block=b'\xff\x00'+b'\x00'*6+color
            payload.extend(block*(size//unit))
    return bytes(header+payload)


def main():
    geometries=0;subresources=0
    for fmt in converter.BC:
        for cube in (False,True):
            data=texture(fmt,cube);shape=converter.inspect_header(data[:144],len(data))
            converted,records=converter.convert_entry(data,shape);subresources+=len(records)
            assert struct.unpack_from('<I',converted,8)[0]==converter.PRIVATE_FORMAT
            allowed=set(range(8,12))|{p for n in range(shape['mip_count'])for p in range(20+n*8,24+n*8)}
            assert all(converted[p]==data[p]for p in range(144)if p not in allowed)
            assert len(converted)==shape['target_bytes']
            for row in records:
                mip=shape['mips'][row['canonical_mip']]
                assert row['source_offset']==mip['source_offset']+row['face']*mip['source_face_bytes']
                assert row['D3D_subresource']==row['face']*shape['mip_count']+row['D3D_mip']
                assert converter.digest(converted[row['output_offset']:row['output_offset']+row['output_bytes']])==row['output_sha256']
            rejects(lambda:converter.inspect_header(data[:144],len(data)-1))
            rejects(lambda:converter.inspect_header(data[:144],len(data)+1))
            altered=bytearray(data[:144]);struct.pack_into('<H',altered,16,4)
            rejects(lambda:converter.inspect_header(altered,len(data)));geometries+=1
    # DXT3 endpoint0<=endpoint1 must still use four colors, never BC1 alpha mode.
    bc2=b'\xff'*8+struct.pack('<HHI',0x001f,0xf800,0xffffffff)
    decoded=converter.decode_bc(bc2,4,4,0x33545844)
    assert decoded==bytes([170,0,85,255])*16
    transparent=struct.pack('<HHI',0,0xffff,0xffffffff)
    assert converter.decode_bc(transparent,4,4,0x31545844)==b'\0'*64
    assert converter.opaque_kind(b'<?xml version="1.0"?><Array><Texture id="1"/></Array>')=='XML'
    rejects(lambda:converter.opaque_kind(b'not an image or XML'))
    with tempfile.TemporaryDirectory(prefix='fp-astc-archive-',dir=ROOT/'build')as folder:
        work=Path(folder);target=work/'resume.bin';partial=work/'resume.bin.partial';partial.write_bytes(b'prefix')
        converter.owned_write(target,b'prefix payload');assert target.read_bytes()==b'prefix payload'and not partial.exists()
        converter.owned_write(target,b'prefix payload')
        rejects(lambda:converter.owned_write(target,b'changed payload'))
        wrong=work/'wrong.bin.partial';wrong.write_bytes(b'wrong')
        rejects(lambda:converter.owned_write(work/'wrong.bin',b'expected payload'))
        data=texture(0x35545844);shape=converter.inspect_header(data[:144],len(data));encoded,cached_subresources=converter.convert_entry(data,shape)
        cache=work/'cache';(cache/'entries').mkdir(parents=True);(cache/'chunks').mkdir()
        row={'index':0,'key':42,'source_row':[42,len(data),len(data),0,0],'source_sha256':converter.digest(data),'shape':shape}
        record={'index':0,'key':42,'source_sha256':row['source_sha256'],'source_bytes':len(data),'output_bytes':len(encoded),
                'settings_sha256':'fixed','converted':True,'old_format':shape['format'],'new_format':converter.PRIVATE_FORMAT,
                'faces':1,'mips':3,'output_sha256':converter.digest(encoded),'subresources':cached_subresources}
        (cache/'entries/0000-42.json').write_text(json.dumps(record));(cache/'chunks/0000-42.bin').write_bytes(encoded)
        assert converter.verified_cached_entry(cache,row,'fixed')==record
        (cache/'chunks/0000-42.bin').write_bytes(bytes([encoded[0]^1])+encoded[1:])
        rejects(lambda:converter.verified_cached_entry(cache,row,'fixed'))
        (cache/'chunks/0000-42.bin').write_bytes(encoded)
        rejects(lambda:converter.verified_cached_entry(cache,row,'changed settings'))
        # Resume an interrupted assembled archive by comparing its exact prefix.
        raw=work/'raw';raw.mkdir();old_raw=converter.RAW;converter.RAW=raw
        try:
            preserved=b'<Array/>\n';(raw/'textures-s3.dat').write_bytes(preserved)
            chunk=b'converted ASTC fixture';(work/'chunks').mkdir();(work/'chunks/0000-10.bin').write_bytes(chunk)
            plan=[{'index':0,'key':10,'shape':{'converted':True},'source_row':[10,1,1,0,0]},
                  {'index':1,'key':11,'shape':{'converted':False},'source_row':[11,len(preserved),len(preserved),0,0]}]
            records=[{'key':10,'output_bytes':len(chunk),'output_sha256':converter.digest(chunk)},
                     {'key':11,'output_bytes':len(preserved),'output_sha256':converter.digest(preserved),'source_sha256':converter.digest(preserved)}]
            index=b'\x00\x02\x01'+struct.pack('<Q',2)
            destination=work/'textures-s3';destination.mkdir();(destination/'textures-s3.dat.partial').write_bytes(chunk[:5])
            result=converter.assemble(work,plan,records,index,len(chunk)+len(preserved))
            assert (destination/'textures-s3.dat').read_bytes()==chunk+preserved
            assert set(result)=={'textures-s3.dat','textures-s3.idx'}
            assert converter.assemble(work,plan,records,index,len(chunk)+len(preserved))==result
            (destination/'textures-s3.dat').write_bytes(b'x'+(chunk+preserved)[1:])
            rejects(lambda:converter.assemble(work,plan,records,index,len(chunk)+len(preserved)))
        finally:converter.RAW=old_raw
    plan,_,_,_,_,counts,predicted=converter.inventory()
    assert len(plan)==4177 and predicted==11782746468
    print(json.dumps({'success':True,'BC_geometry_variants':geometries,'encoded_subresources':subresources,
                      'BC2_four_color_mode':True,'partial_entry_and_archive_resume':True,'changed_existing_output_rejected':True,
                      'all_real_entry_headers_verified':len(plan),'target_bytes':predicted,'format_counts':counts},indent=2))


if __name__=='__main__':main()
