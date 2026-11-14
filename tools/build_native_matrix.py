"""Route all matrix calls to the checked native leaf, retaining the exact original as fallback."""
import hashlib
import json
import subprocess

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from fastbuild import BIN, INPUTS, ROOT
from test_android_matrix_native import ORIGINAL, ORIGINAL_SHA, OUT as PROOF


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    proof = json.loads((PROOF / 'result.json').read_text())
    assert proof['success'] and proof['device']['full_cpu_and_memory']
    assert proof['device']['native_fp_status'] and proof['original_machine_code_unchanged']
    assert proof['reduction_pct'] > 0 and all(row['native_ns'] < row['original_ns'] for row in proof['benchmark']['pairs'])
    assert proof['original_object_sha256'] == ORIGINAL_SHA == sha(ORIGINAL)
    assert all(sha(ROOT / name) == digest for name, digest in proof['sources'].items()), 'Stale matrix proof'
    symbol = 'L_000140DFA630'
    source = ROOT / 'build/arm64/generated-overlay/recomp_funcs_0800.c'
    text = source.read_text()
    assert text.count(symbol) == 1 and text.lower().count('0x140dfa630') == 1, 'Same-unit matrix call needs explicit routing'
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
    out = ROOT / 'build/fast/native-matrix'
    out.mkdir(parents=True, exist_ok=True)
    renamed = out / ORIGINAL.name
    subprocess.run([str(BIN / 'llvm-objcopy.exe'), '--redefine-sym=' + symbol + '=fp_original_' + symbol,
                    str(ORIGINAL), str(renamed)], check=True)
    with renamed.open('rb') as stream:
        elf = ELFFile(stream)
        assert elf.get_section_by_name('.text').data() == original_text
        assert elf.get_section_by_name('.symtab').get_symbol_by_name('fp_original_' + symbol)
        assert not elf.get_section_by_name('.symtab').get_symbol_by_name(symbol)
    previous, replacement = ORIGINAL.relative_to(ROOT).as_posix(), renamed.relative_to(ROOT).as_posix()
    before = INPUTS.read_bytes()
    data = json.loads(before)
    assert sum(row in (previous, replacement) for row in data['objects']) == 1
    data['objects'] = [replacement if row == previous else row for row in data['objects']]
    native = 'android/host/matrix_native.c'
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
