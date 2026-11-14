"""Route frustum tests to the checked native leaf, retaining the exact original."""
import json
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from fastbuild import BIN, INPUTS, ROOT, sha

ORIGINAL = ROOT / 'build/arm64/eh-overlay-objects/recomp_funcs_0804.o'
ORIGINAL_SHA = 'b4254ad1c258f5d8eca75419ea027f56c5cad0d5b46736f6191a6d5a07643c2a'
PROOF = ROOT / 'build/perf/fps45-20261112/native-frustum'


def main():
    proof = json.loads((PROOF / 'result.json').read_text())
    assert proof['success'] and proof['device']['full_cpu_and_memory'] and proof['device']['native_fp_status']
    assert proof['device']['production_packed_compare'] and proof['original_text_unchanged']
    assert all(row['native_ns'] < row['original_ns'] for row in proof['benchmark']['pairs'])
    assert proof['original_object_sha256'] == ORIGINAL_SHA == sha(ORIGINAL)
    assert all(sha(Path(name)) == digest for name, digest in proof['sources'].items()), 'Stale frustum proof'
    symbol = 'L_000140E2C890'
    source = ROOT / 'build/arm64/generated-overlay/recomp_funcs_0804.c'
    text = source.read_text()
    assert text.count(symbol) == 1 and text.lower().count('0x140e2c890') == 1, 'Same-unit call needs explicit routing'
    with ORIGINAL.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        target = symbols.get_symbol_by_name(symbol)
        assert len(target) == 1 and target[0]['st_info']['bind'] == 'STB_GLOBAL'
        references = []
        for section in elf.iter_sections():
            if isinstance(section, RelocationSection):
                table = elf.get_section(section['sh_link'])
                references += [(section.name, relocation['r_offset']) for relocation in section.iter_relocations()
                               if table.get_symbol(relocation['r_info_sym']).name == symbol]
        assert not references, 'Same-unit reference needs explicit routing'
        original_text = elf.get_section_by_name('.text').data()
    out = ROOT / 'build/fast/native-frustum'
    out.mkdir(parents=True, exist_ok=True)
    renamed = out / ORIGINAL.name
    subprocess.run([str(BIN / 'llvm-objcopy.exe'), '--redefine-sym=' + symbol + '=fp_frustum_original',
                    str(ORIGINAL), str(renamed)], check=True)
    with renamed.open('rb') as stream:
        elf = ELFFile(stream)
        assert elf.get_section_by_name('.text').data() == original_text
        assert elf.get_section_by_name('.symtab').get_symbol_by_name('fp_frustum_original')
        assert not elf.get_section_by_name('.symtab').get_symbol_by_name(symbol)
    previous, replacement = ORIGINAL.relative_to(ROOT).as_posix(), renamed.relative_to(ROOT).as_posix()
    before = INPUTS.read_bytes()
    data = json.loads(before)
    assert sum(row in (previous, replacement) for row in data['objects']) == 1
    data['objects'] = [replacement if row == previous else row for row in data['objects']]
    native = 'android/host/frustum_native.c'
    if native not in data['sources']:
        data['sources'].append(native)
    assert INPUTS.read_bytes() == before, 'Runtime inputs changed concurrently; rerun'
    INPUTS.write_text(json.dumps(data, indent=1) + '\n')
    receipt = dict(original_object_sha256=ORIGINAL_SHA, replacement_object_sha256=sha(renamed),
                   original_text_unchanged=True, same_unit_references=references,
                   lifted_source_sha256=sha(source), native_source_sha256=sha(ROOT / native),
                   proof_sha256=sha(PROOF / 'result.json'), runtime_inputs_sha256=sha(INPUTS))
    (out / 'routing.json').write_text(json.dumps(receipt, indent=1) + '\n')
    print(json.dumps(receipt, indent=1))


if __name__ == '__main__':
    main()
