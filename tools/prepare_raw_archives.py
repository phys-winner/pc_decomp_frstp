"""Prepare lossless raw LiquidEngine archives in a private build directory.

Original archive keys and decoded bytes stay identical. The engine's existing
uncompressed-entry flag replaces gzip; texture formats are not changed.
"""
import argparse
from datetime import datetime,timezone
import hashlib,json,os
from pathlib import Path
import struct,zlib

ROOT=Path(__file__).resolve().parents[1]
RECORD=struct.Struct('<IQQQB')
CHUNK=1024*1024

def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def entries(index,size):
    if len(index)<11 or index[:3]!=b'\x00\x02\x01':raise ValueError('Unknown archive header')
    count,=struct.unpack_from('<Q',index,3)
    if len(index)!=11+count*RECORD.size:raise ValueError('Invalid index length')
    rows=list(RECORD.iter_unpack(index[11:]))
    for key,stored,decoded,offset,flag in rows:
        if flag not in (0,1) or offset>size or stored>size-offset or (not flag and stored!=decoded):raise ValueError('Invalid entry geometry')
    return rows

def decode(stream,stored,decoded,compressed,write):
    inflater=zlib.decompressobj(31) if compressed else None
    total=0;digest=hashlib.sha256()
    def emit(block):
        nonlocal total
        total+=len(block)
        if total>decoded:raise ValueError('Decoded entry exceeds declared size')
        digest.update(block);write(block)
    while stored:
        block=stream.read(min(CHUNK,stored))
        if not block:raise ValueError('Truncated archive payload')
        stored-=len(block)
        if inflater:
            emit(inflater.decompress(block,decoded-total+1))
            if inflater.unconsumed_tail or inflater.unused_data:raise ValueError('Unexpected gzip remainder or excess output')
        else:emit(block)
    # Original archives use Z_SYNC_FLUSH, so gzip EOF is deliberately not required.
    if inflater:emit(inflater.flush())
    if total!=decoded:raise ValueError('Decoded entry length mismatch')
    return digest.hexdigest()

def convert(game,name,output,original):
    index_path,data_path=game/(name+'.idx'),game/(name+'.dat')
    snapshots={p:(p.stat().st_size,p.stat().st_mtime_ns) for p in [index_path,data_path]}
    before={p.name:sha(p) for p in snapshots}
    for p in snapshots:
        if before[p.name]!=original[p.name]['sha256']:raise ValueError('Original input differs: '+p.name)
    raw=index_path.read_bytes();rows=entries(raw,data_path.stat().st_size)
    destination=output/name;destination.mkdir() # Never replace an earlier conversion.
    new_data=destination/data_path.name;new_index=destination/index_path.name
    packed=bytearray(raw[:11]);hashes=[]
    with data_path.open('rb') as source,new_data.open('xb') as target:
        for key,stored,decoded,offset,flag in rows:
            source.seek(offset);new_offset=target.tell()
            digest=decode(source,stored,decoded,flag,target.write)
            packed.extend(RECORD.pack(key,decoded,decoded,new_offset,0));hashes.append(digest)
    new_index.write_bytes(packed)
    result_rows=entries(bytes(packed),new_data.stat().st_size)
    with new_data.open('rb') as result:
        for old,row,digest in zip(rows,result_rows,hashes,strict=True):
            key,stored,decoded,offset,flag=row
            if key!=old[0] or decoded!=old[2] or flag:raise ValueError('Derived entry identity mismatch')
            result.seek(offset)
            if decode(result,stored,decoded,0,lambda block:None)!=digest:raise ValueError('Derived content mismatch')
    for p,snapshot in snapshots.items():
        if (p.stat().st_size,p.stat().st_mtime_ns)!=snapshot or sha(p)!=before[p.name]:raise ValueError('Original input changed during conversion')
    for source,target in [(data_path,new_data),(index_path,new_index)]:
        stat=source.stat();os.utime(target,ns=(stat.st_atime_ns,stat.st_mtime_ns))
    return dict(name=name,entries=len(rows),compressed_entries_before=sum(row[4] for row in rows),
                original_sha256=before,decoded_content_sha256_by_entry=hashes,
                derived={p.name:dict(path=str(p.relative_to(ROOT)),bytes=p.stat().st_size,sha256=sha(p),mtime_ns=p.stat().st_mtime_ns) for p in [new_index,new_data]},
                original_inputs_read_only=True,all_decoded_entry_bytes_verified=True)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archives',nargs='+',choices=['animations','archives','common','localizations','scenes','sequences','templates','textures-s3'])
    args=parser.parse_args()
    if len(set(args.archives))!=len(args.archives):parser.error('Duplicate archive name')
    config=json.loads((ROOT/'port.json').read_text());game=Path(config['game_directory'])
    proof_path=ROOT/'research/device-input-verification.json';proof=json.loads(proof_path.read_text())
    producer_sha,proof_sha=sha(__file__),sha(proof_path)
    output=ROOT/'build/raw-archives';output.mkdir(exist_ok=True)
    receipt=output/('conversion-'+ '-'.join(args.archives)+'.json')
    if receipt.exists():raise ValueError('Preserve the previous conversion receipt')
    rows=[]
    for name in args.archives:
        row=convert(game,name,output,proof['files']);rows.append(row)
        print(json.dumps(dict(archive=name,entries=row['entries'],decoded_bytes=row['derived'][name+'.dat']['bytes'])),flush=True)
    if sha(__file__)!=producer_sha or sha(proof_path)!=proof_sha:raise ValueError('Conversion producer or original proof changed')
    report=dict(timestamp_utc=datetime.now(timezone.utc).isoformat(),producer_sha256=producer_sha,original_proof_sha256=proof_sha,
                archives=rows,phone_modified=False,game_boot_proven=False,texture_format_changed=False,
                scope='Lossless offline archive inflation in a private working copy. No EXE or CPU translation change. Engine raw-entry support must be verified before device promotion.')
    receipt.write_text(json.dumps(report,indent=2)+'\n')
    print('Receipt: '+str(receipt))

if __name__=='__main__':main()
