"""One pinned Frostpunk asset -> flat Mali BGRA8/ASTC fixtures; no GPU or device actions."""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import time

import astc_encoder as astc
from PIL import Image, __version__ as pillow_version
import texture2ddecoder as decoder

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/mali-texture-fixture'
ASSET_KEY = 53614496
ENTRY_SHA256 = '0bd4b46568bdc3fdcddcdd3851d4304325f46440cb87378f940dc9eadd324220'
RECORD = struct.Struct('<IQQQB')
VARIANTS = [('bgra8-unorm', 44, 1, 4, 'linear'), ('bgra8-srgb', 50, 1, 4, 'srgb'),
            ('astc4x4-unorm', 157, 4, 16, 'linear'), ('astc4x4-srgb', 158, 4, 16, 'srgb'),
            ('astc6x6-unorm', 165, 6, 16, 'linear'), ('astc6x6-srgb', 166, 6, 16, 'srgb')]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def bgra_rgba(data):
    require(len(data) % 4 == 0, 'Pixel byte length is not RGBA-sized')
    result = bytearray(data)
    result[0::4] = data[2::4]
    result[2::4] = data[0::4]
    return bytes(result)


def srgb_channel(value):
    value /= 255.0
    linear = value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4
    return int(math.floor(linear * 255.0 + 0.5))


SRGB_TABLE = bytes(srgb_channel(value) for value in range(256))


def sampled_reference(rgba, color_space):
    require(color_space in ('linear', 'srgb'), 'Unknown sample reference color space')
    if color_space == 'linear':
        return rgba
    result = bytearray(rgba)
    for channel in range(3):
        result[channel::4] = rgba[channel::4].translate(SRGB_TABLE)
    return bytes(result)  # Alpha is linear in an sRGB image too.


def decoder_agreement(left, right):
    require(len(left) == len(right), 'Independent decoder output length differs')
    difference = max((abs(a - b) for a, b in zip(left, right)), default=0)
    require(difference <= 1, 'Independent decoders disagree by more than 1 LSB')
    return difference


def decode_astc_reference(packed, width, height, block, color):
    independent = bgra_rgba(decoder.decode_astc(packed, width, height, block, block))
    if color == 'linear':
        return independent, independent, 0
    # ASTC sRGB expands endpoints as (C << 8) | 0x80, then uses the top
    # eight interpolated bits before EOTF. A profile-agnostic LDR decoder
    # can differ by 1 encoded LSB, amplified to 3 after sRGB linearization.
    context = astc.ASTCContext(astc.ASTCConfig(astc.ASTCProfile.LDR_SRGB, block, block), threads=1)
    image = astc.ASTCImage(astc.ASTCType.U8, width, height)
    context.decompress(packed, image, astc.ASTCSwizzle.from_str('rgba'))
    decoded = image.data  # astc_encoder returns RGBA, unlike texture2ddecoder.
    return decoded, independent, decoder_agreement(decoded, independent)


