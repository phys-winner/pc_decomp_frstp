"""Route fastlift unit 0982's CRC calls to android/host/crc_native.c.

The published runtime is the fastlift objects (about 130 MB stripped). Relinking
the overlay objects instead is what doubled that library. This step patches a
copy of build/fastlift/obj/recomp_funcs_0982.o: the body is renamed to
fp_original_L_000141C04BB0 and every same-unit CALL26/JUMP26 that named
L_000141C04BB0 is pointed at a new undefined symbol of that name. The dispatch
table already references that name, so both bind to the wrapper. .text is
unchanged. The wrapper is compiled with -march=armv8-a+crc, which the shared
fastbuild flags do not pass.
"""
import hashlib
import json
import shutil
import struct
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from fastbuild import BIN, CLANG, COMPILE_FLAGS, INCLUDES, INPUTS, ROOT

SYMBOL = 'L_000141C04BB0'
ORIGINAL = 'fp_original_' + SYMBOL
SOURCE_UNIT = ROOT / 'build/fastlift/obj/recomp_funcs_0982.o'
OUT = ROOT / 'build/fast/native-crc/fastlift'
ROUTED_UNIT = OUT / 'recomp_funcs_0982.o'
NATIVE_OBJECT = ROOT / 'build/fast/native-crc/crc_native.o'
NATIVE_SOURCE = ROOT / 'android/host/crc_native.c'
RECEIPT = OUT / 'routing.json'
# R_AARCH64_JUMP26, R_AARCH64_CALL26. An ADRP/ADD of this symbol would be a
# different contract and must fail the route instead of being retargeted blindly.
CALL_TYPES = {282, 283}
SHN_ABS = 0xFFF1
SHN_UNDEF = 0


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def text_of(path):
    with Path(path).open('rb') as stream:
        return ELFFile(stream).get_section_by_name('.text').data()


def symbol_named(path, name):
    with Path(path).open('rb') as stream:
        table = ELFFile(stream).get_section_by_name('.symtab')
        found = table.get_symbol_by_name(name)
    if len(found) != 1:
        raise RuntimeError('%s has %d symbols named %s' % (path, len(found), name))
    return found[0]


