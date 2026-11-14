"""Byte-pinned private ASTC format path for a candidate DXVK native source tree.

No BC enum is reused, no archive is converted, and no renderer/device is run.
The game loader integration is a separate byte-gated AOT change.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'reference/dxvk'
REVISION = '0cf05780abd7250c2cd713b7749cf32180157cf5'
PREIMAGES = {
    'src/dxgi/dxgi_format.cpp': '4bbad52a0066a272d591bd04a600e17191aeeffffbf5b7a46e163ce074376246',
    'src/dxgi/dxgi_format.h': 'd74dbadb0cf0da00292979a3ec47c411b4ed081abd0ede3bc7009614eb13c74f',
    'src/dxvk/dxvk_format.cpp': '69b26eb6b85c99d68dbce557a35245256c24378e09fc1b2feb55b573df6b5dda',
    'src/dxvk/dxvk_format.h': '9bd447222b806ccb014c45816eb12e7fc0a5bcf1b2d9d836e1d1bdd7e159b33b',
    'src/d3d11/d3d11_texture.cpp': 'a474d6f71f43e0828541fd9652b73ac82a57c34dfa15de4cd61c35590294352b',
}
NEW_HEADER = 'src/dxgi/dxgi_astc_formats.h'
FILES = (*PREIMAGES, NEW_HEADER)
FORMATS = [
    {'block': 4, 'D3DFMT': 0x34545341, 'DXGI_typeless': 0x7f004400, 'DXGI_unorm': 0x7f004401,
     'DXGI_srgb': 0x7f004402, 'Vk_unorm': 157, 'Vk_srgb': 158},
    {'block': 6, 'D3DFMT': 0x36545341, 'DXGI_typeless': 0x7f006600, 'DXGI_unorm': 0x7f006601,
     'DXGI_srgb': 0x7f006602, 'Vk_unorm': 165, 'Vk_srgb': 166},
]

HEADER = '''#pragma once
#include <cstdint>
#include <dxgiformat.h>

namespace dxvk {
  // Private resource ABI. These are not standard D3D or DXGI enum values.
  constexpr uint32_t FP_D3DFMT_ASTC_4X4 = 0x34545341u; // FOURCC AST4
  constexpr uint32_t FP_D3DFMT_ASTC_6X6 = 0x36545341u; // FOURCC AST6
  constexpr DXGI_FORMAT FP_DXGI_FORMAT_ASTC_4X4_TYPELESS = DXGI_FORMAT(0x7f004400u);
  constexpr DXGI_FORMAT FP_DXGI_FORMAT_ASTC_4X4_UNORM    = DXGI_FORMAT(0x7f004401u);
  constexpr DXGI_FORMAT FP_DXGI_FORMAT_ASTC_4X4_SRGB     = DXGI_FORMAT(0x7f004402u);
  constexpr DXGI_FORMAT FP_DXGI_FORMAT_ASTC_6X6_TYPELESS = DXGI_FORMAT(0x7f006600u);
  constexpr DXGI_FORMAT FP_DXGI_FORMAT_ASTC_6X6_UNORM    = DXGI_FORMAT(0x7f006601u);
  constexpr DXGI_FORMAT FP_DXGI_FORMAT_ASTC_6X6_SRGB     = DXGI_FORMAT(0x7f006602u);

  inline int fpAstcFormatIndex(DXGI_FORMAT format) {
    uint32_t f = uint32_t(format);
    if (f - 0x7f004400u < 3) return int(f - 0x7f004400u);
    if (f - 0x7f006600u < 3) return int(f - 0x7f006600u) + 3;
    return -1;
  }

  inline bool fpAstcViewCompatible(DXGI_FORMAT base, DXGI_FORMAT view) {
    int b = fpAstcFormatIndex(base), v = fpAstcFormatIndex(view);
    if (b < 0 && v < 0) return true; // Existing formats keep upstream validation.
    if (b < 0 || v < 0 || b / 3 != v / 3 || v % 3 == 0) return false;
    return b % 3 == 0 || b == v; // Typeless resource; typed view, same block family.
  }

  inline bool fpAstcFromD3DFormat(uint32_t format, bool srgb, DXGI_FORMAT* result) {
    if (!result) return false;
    if (format == FP_D3DFMT_ASTC_4X4) {
      *result = srgb ? FP_DXGI_FORMAT_ASTC_4X4_SRGB : FP_DXGI_FORMAT_ASTC_4X4_UNORM;
      return true;
    }
    if (format == FP_D3DFMT_ASTC_6X6) {
      *result = srgb ? FP_DXGI_FORMAT_ASTC_6X6_SRGB : FP_DXGI_FORMAT_ASTC_6X6_UNORM;
      return true;
    }
    return false;
  }

  inline bool fpAstcMipLayout(DXGI_FORMAT format, uint32_t width, uint32_t height,
                              uint32_t* rowPitch, uint32_t* mipBytes) {
    int index = fpAstcFormatIndex(format);
    if (index < 0 || !width || !height || !rowPitch || !mipBytes) return false;
    uint32_t block = index < 3 ? 4 : 6;
    uint64_t row = ((uint64_t(width) + block - 1) / block) * 16;
    if (row > UINT32_MAX) return false;
    uint64_t bytes = row * ((uint64_t(height) + block - 1) / block);
    if (bytes > UINT32_MAX) return false;
    *rowPitch = uint32_t(row); *mipBytes = uint32_t(bytes);
    return true;
  }
}
'''

MAPPINGS = '''  // Private ASTC ABI: typeless storage defaults to UNORM, never RAW/depth.
  static const std::array<DXGI_VK_FORMAT_MAPPING, 6> fp_astcMappings = {{
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK },
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_ASTC_4x4_SRGB_BLOCK,  VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK },
    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
    { VK_FORMAT_ASTC_6x6_SRGB_BLOCK,  VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT },
  }};

  static const std::array<DXGI_VK_FORMAT_FAMILY, 6> fp_astcFamilies = {{
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK },
    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK },
    { VK_FORMAT_ASTC_4x4_SRGB_BLOCK },
    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK },
    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK },
    { VK_FORMAT_ASTC_6x6_SRGB_BLOCK },
  }};

'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def apply(work, source=None):
    work, source = Path(work).resolve(), Path(source or work).resolve()
    if work.is_relative_to((ROOT / 'reference').resolve()):
        raise ValueError('Never overlay pristine reference inputs')
    contents = {}
    for name, expected in PREIMAGES.items():
        data = (source / name).read_bytes()
        if sha(data) != expected:
            raise ValueError('ASTC overlay preimage mismatch: ' + name)
        contents[name] = data.decode('utf-8').replace('\r\n', '\n')

    def change(name, before, after, count=1):
        if contents[name].count(before) != count:
            raise ValueError('ASTC overlay anchor mismatch: ' + name)
        contents[name] = contents[name].replace(before, after)

    change('src/dxgi/dxgi_format.h', '#include "dxgi_include.h"',
           '#include "dxgi_include.h"\n#include "dxgi_astc_formats.h"')
    name = 'src/dxgi/dxgi_format.cpp'
    change(name, 'namespace dxvk {\n', 'namespace dxvk {\n\n' + MAPPINGS)
    before = '    const size_t formatId = size_t(Format);\n'
    # The first occurrence is GetFormatFamily; following two are mapping lookups.
    if contents[name].count(before) != 3:
        raise ValueError('ASTC format lookup preimage count differs')
    for method, action in [
        ('DXGI_VK_FORMAT_FAMILY DXGIVkFormatTable::GetFormatFamily(', 'return fp_astcFamilies[astc];'),
        ('const DXGI_VK_FORMAT_MAPPING* DXGIVkFormatTable::GetFormatMapping(', 'return &fp_astcMappings[astc];'),
        ('const DXGI_VK_FORMAT_MAPPING* DXGIVkFormatTable::GetPackedFormatMapping(', 'return &fp_astcMappings[astc];')]:
        position = contents[name].index(method)
        position = contents[name].index(before, position)
        contents[name] = contents[name][:position] + '    int astc = fpAstcFormatIndex(Format);\n    if (astc >= 0) ' + action + '\n\n' + contents[name][position:]

    name = 'src/dxvk/dxvk_format.cpp'
    entries = ''
    for block in (4, 6):
        for color in ('UNORM', 'SRGB'):
            flags = 'DxvkFormatFlag::BlockCompressed' if color == 'UNORM' else 'DxvkFormatFlags(DxvkFormatFlag::BlockCompressed, DxvkFormatFlag::ColorSpaceSrgb)'
            entries += f'\n    // VK_FORMAT_ASTC_{block}x{block}_{color}_BLOCK\n    {{ 16, RGBA, VK_IMAGE_ASPECT_COLOR_BIT, {flags}, VkExtent3D {{ {block}, {block}, 1 }} }},\n'
    change(name, '    // VK_FORMAT_A8_UNORM_KHR\n    { 1, A, VK_IMAGE_ASPECT_COLOR_BIT },\n',
           '    // VK_FORMAT_A8_UNORM_KHR\n    { 1, A, VK_IMAGE_ASPECT_COLOR_BIT },\n' + entries)
    change(name, 'const std::array<std::pair<VkFormat, VkFormat>, 5> g_formatGroups',
           'const std::array<std::pair<VkFormat, VkFormat>, 7> g_formatGroups')
    anchor = '    { VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR,  VK_FORMAT_A8_UNORM_KHR              },\n'
    change(name, anchor, anchor + '    { VK_FORMAT_ASTC_4x4_UNORM_BLOCK,      VK_FORMAT_ASTC_4x4_SRGB_BLOCK       },\n    { VK_FORMAT_ASTC_6x6_UNORM_BLOCK,      VK_FORMAT_ASTC_6x6_SRGB_BLOCK       },\n')
    change('src/dxvk/dxvk_format.h', 'constexpr size_t DxvkFormatCount = 155;', 'constexpr size_t DxvkFormatCount = 159;')

    name = 'src/d3d11/d3d11_texture.cpp'
    before = '    DXGI_VK_FORMAT_MODE   formatMode   = GetFormatMode();\n'
    change(name, before, '''    if (fpAstcFormatIndex(m_desc.Format) >= 0
     && (Dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || m_desc.Depth != 1
      || m_desc.SampleDesc.Count != 1 || m_desc.SampleDesc.Quality != 0
      || (m_desc.Usage != D3D11_USAGE_DEFAULT && m_desc.Usage != D3D11_USAGE_IMMUTABLE)
      || m_desc.CPUAccessFlags || (m_desc.BindFlags & ~D3D11_BIND_SHADER_RESOURCE)
      || (m_desc.MiscFlags & ~D3D11_RESOURCE_MISC_TEXTURECUBE)
      || ((m_desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
       && (m_desc.Width != m_desc.Height || m_desc.ArraySize % 6))))
      throw DxvkError("D3D11: private ASTC requires a non-MSAA sampled 2D/array/cube resource");

''' + before)
    before = '    // Check whether the view format is compatible\n'
    change(name, before, '''    if (!fpAstcViewCompatible(m_desc.Format, Format))
      return false;

''' + before)
    contents[NEW_HEADER] = HEADER
    # All inputs/anchors are validated before any output is changed.
    for name, text in contents.items():
        path = work / name
        data = text.encode('utf-8')
        if not path.exists() or path.read_bytes() != data:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    return {name: sha(text.encode('utf-8')) for name, text in contents.items()}


def subresource_layout(width, height, mips, layers, block, *, cube=False, payload_bytes=None):
    """Candidate flat upload layout; canonical archive layout is separately proven."""
    values = (width, height, mips, layers, block)
    if not all(isinstance(v, int) and not isinstance(v, bool) and 0 < v <= 0xffffffff for v in values):
        raise ValueError('Invalid dimensions/mips/layers')
    if max(width, height) > 16384 or layers > 2048:
        raise ValueError('D3D11 texture dimension/array limit exceeded')
    if block not in (4, 6) or mips > max(width, height).bit_length():
        raise ValueError('Invalid block or mip count')
    if cube and (width != height or layers % 6):
        raise ValueError('Invalid cube shape/layer count')
    # All allocation lengths remain representable even before enumerating layers.
    per_layer = sum(((max(1, width >> mip) + block - 1) // block) *
                    ((max(1, height >> mip) + block - 1) // block) * 16 for mip in range(mips))
    if per_layer > 0xffffffff or per_layer * layers > 0xffffffffffffffff:
        raise ValueError('Upload allocation length overflow')
    if payload_bytes is not None and payload_bytes != per_layer * layers:
        raise ValueError('Upload allocation length differs')
    result = []
    offset = 0
    for layer in range(layers):
        for mip in range(mips):
            w, h = max(1, width >> mip), max(1, height >> mip)
            row = ((w + block - 1) // block) * 16
            size = row * ((h + block - 1) // block)
            result.append({'layer': layer, 'mip': mip, 'index': mip + layer * mips,
                           'width': w, 'height': h, 'offset': offset, 'row_pitch': row, 'bytes': size})
            offset += size
    return result


def game_evidence():
    """Read only pinned EXE plus the 18 cube entries; no original execution."""
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from prepare_texture_fixture import source_entry, RECORD, ENTRY_SHA256
    config = json.loads((ROOT / 'port.json').read_text())
    original = Path(config['game_directory']) / 'Frostpunk.exe'
    with original.open('rb') as stream:
        original_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
    if original_hash != '45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc':
        raise ValueError('Original PE identity differs')
    pe = pefile.PE(str(original), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    gates = []
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    for va, length, expected, meaning in [
        (0x140d95650, 0x288, '98e6de09a0a6aa5421cf797b3bb87386e071a74a7b126f3904b8e0058ecf6265', 'D3DFMT->DXGI function and both switch tables'),
        (0x140d931a8, 29, '3b9f58781d89268684d1d423c06995559076ce9f1ce87c0b316da23985c219fa', 'height=-1 selects six layers, square height, TEXTURECUBE misc flag'),
        (0x140d932e1, 57, '8e1399648c723e3e39ec232d0fb1a149909ea222df6d58bbf9a2e0ae9ad2940c', 'cumulative canonical mip offsets advance by size*layerCount'),
        (0x140d93360, 126, 'ab6bfb89e9c751605cd1adf0856878a7f2a6404f6f6cdf0eb6cb814f06d6e117', 'reverse mip index, unchanged face index, initialData[face*mipCount+mip]'),
        (0x140f499a0, 532, '08f94fef7261783643255e6d4a4e6eb2739f6817144c00ba892b97aea5905c98', 'D3DFMT pitch/size helper body; must gain explicit private ASTC cases')]:
        data = pe.get_data(va - base, length)
        if len(data) != length or sha(data) != expected:
            raise ValueError('Original AOT byte gate differs: ' + hex(va))
        code = data[:0x18e] if va == 0x140d95650 else data
        gates.append({'VA': hex(va), 'bytes': length, 'sha256': expected, 'original_bytes': data.hex(),
                      'meaning': meaning, 'decode': [f'{i.address:x}: {i.mnemonic} {i.op_str}' for i in decoder.disasm(code, va)]})
    # Reuse sealed archive/index/selected-entry verification; the 18GB data file
    # is not rehashed here. Every cube entry below is read and individually hashed.
    _, source, snapshots = source_entry()
    archive_dir = ROOT / 'build/raw-archives/textures-s3'
    index_path, data_path = archive_dir / 'textures-s3.idx', archive_dir / 'textures-s3.dat'
    receipt = json.loads((ROOT / source['conversion_receipt']).read_text())['archives'][0]
    rows = list(RECORD.iter_unpack(index_path.read_bytes()[11:]))
    cubes = []
    with data_path.open('rb') as stream:
        for entry_index, (key, stored, decoded, offset, compressed) in enumerate(rows):
            stream.seek(offset)
            header = stream.read(min(stored, 144))
            if len(header) < 16 or struct.unpack_from('<I', header, 4)[0] != 0xffffffff:
                continue
            if compressed or stored != decoded:
                raise ValueError('Unexpected compressed cube source')
            stream.seek(offset)
            entry = stream.read(stored)
            if sha(entry) != receipt['decoded_content_sha256_by_entry'][entry_index]:
                raise ValueError('Cube decoded entry proof differs')
            width, _, fmt, count = struct.unpack_from('<4I', entry)
            if fmt not in (21, 0x35545844) or not 0 < count <= 16 or len(entry) < 144:
                raise ValueError('Cube source header differs')
            mip_sizes, mip_dimensions = [], []
            for n in range(count):
                w, h, size = struct.unpack_from('<HHI', entry, 16 + n * 8)
                expected = w * h * 4 if fmt == 21 else ((w + 3) // 4) * ((h + 3) // 4) * 16
                if w != h or not w or size != expected or (n and w != mip_dimensions[-1] * 2):
                    raise ValueError('Cube mip descriptor differs')
                mip_sizes.append(size);mip_dimensions.append(w)
            if mip_dimensions[-1] != width or stored != 144 + 6 * sum(mip_sizes):
                raise ValueError('Cube payload exact length/geometry differs')
            subresources = []
            prefix = 144
            for canonical_index, size in enumerate(mip_sizes):
                for face, axis in enumerate(('+X', '-X', '+Y', '-Y', '+Z', '-Z')):
                    start = prefix + face * size
                    subresources.append({'canonical_mip': canonical_index, 'D3D_mip': count - 1 - canonical_index,
                                         'face': face, 'axis': axis, 'D3D_subresource': face * count + count - 1 - canonical_index,
                                         'entry_offset': start, 'bytes': size, 'sha256': sha(entry[start:start + size])})
                prefix += size * 6
            cubes.append({'key': key, 'raw_archive_offset': offset, 'entry_bytes': stored,
                          'entry_sha256_actual': sha(entry), 'source_format': fmt,
                          'width': width, 'mip_count': count, 'subresources': subresources})
    if len(cubes) != 18 or sum(c['source_format'] == 0x35545844 for c in cubes) != 11:
        raise ValueError('Cube inventory differs')
    if any((p.stat().st_size, p.stat().st_mtime_ns) != stamp for p, stamp in snapshots.items()):
        raise ValueError('Archive changed while inspecting cubes')
    return {'original_exe_sha256': original_hash, 'byte_gates': gates,
            'mapping_function': '0x140D95650', 'mapping_inputs': 'ECX=D3DFMT, DL=bool sRGB; EAX=DXGI_FORMAT',
            'original_mappings': {'DXT1': [71, 72], 'DXT3': [74, 75], 'DXT5': [77, 78], 'D3DFMT21': [87, 91]},
            'new_format_size_helper_required': '0x140F499A0(D3DFMT,width,height,outBytes,outRowPitch)',
            'private_format_helpers': NEW_HEADER + ': fpAstcFromD3DFormat, fpAstcMipLayout',
            'canonical_cube_layout': '144-byte header; mip-major smallest->largest; all six faces per mip; each face unchanged',
            'cube_source_pointer': 'entry+144+6*sum(previousCanonicalMipBytes)+face*currentMipBytes',
            'D3D11_initialData_index': 'face*mipCount+(mipCount-1-canonicalMip)',
            'face_order_proof': 'Original loader assigns source face index directly to D3D11 cube array layer; no swizzle/flip/copy',
            'face_enum_source': 'https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_texturecube_face',
            'source_archive': source, 'cube_count': 18, 'cubes': cubes,
            'cube_order_proven_statically': True, 'original_executed': False, 'whole_archive_converted': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate-dir', type=Path, default=ROOT / 'build/dxvk-astc-candidate')
    parser.add_argument('--game-evidence', action='store_true', help='Also verify original loader bytes and the 18 cube entries')
    args = parser.parse_args()
    outputs = apply(args.candidate_dir, SOURCE)
    manifest = {'schema_version': 1, 'revision': REVISION, 'producer_sha256': sha(Path(__file__).read_bytes()),
                'preimages': PREIMAGES, 'outputs': outputs, 'private_format_ABI': FORMATS,
                'block_bytes': 16, 'mip_extent': 'max(1, base_dimension >> mip)',
                'row_pitch': 'ceil(width/block_width)*16', 'slice_pitch': 'row_pitch*ceil(height/block_height)',
                'D3D11_subresource_index': 'mip + array_layer*mip_count',
                'cube_layers': '6 per cube; orientation/order must be proven in original loader before archive conversion',
                'supported_scope': 'sampled non-MSAA 2D, 2D arrays and cube/cube arrays; no CPU/RT/DS/UAV/shared/tiled/autogen',
                'typeless_scope': 'typed UNORM/sRGB views within exact block family; typed resource requires identical typed view',
                'original_BC_mapping_changed': False, 'original_resource_data_changed': False,
                'AOT_loader_integrated': False, 'GPU_executed': False}
    path = args.candidate_dir / 'format-manifest.json'
    path.write_text(json.dumps(manifest, indent=2) + '\n')
    if args.game_evidence:
        evidence = game_evidence()
        (args.candidate_dir / 'game-loader-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps({'success': True, 'manifest': str(path), 'manifest_sha256': sha(path.read_bytes()), 'files': len(outputs)}, indent=2))


if __name__ == '__main__':
    main()