def inspect_entry(data):
    require(len(data) >= 144, 'Truncated canonical texture header')
    width, height, fmt, levels = struct.unpack_from('<IIII', data)
    require((width, height, fmt, levels) == (256, 256, 0x35545844, 9), 'Unexpected pinned texture geometry/format')
    require(not any(data[16 + levels * 8:144]), 'Unexpected unused mip descriptor bytes')
    offset, result = 144, []
    for index in range(levels):
        w, h, size = struct.unpack_from('<HHI', data, 16 + index * 8)
        expected = ((w + 3) // 4) * ((h + 3) // 4) * 16
        require((w, h) == (1 << index, 1 << index) and size == expected, 'Invalid canonical BC3 mip geometry')
        require(offset <= len(data) and size <= len(data) - offset, 'Truncated canonical mip payload')
        result.append({'canonical_index': index, 'width': w, 'height': h,
                       'canonical_offset': offset, 'canonical_bytes': size, 'data': data[offset:offset + size]})
        offset += size
    require(offset == len(data), 'Canonical texture has an unexpected tail')
    require(digest(data) == ENTRY_SHA256, 'Pinned source texture entry changed')
    return result


def source_entry():
    directory = ROOT / 'build/raw-archives/textures-s3'
    index_path, data_path = directory / 'textures-s3.idx', directory / 'textures-s3.dat'
    receipt_path = ROOT / 'build/raw-archives/conversion-textures-s3.json'
    receipt = json.loads(receipt_path.read_text())
    producer, original_proof = ROOT / 'tools/prepare_raw_archives.py', ROOT / 'research/device-input-verification.json'
    require(receipt['producer_sha256'] == file_sha(producer) and
            receipt['original_proof_sha256'] == file_sha(original_proof) and
            receipt['texture_format_changed'] is False, 'Raw archive producer/proof changed')
    require(len(receipt['archives']) == 1 and receipt['archives'][0]['name'] == 'textures-s3', 'Unexpected source archive scope')
    archive = receipt['archives'][0]
    snapshots = {path: (path.stat().st_size, path.stat().st_mtime_ns) for path in (index_path, data_path, receipt_path)}
    index = index_path.read_bytes()
    require(index[:3] == b'\x00\x02\x01' and len(index) >= 11, 'Invalid archive index header')
    count, = struct.unpack_from('<Q', index, 3)
    require(len(index) == 11 + RECORD.size * count and count == archive['entries'], 'Invalid archive index length')
    require(digest(index) == archive['derived']['textures-s3.idx']['sha256'], 'Raw archive index changed')
    require(data_path.stat().st_size == archive['derived']['textures-s3.dat']['bytes'], 'Raw archive data length changed')
    rows = list(RECORD.iter_unpack(index[11:]))
    matches = [(i, row) for i, row in enumerate(rows) if row[0] == ASSET_KEY]
    require(len(matches) == 1, 'Pinned asset key missing/duplicated')
    entry_index, (key, stored, decoded, offset, compressed) = matches[0]
    require(compressed == 0 and stored == decoded and offset <= data_path.stat().st_size and
            stored <= data_path.stat().st_size - offset, 'Invalid raw source entry span')
    require(archive['all_decoded_entry_bytes_verified'] is True and
            archive['decoded_content_sha256_by_entry'][entry_index] == ENTRY_SHA256, 'Decoded source proof differs')
    with data_path.open('rb') as stream:
        stream.seek(offset)
        data = stream.read(stored)
    inspect_entry(data)
    evidence = {'archive_key': key, 'raw_index': str(index_path.relative_to(ROOT)), 'raw_data': str(data_path.relative_to(ROOT)),
                'raw_index_sha256_actual': digest(index), 'entry_offset': offset, 'entry_bytes': stored,
                'entry_sha256_actual': digest(data), 'decoded_BC3_entry_sha256': ENTRY_SHA256,
                'raw_data_full_sha256_from_conversion_receipt': archive['derived']['textures-s3.dat']['sha256'],
                'raw_data_full_hash_repeated_by_this_tool': False,
                'original_archive_sha256_from_sealed_proof': archive['original_sha256'],
                'conversion_receipt': str(receipt_path.relative_to(ROOT)), 'conversion_receipt_sha256': file_sha(receipt_path)}
    return data, evidence, snapshots


def append_aligned(blob, data):
    blob.extend(b'\x00' * (-len(blob) % 16))
    offset = len(blob)
    blob.extend(data)
    return offset


def write_blob(output, name, data):
    path = output / name
    with path.open('xb') as stream:
        stream.write(data)
    return {'path': name, 'bytes': len(data), 'sha256': digest(data)}


def local_blob(directory, record):
    name = record['path']
    require(isinstance(name, str) and name not in ('', '.', '..') and Path(name).name == name and
            not any(char in name for char in '/\\:'), 'Unsafe fixture blob path')
    path = directory / name
    require(not path.is_symlink() and path.is_file(), 'Missing/symlink fixture blob')
    data = path.read_bytes()
    require(len(data) == record['bytes'] and digest(data) == record['sha256'], 'Fixture blob length/hash changed: ' + name)
    return data


def verify_manifest(path):
    path = Path(path)
    manifest = json.loads(path.read_text())
    require(manifest['schema_version'] == 1 and manifest['asset_key'] == ASSET_KEY and
            manifest['source']['entry_sha256_actual'] == ENTRY_SHA256, 'Fixture asset identity differs')
    require(manifest['reference_revision'] == 2, 'Profile-aware reference revision differs')
    require(manifest['producer']['sha256'] == file_sha(__file__), 'Fixture producer changed')
    source_rgba = local_blob(path.parent, manifest['source']['decoded_source_rgba8'])
    require(len(manifest['source']['decoded_source_mips']) == 9, 'Unexpected decoded source mip count')
    source_end = 0
    for level, mip in enumerate(manifest['source']['decoded_source_mips']):
        width = 256 >> level
        require((mip['level'], mip['canonical_index'], mip['width'], mip['height'], mip['bytes']) ==
                (level, 8 - level, width, width, width * width * 4), 'Decoded source mip geometry differs')
        offset = mip['offset']
        require(offset == (source_end + 15) // 16 * 16 and not any(source_rgba[source_end:offset]), 'Decoded source mip packing differs')
        data = source_rgba[offset:offset + mip['bytes']]
        require(len(data) == mip['bytes'] and digest(data) == mip['sha256'], 'Decoded source mip changed')
        source_end = offset + mip['bytes']
    require(source_end == len(source_rgba), 'Decoded source has an unexpected tail')
    require(len(manifest['variants']) == len(VARIANTS), 'Unexpected variant count')
    for variant, expected in zip(manifest['variants'], VARIANTS, strict=True):
        name, fmt, block, unit, color = expected
        require((variant['name'], variant['vk_format'], variant['block_width'], variant['block_height'],
                 variant['bytes_per_block'], variant['color_space']) == (name, fmt, block, block, unit, color), 'Variant format differs')
        payload = local_blob(path.parent, variant['payload'])
        reference = local_blob(path.parent, variant['reference'])
        decoded = local_blob(path.parent, variant['decoded_rgba8'])
        require(len(variant['mips']) == 9, 'Unexpected mip count')
        last_payload = last_reference = 0
        for level, mip in enumerate(variant['mips']):
            width = 256 >> level
            expected_bytes = ((width + block - 1) // block) ** 2 * unit
            require((mip['level'], mip['canonical_index'], mip['width'], mip['height'], mip['payload_bytes'], mip['reference_bytes']) ==
                    (level, 8 - level, width, width, expected_bytes, width * width * 4), 'Mip geometry/order/length differs')
            po, ro = mip['payload_offset'], mip['reference_offset']
            require(po == (last_payload + 15) // 16 * 16 and ro == (last_reference + 15) // 16 * 16, 'Mip alignment/packing differs')
            require(not any(payload[last_payload:po]) and not any(reference[last_reference:ro]) and
                    not any(decoded[last_reference:ro]), 'Nonzero alignment padding')
            p, r, d = payload[po:po + expected_bytes], reference[ro:ro + width * width * 4], decoded[ro:ro + width * width * 4]
            require(len(p) == expected_bytes and len(r) == len(d) == width * width * 4 and
                    digest(p) == mip['payload_sha256'] and digest(r) == mip['reference_sha256'] and
                    digest(d) == mip['decoded_rgba8_sha256'], 'Mip data/hash differs')
            if block == 1:
                actual, independent, difference = bgra_rgba(p), bgra_rgba(p), 0
            else:
                actual, independent, difference = decode_astc_reference(p, width, width, block, color)
            require(digest(independent) == mip['independent_decoded_rgba8_sha256'] and
                    difference == mip['independent_decoder_max_difference_LSB'], 'Independent decode evidence differs')
            require(actual == d and sampled_reference(d, color) == r, 'Payload decode/sample reference differs')
            if block == 1:
                source_mip = manifest['source']['decoded_source_mips'][level]
                require(d == source_rgba[source_mip['offset']:source_mip['offset'] + source_mip['bytes']], 'BGRA payload differs from decoded source')
            last_payload, last_reference = po + expected_bytes, ro + width * width * 4
        require(len(payload) == last_payload and len(reference) == len(decoded) == last_reference, 'Unexpected payload/reference tail')
    return manifest


def prepare(output=OUT):
    output = Path(output)
    require(not output.exists(), 'Preserve existing texture fixture directory')
    data, source, snapshots = source_entry()
    levels = inspect_entry(data)
    # Known endpoint test makes the decoder's BGRA byte convention explicit.
    red = bytes([255, 0, 0, 0, 0, 0, 0, 0]) + struct.pack('<HHI', 0xf800, 0x07e0, 0)
    require(decoder.decode_bc3(red, 4, 4)[:4] == bytes([0, 0, 255, 255]), 'BC3 decoder channel convention changed')
    agreement = 0
    for mip in levels:
        rgba = bgra_rgba(decoder.decode_bc3(mip['data'], mip['width'], mip['height']))
        independent = Image.frombytes('RGBA', (mip['width'], mip['height']), mip['data'], 'bcn', (3, 'DXT5')).tobytes()
        agreement = max(agreement, decoder_agreement(rgba, independent))
        mip['rgba'] = rgba
    output.mkdir(parents=True)
    source_rgba, source_mips = bytearray(), []
    for level, mip in enumerate(reversed(levels)):
        offset = append_aligned(source_rgba, mip['rgba'])
        source_mips.append({'level': level, 'width': mip['width'], 'height': mip['height'],
                            'offset': offset, 'bytes': len(mip['rgba']), 'sha256': digest(mip['rgba']),
                            'canonical_index': mip['canonical_index'], 'BC3_sha256': digest(mip['data'])})
    source['decoded_source_rgba8'] = write_blob(output, 'source-decoded-rgba8.bin', source_rgba)
    source['decoded_source_mips'] = source_mips
    variants = []
    for name, fmt, block, unit, color in VARIANTS:
        began = time.perf_counter()
        payload, reference, decoded_blob, mips = bytearray(), bytearray(), bytearray(), []
        profile = astc.ASTCProfile.LDR_SRGB if color == 'srgb' else astc.ASTCProfile.LDR
        for level, mip in enumerate(reversed(levels)):
            width, height, rgba = mip['width'], mip['height'], mip['rgba']
            if block == 1:
                packed, decoded = bgra_rgba(rgba), rgba
                independent, difference = decoded, 0
            else:
                config = astc.ASTCConfig(profile, block, block, quality=astc.ASTCQualityPreset.MEDIUM)
                context = astc.ASTCContext(config, threads=1)
                packed = context.compress(astc.ASTCImage(astc.ASTCType.U8, width, height, data=rgba), astc.ASTCSwizzle.from_str('rgba'))
                require(len(packed) == ((width + block - 1) // block) * ((height + block - 1) // block) * unit, 'Encoder block length differs')
                decoded, independent, difference = decode_astc_reference(packed, width, height, block, color)
            sampled = sampled_reference(decoded, color)
            po, ro = append_aligned(payload, packed), append_aligned(reference, sampled)
            require(append_aligned(decoded_blob, decoded) == ro, 'Decode/reference offset differs')
            mips.append({'level': level, 'canonical_index': mip['canonical_index'], 'width': width, 'height': height,
                         'payload_offset': po, 'payload_bytes': len(packed), 'payload_sha256': digest(packed),
                         'reference_offset': ro, 'reference_bytes': len(sampled), 'reference_sha256': digest(sampled),
                         'decoded_rgba8_sha256': digest(decoded),
                         'independent_decoded_rgba8_sha256': digest(independent),
                         'independent_decoder_max_difference_LSB': difference})
        variants.append({'name': name, 'vk_format': fmt, 'color_space': color, 'block_width': block,
                         'block_height': block, 'bytes_per_block': unit, 'mip_count': 9,
                         'encoder_profile': None if block == 1 else ('LDR_SRGB' if color == 'srgb' else 'LDR'),
                         'encoder_quality': None if block == 1 else 'MEDIUM60',
                         'reference_decoder': ('BGRA8 channel swizzle' if block == 1 else
                                               'astc_encoder LDR_SRGB RGBA8' if color == 'srgb' else
                                               'texture2ddecoder LDR BGRA8 explicitly swizzled to RGBA8'),
                         'payload': write_blob(output, name + '-payload.bin', payload),
                         'reference': dict(write_blob(output, name + '-reference-rgba8.bin', reference), format='RGBA8',
                                           semantics='texelFetch RGB ' + ('UNORM' if color == 'linear' else 'sRGB EOTF linear') + '; alpha UNORM; floor(channel*255+0.5)'),
                         'decoded_rgba8': dict(write_blob(output, name + '-decoded-rgba8.bin', decoded_blob),
                                               semantics='RGBA8 before EOTF; ASTC sRGB uses profile-aware sRGB interpolation; texture2ddecoder output is BGRA-swizzled'),
                         'mips': mips, 'host_conversion_seconds': time.perf_counter() - began})
    # Re-read the selected source after conversion, rather than trusting only mtime.
    fresh, _, _ = source_entry()
    require(fresh == data, 'Selected source changed during conversion')
    require(all((path.stat().st_size, path.stat().st_mtime_ns) == stamp for path, stamp in snapshots.items()), 'Source changed during conversion')
    modules = {name: {'path': module.__file__, 'sha256': file_sha(module.__file__)}
               for name, module in sys.modules.items()
               if name.startswith(('astc_encoder._encoder_', 'texture2ddecoder._texture2ddecoder'))}
    manifest = {'schema_version': 1, 'reference_revision': 2, 'asset_key': ASSET_KEY, 'timestamp_utc': datetime.now(timezone.utc).isoformat(),
                'source': source, 'producer': {'path': 'tools/prepare_texture_fixture.py', 'sha256': file_sha(__file__)},
                'tools': {'astc_encoder': astc.__version__, 'texture2ddecoder': decoder.__version__,
                          'Pillow': pillow_version, 'native_modules': modules},
                'layout': {'mip0': 'largest', 'offset_alignment': 16, 'payload_file_headers': False,
                           'each_variant_separate_payload': True, 'exact_file_lengths_required': True,
                           'reference_pixel_format': 'RGBA8', 'BGRA_payload_pixel_format': 'BGRA8',
                           'sRGB_alpha_linear': True, 'sampling': 'texelFetch, integer coordinates, explicit mip; no filtering',
                           'GPU_reference_tolerance_LSB': 1},
                'BC3_independent_decoder_max_difference_LSB': agreement,
                'ASTC_sRGB_reference_rule': {
                    'endpoint_expansion': '(C << 8) | 0x80',
                    'interpolation': 'floor((C0 * (64 - weight) + C1 * weight + 32) / 64), then top 8 bits',
                    'RGB_sampling': 'Apply sRGB EOTF only after profile-specific 8-bit interpolation',
                    'alpha_sampling': 'UNORM; no sRGB EOTF',
                    'independent_decoder_tolerance_LSB_before_EOTF': 1,
                    'specification': 'https://raw.githubusercontent.com/KhronosGroup/DataFormat/main/astc.txt',
                    'official_decoder': 'https://raw.githubusercontent.com/ARM-software/astc-encoder/main/Source/astcenc_color_unquantize.cpp'},
                'semantic_scope': 'The asset material/color role is not inferred. Linear and sRGB are two explicit alternative sampling tests.',
                'variants': variants, 'original_inputs_modified': False, 'Android_contract_modified': False,
                'device_actions': False, 'GPU_tested': False}
    path = output / 'manifest.json'
    with path.open('x', encoding='utf-8') as stream:
        json.dump(manifest, stream, indent=2)
        stream.write('\n')
    verify_manifest(path)
    return path


def self_test(path):
    manifest = verify_manifest(path)
    require(bgra_rgba(bytes([0, 0, 255, 128])) == bytes([255, 0, 0, 128]), 'Channel swizzle test')
    require(sampled_reference(bytes([128, 64, 255, 128]), 'srgb') == bytes([55, 13, 255, 128]), 'sRGB reference/linear alpha test')
    require(decoder_agreement(bytes([0, 4, 255]), bytes([1, 3, 254])) == 1, '1 LSB tolerance test')
    srgb_variant = manifest['variants'][3]
    srgb_mip = srgb_variant['mips'][0]
    srgb_payload = local_blob(Path(path).parent, srgb_variant['payload'])[:srgb_mip['payload_bytes']]
    correct, old_decode, _ = decode_astc_reference(srgb_payload, 256, 256, 4, 'srgb')
    corrected = sampled_reference(correct, 'srgb')
    require(digest(corrected) == 'dd4fb2ace5c85600ad71cee1c5b23ab112456e56a5e9a1bc984648d3b324110d',
            'Pinned profile-aware sRGB interpolation regression')
    require(max(abs(a - b) for a, b in zip(corrected, sampled_reference(old_decode, 'srgb'))) == 3,
            'Pinned asset must expose the profile-agnostic sRGB reference error')
    rejected = []

    def rejects(name, function):
        try:
            function()
        except ValueError:
            rejected.append(name)
        else:
            raise AssertionError('Expected rejection: ' + name)

    rejects('2LSB decoder disagreement', lambda: decoder_agreement(b'\x00', b'\x02'))
    data, _, _ = source_entry()
    rejects('source mutation', lambda: inspect_entry(data[:-1] + bytes([data[-1] ^ 1])))
    rejects('source truncated mip', lambda: inspect_entry(data[:-1]))
    rejects('source extra tail', lambda: inspect_entry(data + b'\x00'))
    # Tiny candidate files only; no recopy of the 18GB archive.
    import shutil
    with tempfile.TemporaryDirectory(prefix='fp-texture-fixture-') as temporary:
        target = Path(temporary)
        for source in Path(path).parent.iterdir():
            if source.is_file():
                shutil.copyfile(source, target / source.name)
        candidate = target / 'manifest.json'
        variant = manifest['variants'][0]
        blob = target / variant['payload']['path']
        original = blob.read_bytes()
        blob.write_bytes(original + b'\x00')
        rejects('payload extra tail', lambda: verify_manifest(candidate))
        blob.write_bytes(original[:-1])
        rejects('payload truncated small mip', lambda: verify_manifest(candidate))
        blob.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        rejects('payload change', lambda: verify_manifest(candidate))
        blob.write_bytes(original)
        decoded_source = target / manifest['source']['decoded_source_rgba8']['path']
        source_original = decoded_source.read_bytes()
        decoded_source.write_bytes(source_original[:-1] + bytes([source_original[-1] ^ 1]))
        rejects('decoded source change', lambda: verify_manifest(candidate))
        decoded_source.write_bytes(source_original)
        reference = target / variant['reference']['path']
        reference_original = reference.read_bytes()
        reference.write_bytes(reference_original[:-1])
        rejects('reference truncated small mip', lambda: verify_manifest(candidate))
        reference.write_bytes(reference_original)
        altered = json.loads(json.dumps(manifest))
        altered['variants'][0]['mips'][-1]['payload_offset'] += 1
        candidate.write_text(json.dumps(altered))
        rejects('unaligned small mip', lambda: verify_manifest(candidate))
        altered = json.loads(json.dumps(manifest))
        altered['variants'][4]['mips'][-1]['payload_bytes'] = 4
        candidate.write_text(json.dumps(altered))
        rejects('ASTC 1x1 is one full 16-byte block', lambda: verify_manifest(candidate))
        altered = json.loads(json.dumps(manifest))
        old_reference = sampled_reference(old_decode, 'srgb')
        srgb_reference = target / srgb_variant['reference']['path']
        corrected_reference = srgb_reference.read_bytes()
        srgb_reference.write_bytes(old_reference + corrected_reference[len(old_reference):])
        altered['variants'][3]['reference']['sha256'] = file_sha(srgb_reference)
        altered['variants'][3]['mips'][0]['reference_sha256'] = digest(old_reference)
        candidate.write_text(json.dumps(altered))
        rejects('profile-agnostic sRGB reference despite matching hashes', lambda: verify_manifest(candidate))
    return {'success': True, 'variants': 6, 'mips_per_variant': 9, 'negative_cases': rejected,
            'profile_aware_sRGB_regression': True, 'source_unchanged': True, 'GPU_tested': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path, help='Verify an existing candidate; do not rewrite it')
    parser.add_argument('--self-test', type=Path, help='Test an existing candidate and malformed tiny copies')
    args = parser.parse_args()
    if args.verify:
        manifest = verify_manifest(args.verify)
        result = {'success': True, 'manifest': str(args.verify), 'variants': len(manifest['variants'])}
    elif args.self_test:
        result = self_test(args.self_test)
    else:
        path = prepare()
        result = {'success': True, 'manifest': str(path), 'manifest_sha256': file_sha(path),
                  'self_test': self_test(path)}
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
