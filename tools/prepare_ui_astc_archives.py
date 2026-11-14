"""Private named BGRA UI -> existing ASTC6; source archives and receipts stay immutable."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import time

import astc_encoder as astc
import numpy as np
from PIL import Image
import texture2ddecoder as decoder

from prepare_astc_archives import inspect_header, need, owned_write, verify_data
from prepare_raw_archives import entries, RECORD
from prepare_texture_fixture import bgra_rgba

ROOT = Path(__file__).resolve().parents[1]
SOURCE_RECEIPT = ROOT / 'build/astc6-archives/conversion-textures-s3-astc6.json'
SOURCE_RECEIPT_SHA = 'fe68a0fc498acca6f2776269e869fae34f8b97f4cf5d83d5706da769eb5bd3c9'
WHITELIST = ROOT / 'build/ui-astc6-candidate/whitelist.json'
OUT = ROOT / 'build/ui-astc6-candidate'
PRIVATE_FORMAT = 0x36545341
SAMPLE_KEYS = (239392142, 657233559, 1708528010, 114456476, 1231634985)
CONVERSION = ROOT / 'build/ui-astc6-archives'
QUALITY_LIMITS = {'alpha_PSNR_min_dB': 45, 'fractional_alpha_MAE_max': 2,
                  'fractional_alpha_p99_max': 24, 'composite_RGB_p99_max': 8,
                  'composite_RGB_p999_max': 24, 'composite_RGB_PSNR_min_dB': 40}
SETTINGS = {'block': 6, 'quality': 60, 'profile': 'LDR', 'flags': 0, 'threads': 8,
            'private_D3DFMT': PRIVATE_FORMAT, 'private_DXGI': [0x7f006601, 0x7f006602],
            'lossy': True, 'quality_limits': QUALITY_LIMITS,
            'source_value_coding': 'BGRA -> RGBA swizzle only. No gamma transform; original caller selects sRGB.',
            'layout': 'Original dimensions, mip count, canonical smallest-to-largest order, then face0..5; unchanged nonselected entries.'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def packed_key(name):
    """Original 140E5A8E0: lower-case canonical path, seed0, MurmurHash2."""
    data = name.encode('ascii')
    need(name == name.lower() and '\\' not in name and not name.startswith('/'), 'Noncanonical packed path')
    h, at, multiplier = len(data), 0, 0x5bd1e995
    while at + 4 <= len(data):
        k = int.from_bytes(data[at:at+4], 'little') * multiplier & 0xffffffff
        k ^= k >> 24
        k = k * multiplier & 0xffffffff
        h = (h * multiplier & 0xffffffff) ^ k
        at += 4
    if at < len(data):
        h = (h ^ int.from_bytes(data[at:], 'little')) * multiplier & 0xffffffff
    h ^= h >> 13
    h = h * multiplier & 0xffffffff
    return h ^ (h >> 15)


def source():
    need(file_sha(SOURCE_RECEIPT) == SOURCE_RECEIPT_SHA, 'Accepted ASTC6 receipt changed')
    receipt = json.loads(SOURCE_RECEIPT.read_text())
    need(receipt['success'] and receipt['all_entries_verified'] and len(receipt['perentry']) == 4177, 'Unverified source archive')
    paths = {name: ROOT / info['path'] for name, info in receipt['outputs'].items()}
    for name, path in paths.items():
        need(not path.is_symlink() and path.stat().st_size == receipt['outputs'][name]['bytes'], 'Source size/path changed')
    index = paths['textures-s3.idx'].read_bytes()
    need(sha(index) == receipt['outputs']['textures-s3.idx']['sha256'], 'Source index changed')
    rows = entries(index, paths['textures-s3.dat'].stat().st_size)
    need(len(rows) == 4177 and len({r[0] for r in rows}) == 4177, 'Source key count changed')
    by_key = {}
    offset = 0
    for row, record in zip(rows, receipt['perentry'], strict=True):
        key, stored, decoded, at, flag = row
        need(at == offset and stored == decoded and flag == 0, 'Source entry layout changed')
        need(key == record['key'] and stored == record['output_bytes'] and at == record['archive_offset'], 'Source entry identity changed')
        by_key[key] = (row, record)
        offset += stored
    need(offset == paths['textures-s3.dat'].stat().st_size, 'Source archive tail changed')
    return receipt, paths, rows, by_key


def selected(by_key):
    manifest = json.loads(WHITELIST.read_text())
    need(manifest['source_receipt_sha256'] == SOURCE_RECEIPT_SHA, 'Whitelist source changed')
    rows = manifest['entries']
    need(len(rows) == 160 and len({r['key'] for r in rows}) == 160, 'Whitelist key set changed')
    for row in rows:
        name = row['name']
        need(name.startswith(('ui/expedition/animate/', 'ui/menu/')) and name.endswith('.texture'), 'Whitelist is outside named UI scope')
        need(not any(word in name for word in ('gradient', 'mask', 'lookup', 'normal', 'sdf', 'density', 'softness')), 'Numerical/ambiguous name selected')
        need(packed_key(name) == row['key'], 'Name hash differs from archive key')
        _, entry = by_key[row['key']]
        need(entry['new_format'] == 21 and entry['output_bytes'] == row['source_bytes'], 'Selected source is not pinned BGRA8')
        need(row['faces'] == row['mips'] == 1, 'Initial UI scope must be single mip/face')
    return manifest, {row['key']: row for row in rows}


def read_source(stream, item):
    row, record = item
    stream.seek(row[3])
    data = stream.read(row[1])
    need(len(data) == row[1] and sha(data) == record['output_sha256'], 'Pinned source entry changed')
    return data


def encode(rgba, width, height, quality):
    config = astc.ASTCConfig(astc.ASTCProfile.LDR, 6, 6, quality=quality, flags=0)
    context = astc.ASTCContext(config, threads=8)
    before = time.perf_counter()
    payload = context.compress(astc.ASTCImage(astc.ASTCType.U8, width, height, data=rgba), astc.ASTCSwizzle.from_str('rgba'))
    elapsed = time.perf_counter() - before
    need(len(payload) == ((width+5)//6)*((height+5)//6)*16, 'ASTC6 payload length differs')
    image = astc.ASTCImage(astc.ASTCType.U8, width, height)
    context.decompress(payload, image, astc.ASTCSwizzle.from_str('rgba'))
    independent = bgra_rgba(decoder.decode_astc(payload, width, height, 6, 6))
    actual = np.frombuffer(image.data, dtype=np.uint8).reshape(-1, 4)
    alternative = np.frombuffer(independent, dtype=np.uint8).reshape(-1, 4)
    agreement = np.abs(actual.astype(np.int16) - alternative).max(axis=0).tolist()
    need(max(agreement) <= 1, 'Independent ASTC decoders differ by more than 1 pre-EOTF LSB')
    return payload, image.data, elapsed, agreement


def error_metrics(before, after):
    a = np.frombuffer(before, dtype=np.uint8).reshape(-1, 4).astype(np.int32)
    b = np.frombuffer(after, dtype=np.uint8).reshape(-1, 4).astype(np.int32)
    delta = np.abs(a-b)
    def summary(d):
        if not len(d):
            return None
        mse = (d.astype(np.float64)**2).mean(axis=0)
        return {'maximum': d.max(axis=0).tolist(), 'p99': np.quantile(d, .99, axis=0).tolist(),
                'p999': np.quantile(d, .999, axis=0).tolist(), 'MAE': d.mean(axis=0).tolist(),
                'PSNR_dB': [None if not x else 10*math.log10(255*255/x) for x in mse]}
    edge = (a[:,3] > 0) & (a[:,3] < 255)
    opaque = a[:,3] == 255
    # These are diagnostics for two conventional straight-alpha backgrounds,
    # not an assertion that every original UI material has this blend state.
    def composite(x, background):
        return (x[:,:3]*x[:,3,None] + background*(255-x[:,3,None]) + 127)//255
    return {'RGBA': summary(delta), 'opaque_RGBA': summary(delta[opaque]), 'fractional_alpha_RGBA': summary(delta[edge]),
            'straight_over_black_RGB': summary(np.abs(composite(a,0)-composite(b,0))),
            'straight_over_white_RGB': summary(np.abs(composite(a,255)-composite(b,255))),
            'source_alpha': {'zero': int((a[:,3] == 0).sum()), 'opaque': int(opaque.sum()), 'fractional': int(edge.sum())}}


def preview(path, rgba, width, height):
    image = Image.frombytes('RGBA', (width, height), rgba)
    image.thumbnail((1280, 800), Image.Resampling.LANCZOS)
    checker = Image.new('RGBA', image.size, (72,72,72,255))
    checker.alpha_composite(image)
    checker.convert('RGB').save(path)


def quality_gate(metrics):
    failures = []
    psnr = metrics['RGBA']['PSNR_dB'][3]
    if psnr is not None and psnr < QUALITY_LIMITS['alpha_PSNR_min_dB']:
        failures.append('alpha PSNR')
    edge = metrics['fractional_alpha_RGBA']
    if edge is not None:
        if edge['MAE'][3] > QUALITY_LIMITS['fractional_alpha_MAE_max']:
            failures.append('fractional alpha MAE')
        if edge['p99'][3] > QUALITY_LIMITS['fractional_alpha_p99_max']:
            failures.append('fractional alpha p99')
    for background in ('black', 'white'):
        item = metrics['straight_over_'+background+'_RGB']
        if max(item['p99']) > QUALITY_LIMITS['composite_RGB_p99_max']:
            failures.append(background+' composite p99')
        if max(item['p999']) > QUALITY_LIMITS['composite_RGB_p999_max']:
            failures.append(background+' composite p999')
        if any(p is not None and p < QUALITY_LIMITS['composite_RGB_PSNR_min_dB'] for p in item['PSNR_dB']):
            failures.append(background+' composite PSNR')
    return {'accepted': not failures, 'failures': failures}


def convert_ui(data, shape):
    need(shape == inspect_header(data[:144], len(data)) and shape['format'] == 21, 'Selected BGRA geometry changed')
    output = bytearray(data[:144])
    struct.pack_into('<I', output, 8, PRIVATE_FORMAT)
    subresources = []
    for mip in shape['mips']:
        w, h = mip['width'], mip['height']
        size = ((w+5)//6)*((h+5)//6)*16
        struct.pack_into('<I', output, 20+8*mip['canonical_index'], size)
        for face in range(shape['faces']):
            start = mip['source_offset']+face*mip['source_face_bytes']
            block = data[start:start+mip['source_face_bytes']]
            need(len(block) == w*h*4, 'Selected BGRA face byte length differs')
            rgba = bgra_rgba(block)
            packed, decoded, elapsed, agreement = encode(rgba, w, h, SETTINGS['quality'])
            metrics = error_metrics(rgba, decoded)
            subresources.append({'canonical_mip': mip['canonical_index'], 'face': face,
                'D3D_mip': shape['mip_count']-1-mip['canonical_index'],
                'D3D_subresource': face*shape['mip_count']+shape['mip_count']-1-mip['canonical_index'],
                'width': w, 'height': h, 'source_offset': start, 'source_bytes': len(block),
                'source_sha256': sha(block), 'RGBA_source_sha256': sha(rgba),
                'output_offset': len(output), 'output_bytes': len(packed), 'output_sha256': sha(packed),
                'encode_seconds': elapsed, 'independent_decoder_max_RGBA_LSB': agreement,
                'errors': metrics, 'quality_gate': quality_gate(metrics)})
            output.extend(packed)
    accepted = all(item['quality_gate']['accepted'] for item in subresources)
    return (bytes(output) if accepted else data), subresources, accepted


def cached_record(directory, key, identity_sha, source_record):
    path = directory / 'entries' / f'{key}.json'
    if not path.exists():
        return None
    need(not path.is_symlink(), 'Symlink cached receipt')
    record = json.loads(path.read_text())
    need(record['key'] == key and record['identity_sha256'] == identity_sha and
         record['source_sha256'] == source_record['output_sha256'] and
         record['source_bytes'] == source_record['output_bytes'], 'Cached entry identity changed')
    if record['converted']:
        chunk = directory / 'chunks' / f'{key}.bin'
        need(not chunk.is_symlink() and chunk.stat().st_size == record['output_bytes'] and
             file_sha(chunk) == record['output_sha256'], 'Cached encoded chunk changed')
    else:
        need(record['output_sha256'] == record['source_sha256'] and
             record['output_bytes'] == record['source_bytes'], 'Preserved quality-failure entry differs')
    return record


def output_blocks(directory, records, source_stream, source_rows):
    for record, row in zip(records, source_rows, strict=True):
        if record['converted']:
            path = directory / 'chunks' / f'{row[0]}.bin'
            with path.open('rb') as stream:
                check = hashlib.sha256()
                while block := stream.read(1024*1024):
                    check.update(block)
                    yield block
        else:
            source_stream.seek(row[3])
            left, check = row[1], hashlib.sha256()
            while left:
                block = source_stream.read(min(left, 1024*1024))
                need(block, 'Truncated preserved source entry')
                left -= len(block)
                check.update(block)
                yield block
        need(check.hexdigest() == record['output_sha256'], 'Assembly input changed')


def assemble(directory, records, source_path, source_rows, source_header):
    destination = directory / 'textures-s3'
    destination.mkdir(exist_ok=True)
    target = destination / 'textures-s3.dat'
    partial = target.with_name(target.name+'.partial')
    predicted = sum(record['output_bytes'] for record in records)
    need(not target.is_symlink() and not partial.is_symlink(), 'Symlink archive output')
    if not target.exists():
        old_size = partial.stat().st_size if partial.exists() else 0
        need(old_size <= predicted, 'Partial output exceeds exact planned length')
        if not partial.exists():
            partial.open('xb').close()
        with source_path.open('rb') as source_stream, partial.open('rb') as old, partial.open('ab') as writer:
            position = 0
            for block in output_blocks(directory, records, source_stream, source_rows):
                count = min(len(block), max(0, old_size-position))
                if count:
                    need(old.read(count) == block[:count], 'Unrecognized partial archive prefix')
                if count < len(block):
                    writer.write(block[count:])
                position += len(block)
            need(position == predicted, 'Assembled length differs from exact plan')
            writer.flush()
            os.fsync(writer.fileno())
        expected, size = verify_data(partial, records)
        need(not target.exists(), 'Completed target appeared during assembly')
        partial.rename(target)
    else:
        need(not partial.exists(), 'Ambiguous completed/partial output')
        expected, size = verify_data(target, records)
    need(size == predicted, 'Completed output length differs')
    packed = bytearray(source_header)
    offset = 0
    for record in records:
        packed.extend(RECORD.pack(record['key'], record['output_bytes'], record['output_bytes'], offset, 0))
        offset += record['output_bytes']
    index_path = destination / 'textures-s3.idx'
    owned_write(index_path, bytes(packed))
    need(entries(bytes(packed), size) == [(r['key'],r['output_bytes'],r['output_bytes'],r['archive_offset'],0) for r in records], 'Output index differs')
    return {p.name: {'path': str(p.relative_to(ROOT)).replace('\\','/'), 'bytes': p.stat().st_size,
            'sha256': expected if p == target else file_sha(p)} for p in (target,index_path)}


def convert():
    receipt, paths, source_rows, by_key = source()
    manifest, chosen = selected(by_key)
    CONVERSION.mkdir(exist_ok=True)
    need(not CONVERSION.is_symlink(), 'Symlink conversion directory')
    helper_paths = [Path(__file__), WHITELIST, ROOT/'tools/prepare_astc_archives.py',
                    ROOT/'tools/prepare_raw_archives.py', ROOT/'tools/prepare_texture_fixture.py']
    modules = {name: {'path': module.__file__, 'sha256': file_sha(module.__file__)}
        for name,module in sys.modules.items() if getattr(module,'__file__',None) and
        name.startswith(('astc_encoder._encoder_','texture2ddecoder._texture2ddecoder','numpy._core._multiarray_umath'))}
    identity = {'schema_version': 2, 'producer': {'path': str(Path(__file__).relative_to(ROOT)).replace('\\','/'), 'sha256': file_sha(__file__)},
        'settings': SETTINGS, 'source_receipt_sha256': SOURCE_RECEIPT_SHA, 'source_pair': receipt['outputs'],
        'whitelist': {'path': str(WHITELIST.relative_to(ROOT)).replace('\\','/'), 'sha256': file_sha(WHITELIST), 'entries':len(chosen)},
        'helper_sha256': {str(p.relative_to(ROOT)).replace('\\','/'): file_sha(p) for p in helper_paths},
        'native_modules': modules, 'numpy_version': np.__version__, 'hardware_ASTC_proof': receipt['hardware_ASTC_proof'],
        'loader_byte_proof': receipt['loader_byte_proof']}
    identity_bytes = (json.dumps(identity,sort_keys=True,indent=2)+'\n').encode()
    identity_sha = sha(identity_bytes)
    owned_write(CONVERSION/'identity.json', identity_bytes)
    need(shutil.disk_usage(CONVERSION).free > 2*paths['textures-s3.dat'].stat().st_size+1024**3, 'Insufficient free space')
    snapshots = {p: (p.stat().st_size,p.stat().st_mtime_ns) for p in (*paths.values(),SOURCE_RECEIPT,*helper_paths)}
    started, full_source, records, encoded, rejected, reused = time.monotonic(), hashlib.sha256(), [], 0, 0, 0
    with paths['textures-s3.dat'].open('rb') as stream:
        for index, row in enumerate(source_rows):
            key = row[0]
            original_record = by_key[key][1]
            data = read_source(stream, by_key[key])
            full_source.update(data)
            record = cached_record(CONVERSION,key,identity_sha,original_record) if key in chosen else None
            if record is not None:
                reused += 1
            else:
                shape, subresources, accepted = None, [], False
                if key in chosen:
                    shape = inspect_header(data[:144],len(data))
                    expected = chosen[key]
                    need(shape and (shape['width'],shape['height'],shape['faces'],shape['mip_count']) ==
                         (expected['width'],expected['height'],1,1), 'Whitelisted dimensions/mips/faces changed')
                    result, subresources, accepted = convert_ui(data,shape)
                else:
                    result = data
                record = {'index':index,'key':key,'name':chosen.get(key,{}).get('name'),
                    'source_sha256':sha(data),'source_bytes':len(data),'output_sha256':sha(result),'output_bytes':len(result),
                    'identity_sha256':identity_sha,'selection_eligible':key in chosen,'converted':accepted,
                    'old_format':original_record['new_format'],'new_format':PRIVATE_FORMAT if accepted else original_record['new_format'],
                    'faces':original_record['faces'],'mips':original_record['mips'],
                    'quality_rejected':key in chosen and not accepted,
                    'subresources':subresources if accepted else [], 'candidate_subresources':subresources if not accepted else []}
                if key in chosen:
                    if accepted:
                        owned_write(CONVERSION/'chunks'/f'{key}.bin',result)
                    owned_write(CONVERSION/'entries'/f'{key}.json',(json.dumps(record,sort_keys=True,indent=2)+'\n').encode())
            encoded += int(record['converted'])
            rejected += int(record['quality_rejected'])
            records.append(record)
            if key in chosen or index+1 == len(source_rows):
                print(json.dumps({'completed':index+1,'total':len(source_rows),'converted':encoded,'quality_preserved':rejected,
                                  'reused':reused,'key':key,'elapsed_seconds':round(time.monotonic()-started,3)}),flush=True)
    source_sha = full_source.hexdigest()
    need(source_sha == receipt['outputs']['textures-s3.dat']['sha256'], 'Full accepted source archive SHA differs')
    print('UI_ASTC_ASSEMBLY_BEGIN',flush=True)
    outputs = assemble(CONVERSION,records,paths['textures-s3.dat'],source_rows,paths['textures-s3.idx'].read_bytes()[:11])
    need(all((p.stat().st_size,p.stat().st_mtime_ns) == stamp for p,stamp in snapshots.items()), 'A pinned input changed')
    need(all(file_sha(ROOT/p) == value for p,value in identity['helper_sha256'].items()), 'Converter, whitelist or helper changed')
    need(file_sha(SOURCE_RECEIPT) == SOURCE_RECEIPT_SHA and
         file_sha(paths['textures-s3.idx']) == receipt['outputs']['textures-s3.idx']['sha256'], 'Source receipt/index changed')
    need(file_sha(paths['textures-s3.dat']) == source_sha, 'Source data changed during output assembly')
    report = {'success':True,'schema_version':2,'timestamp_utc':datetime.now(timezone.utc).isoformat(),
        **identity,'identity_sha256':identity_sha,'source_data_sha256_actual':source_sha,
        'original_archive_sha256':receipt['original_archive_sha256'],'outputs':outputs,'perentry':records,
        'counts':{'entries':len(records),'selected':len(chosen),'BGRA_converted':encoded,'quality_preserved':rejected,
                  'nonselected_byte_exact':len(records)-len(chosen),'preserved_existing_ASTC6':3184},
        'actual_data_bytes_saved':receipt['outputs']['textures-s3.dat']['bytes']-outputs['textures-s3.dat']['bytes'],
        'all_entries_verified':True,'all_source_entries_verified':True,'all_mip_face_geometry_verified':True,
        'original_inputs_read_only':True,'Android_data_receipts_modified':False,'device_actions':False,
        'conversion_seconds':time.monotonic()-started}
    destination = CONVERSION/'conversion-textures-s3-ui-astc6.json'
    if destination.exists():
        old = json.loads(destination.read_text())
        need(old['identity_sha256'] == identity_sha and old['outputs'] == outputs and old['perentry'] == records, 'Existing final receipt changed')
    else:
        owned_write(destination,(json.dumps(report,sort_keys=True,indent=2)+'\n').encode())
    print(json.dumps({'success':True,'receipt':str(destination),'sha256':file_sha(destination),
                     'counts':report['counts'],'actual_data_bytes_saved':report['actual_data_bytes_saved'],
                     'conversion_seconds':report['conversion_seconds']}),flush=True)


def samples():
    receipt, paths, rows, by_key = source()
    manifest, chosen = selected(by_key)
    destination = OUT / 'samples'
    need(not destination.exists(), 'Preserve existing sample evidence')
    destination.mkdir(parents=True)
    report = {'timestamp_utc': datetime.now(timezone.utc).isoformat(), 'source_receipt_sha256': SOURCE_RECEIPT_SHA,
              'whitelist_sha256': file_sha(WHITELIST), 'producer_sha256': file_sha(__file__),
              'profile': 'LDR byte values unchanged; no gamma transform; original caller selects sRGB view',
              'flags': 0, 'block': 6, 'threads': 8, 'device_actions': False, 'samples': []}
    with paths['textures-s3.dat'].open('rb') as stream:
        for key in SAMPLE_KEYS:
            data = read_source(stream, by_key[key])
            shape = inspect_header(data[:144], len(data))
            need(shape and shape['format'] == 21 and shape['faces'] == shape['mip_count'] == 1, 'Sample shape changed')
            w, h = shape['width'], shape['height']
            rgba = bgra_rgba(data[144:])
            preview(destination / f'{key}-source.jpg', rgba, w, h)
            for quality in (10, 60):
                payload, decoded, elapsed, agreement = encode(rgba, w, h, quality)
                owned_write(destination / f'{key}-q{quality}.astc6', payload)
                preview(destination / f'{key}-q{quality}.jpg', decoded, w, h)
                row = {'key': key, 'name': chosen.get(key, {}).get('name', 'excluded gradient qualification sample'),
                       'width': w, 'height': h, 'source_sha256': sha(data), 'RGBA_source_sha256': sha(rgba),
                       'source_bytes': len(data), 'ASTC_payload_bytes': len(payload), 'ASTC_payload_sha256': sha(payload),
                       'quality': quality, 'encode_seconds': elapsed, 'million_pixels_per_second': w*h/elapsed/1e6,
                       'independent_decoder_max_RGBA_LSB': agreement, 'errors': error_metrics(rgba, decoded)}
                report['samples'].append(row)
                owned_write(destination / f'{key}-q{quality}.json', (json.dumps(row, indent=2)+'\n').encode())
                print(json.dumps({'key': key, 'quality': quality, 'encode_seconds': elapsed, 'errors': row['errors']['RGBA']}), flush=True)
    owned_write(OUT / 'sample-report.json', (json.dumps(report, indent=2)+'\n').encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--samples', action='store_true')
    group.add_argument('--convert', action='store_true')
    args = parser.parse_args()
    if args.samples:
        samples()
    else:
        convert()


if __name__ == '__main__':
    main()
