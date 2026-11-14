"""Route bounds calls to the checked native transform, retaining the accepted original."""
import json,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from fastbuild import BIN,INPUTS,ROOT,sha
from test_android_bounds_native import ORIGINAL,ORIGINAL_SHA,OUT as PROOF
OUT=ROOT/'build/fast/native-bounds'
LEGACY='build/fast/native-frustum/recomp_funcs_0804.o'
LEGACY_SHA='7f3da44314c9a30bce9066eabd7b9e10e7aa82089074191bf9bf8b594980eb14'
def main():
 proof=json.loads((PROOF/'result.json').read_text());assert proof['success']
 assert proof['device']['full_cpu_and_memory'] and proof['device']['native_fp_status'] and proof['device']['fast_cases']>1000
 assert proof['original_object_sha256']==ORIGINAL_SHA==sha(ORIGINAL)
 assert all(row['native_ns']<row['original_ns'] for row in proof['benchmark']['pairs'])
 assert all(sha(Path(p))==h for p,h in proof['sources'].items()),'Stale bounds proof'
 symbol='L_000140E2C200';source=ROOT/'build/arm64/generated-overlay/recomp_funcs_0804.c'
 assert source.read_text().count(symbol)==1,'Same-unit bounds call needs explicit routing'
 with ORIGINAL.open('rb') as f:
  elf=ELFFile(f);original_text=elf.get_section_by_name('.text').data();refs=[]
  for section in elf.iter_sections():
   if isinstance(section,RelocationSection):
    table=elf.get_section(section['sh_link'])
    refs.extend(r['r_offset'] for r in section.iter_relocations() if table.get_symbol(r['r_info_sym']).name==symbol)
  assert not refs,'Same-unit bounds reference needs explicit routing'
 before=INPUTS.read_bytes();data=json.loads(before);OUT.mkdir(parents=True,exist_ok=True)
 renamed=OUT/'recomp_funcs_0804.o';replacement=renamed.relative_to(ROOT).as_posix();previous=ORIGINAL.relative_to(ROOT).as_posix()
 selected=[p for p in data['objects'] if p in (previous,LEGACY,replacement)];assert len(selected)==1,'Run the accepted frustum route before bounds'
 if selected[0]==LEGACY:assert sha(ROOT/LEGACY)==LEGACY_SHA,'Legacy frustum input changed'
 # The packed-FP/flags-qualified0804 is the fallback, preserving the accepted runtime contract.
 subprocess.run([str(BIN/'llvm-objcopy.exe'),'--redefine-sym='+symbol+'=fp_bounds_original',str(ORIGINAL),str(renamed)],check=True)
 with renamed.open('rb') as f:
  elf=ELFFile(f);assert elf.get_section_by_name('.text').data()==original_text
  assert elf.get_section_by_name('.symtab').get_symbol_by_name('fp_bounds_original')
  assert not elf.get_section_by_name('.symtab').get_symbol_by_name(symbol)
 data['objects']=[replacement if p==selected[0] else p for p in data['objects']]
 native='android/host/bounds_native.c'
 if native not in data['sources']:data['sources'].append(native)
 assert INPUTS.read_bytes()==before,'Runtime inputs changed concurrently; rerun'
 INPUTS.write_text(json.dumps(data,indent=1)+'\n')
 receipt=dict(success=True,original_object_sha256=ORIGINAL_SHA,replacement_object_sha256=sha(renamed),original_text_unchanged=True,same_unit_references=refs,native_source_sha256=sha(ROOT/native),proof_sha256=sha(PROOF/'result.json'),runtime_inputs_sha256=sha(INPUTS))
 (OUT/'routing.json').write_text(json.dumps(receipt,indent=1)+'\n');print(json.dumps(receipt,indent=1))
if __name__=='__main__':main()