def ensure_routed():
    """Write the routed unit and the +crc wrapper object. Both are content-addressed."""
    if not SOURCE_UNIT.is_file():
        raise FileNotFoundError(SOURCE_UNIT)
    if not NATIVE_SOURCE.is_file():
        raise FileNotFoundError(NATIVE_SOURCE)
    source_text = hashlib.sha256(text_of(SOURCE_UNIT)).hexdigest()
    source_c = sha(NATIVE_SOURCE)
    if ROUTED_UNIT.is_file() and NATIVE_OBJECT.is_file() and RECEIPT.is_file():
        previous = json.loads(RECEIPT.read_text())
        if (previous.get('source_text_sha256') == source_text
                and previous.get('native_source_sha256') == source_c
                and previous.get('routed_object_sha256') == sha(ROUTED_UNIT)
                and previous.get('native_object_sha256') == sha(NATIVE_OBJECT)):
            return previous
    body = symbol_named(SOURCE_UNIT, SYMBOL)
    if body['st_info']['bind'] != 'STB_GLOBAL' or body['st_info']['type'] != 'STT_FUNC':
        raise RuntimeError('CRC symbol is not a global function')
    body_lo, body_hi = body['st_value'], body['st_value'] + body['st_size']
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(BIN / 'llvm-objcopy.exe'),
                    '--redefine-sym=' + SYMBOL + '=' + ORIGINAL,
                    '--add-symbol=' + SYMBOL + '=0,global,function',
                    str(SOURCE_UNIT), str(ROUTED_UNIT)], check=True)
    if text_of(ROUTED_UNIT) != text_of(SOURCE_UNIT):
        raise RuntimeError('objcopy changed .text of unit 0982')
    data = bytearray(ROUTED_UNIT.read_bytes())
    with ROUTED_UNIT.open('rb') as stream:
        elf = ELFFile(stream)
        symtab = elf.get_section_by_name('.symtab')
        if symtab['sh_entsize'] != 24:
            raise RuntimeError('unexpected symbol entry size')
        wanted = original_index = None
        for index, symbol in enumerate(symtab.iter_symbols()):
            if symbol.name == SYMBOL:
                wanted = index
            elif symbol.name == ORIGINAL:
                original_index = index
        if wanted is None or original_index is None:
            raise RuntimeError('routed object is missing a CRC symbol')
        entry = symtab.get_symbol(wanted)
        if entry['st_shndx'] != 'SHN_ABS' or entry['st_value'] != 0:
            raise RuntimeError('added symbol is not the absolute placeholder')
        sym_off = symtab['sh_offset'] + wanted * 24
        if struct.unpack_from('<H', data, sym_off + 6)[0] != SHN_ABS:
            raise RuntimeError('symbol shndx mismatch')
        struct.pack_into('<H', data, sym_off + 6, SHN_UNDEF)
        struct.pack_into('<Q', data, sym_off + 8, 0)
        struct.pack_into('<Q', data, sym_off + 16, 0)
        patched = []
        for section in elf.iter_sections():
            if not isinstance(section, RelocationSection):
                continue
            table = elf.get_section(section['sh_link'])
            for index, relocation in enumerate(section.iter_relocations()):
                name = table.get_symbol(relocation['r_info_sym']).name
                if name != ORIGINAL:
                    continue
                if section.name != '.rela.text':
                    raise RuntimeError('CRC reference outside .rela.text')
                if body_lo <= relocation['r_offset'] < body_hi:
                    raise RuntimeError('CRC body references itself at %#x' % relocation['r_offset'])
                typ = relocation['r_info_type']
                if typ not in CALL_TYPES:
                    raise RuntimeError('CRC reloc type %s at %#x' % (typ, relocation['r_offset']))
                info_off = section['sh_offset'] + index * section['sh_entsize'] + 8
                info = struct.unpack_from('<Q', data, info_off)[0]
                if (info & 0xFFFFFFFF) != typ:
                    raise RuntimeError('reloc type mismatch at %#x' % relocation['r_offset'])
                struct.pack_into('<Q', data, info_off, (wanted << 32) | typ)
                patched.append(relocation['r_offset'])
        if len(patched) < 2:
            raise RuntimeError('expected the CRC call sites, patched %d' % len(patched))
    ROUTED_UNIT.write_bytes(data)
    if text_of(ROUTED_UNIT) != text_of(SOURCE_UNIT):
        raise RuntimeError('reloc patch changed .text')
    undefined = symbol_named(ROUTED_UNIT, SYMBOL)
    renamed = symbol_named(ROUTED_UNIT, ORIGINAL)
    if undefined['st_shndx'] != 'SHN_UNDEF':
        raise RuntimeError('wrapper symbol is not undefined')
    if renamed['st_shndx'] == 'SHN_UNDEF' or renamed['st_value'] != body['st_value'] or renamed['st_size'] != body['st_size']:
        raise RuntimeError('original CRC body was not preserved')
    if symbol_named(ROUTED_UNIT, 'L_000141C05470')['st_shndx'] == 'SHN_UNDEF':
        raise RuntimeError('inflate symbol was disturbed')
    with ROUTED_UNIT.open('rb') as stream:
        elf = ELFFile(stream)
        for section in elf.iter_sections():
            if not isinstance(section, RelocationSection):
                continue
            table = elf.get_section(section['sh_link'])
            for relocation in section.iter_relocations():
                if table.get_symbol(relocation['r_info_sym']).name == ORIGINAL:
                    raise RuntimeError('a CRC reloc still names the original body')
    NATIVE_OBJECT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(CLANG), *COMPILE_FLAGS, '-march=armv8-a+crc',
                    *['-I' + str(ROOT / item) for item in INCLUDES],
                    '-c', str(NATIVE_SOURCE), '-o', str(NATIVE_OBJECT)], check=True)
    receipt = dict(source_unit=str(SOURCE_UNIT.relative_to(ROOT)).replace('\\', '/'),
                   source_text_sha256=source_text, source_body_value=body['st_value'],
                   source_body_size=body['st_size'], call_sites=len(patched),
                   routed_object_sha256=sha(ROUTED_UNIT), native_source_sha256=source_c,
                   native_object_sha256=sha(NATIVE_OBJECT))
    RECEIPT.write_text(json.dumps(receipt, indent=1) + '\n')
    return receipt


def main():
    receipt = ensure_routed()
    snapshot = ROOT / 'build/fast/native-crc/runtime-inputs.with-inflate.json'
    if not snapshot.exists():
        shutil.copyfile(INPUTS, snapshot)
    data = json.loads(INPUTS.read_text())
    replacement = ROUTED_UNIT.relative_to(ROOT).as_posix()
    native = NATIVE_OBJECT.relative_to(ROOT).as_posix()
    rows = []
    seen = 0
    for row in data['objects']:
        normal = row.replace('\\', '/')
        if normal == 'build/fast/native-inflate/inflate_native.o':
            continue
        name = Path(normal).name
        if name == 'recomp_funcs_0982.o' and not normal.startswith('generated/'):
            seen += 1
            rows.append(replacement)
            continue
        rows.append(normal)
    if seen != 1:
        raise SystemExit('unit 0982 is not a single runtime object')
    if native not in rows:
        rows.append(native)
    if 'android/host/crc_native.c' in data['sources'] or 'android/host/inflate_native.c' in data['sources']:
        raise SystemExit('CRC/inflate sources would be compiled again without the per-file flags')
    data['objects'] = rows
    INPUTS.write_text(json.dumps(data, indent=1) + '\n')
    receipt = dict(receipt, runtime_inputs_sha256=sha(INPUTS), unit_row=replacement, native_object=native,
                   inflate_object_omitted=True)
    print(json.dumps(receipt, indent=1))


if __name__ == '__main__':
    main()
