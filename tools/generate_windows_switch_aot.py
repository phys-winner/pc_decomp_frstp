"""Restore two proven jump-table entries without changing canonical generated C."""
import hashlib
import json
from pathlib import Path
import struct
import pefile
from lift64_checked import CheckedLifter

ROOT=Path(__file__).resolve().parents[1]
config=json.loads((ROOT/'port.json').read_text())
original=Path(config['game_directory'])/'Frostpunk.exe'
original_hash=hashlib.sha256(original.read_bytes()).hexdigest()
assert original_hash==config['original_exe_sha256']
pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
pe.parse_data_directories(directories=[3])
containers=[(base+e.struct.BeginAddress,base+e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION
    if base+e.struct.BeginAddress<=0x140eca00a<base+e.struct.EndAddress]
assert containers==[(0x140ec9f1d,0x140eca02b)]
text=next(s for s in pe.sections if s.Name.rstrip(b'\0')==b'.text')
def read(va,n):
    assert base+text.VirtualAddress<=va and va+n<=base+text.VirtualAddress+text.SizeOfRawData
    return pe.get_data(va-base,n)
table=read(0x140eca040,60)
targets=[base+rva for rva in struct.unpack('<15I',table)]
assert set(targets)=={0x140ec9f6d,0x140eca00a,0x140eca026,0x140ec9ebb,0x140ec9f1d}
lifter=CheckedLifter(image_size=pe.OPTIONAL_HEADER.SizeOfImage,read_va=read,image_base=base,
    original_sha256=original_hash)
bodies=['/* Exact original switch continuations selected by the 15-entry RVA table. */']
entries=[];cases=[];rows=[]
for va,end in [(0x140ec9f6d,0x140eca00a),(0x140eca00a,0x140eca026)]:
    code=read(va,end-va);name=f'switch_case_{va:X}'
    body=lifter.lift_function(code,va,name=name)
    assert 'RECOMP_TODO(' not in body
    bodies.append(body.replace('void '+name+'(', 'static void '+name+'(',1))
    literal='"'+''.join('\\x%02x'%b for b in code)+'"'
    entries.append(f'    {{UINT64_C(0x{va:x}),{len(code)},{literal},{name}}},')
    cases.append(f'    {{UINT64_C(0x{va:x}),UINT64_C(0x140eca026),{len(code)},{literal}}},')
    rows.append(dict(address=hex(va),end=hex(end),size=len(code),original_bytes=code.hex(),
        original_bytes_sha256=hashlib.sha256(code).hexdigest(),lifted_C_sha256=hashlib.sha256(body.encode()).hexdigest()))
bodies+=['#define SWITCH_EXTRA_ENTRIES \\',entries[0]+' \\',entries[1]]
output=ROOT/'native/windows/switch_aot_bodies.inc'
output.write_text('\n\n'.join(bodies[:-3])+'\n\n'+'\n'.join(bodies[-3:])+'\n',encoding='utf-8')
cases_path=ROOT/'tests/windows_switch_aot_cases.h'
cases_path.write_text('static const test_case switch_cases[]={\n'+'\n'.join(cases)+'\n};\n',encoding='ascii')
report=dict(success=True,original_exe_sha256=original_hash,jump_table_va='0x140eca040',
    jump_table_bytes=table.hex(),jump_table_targets=list(map(hex,targets)),missing_entries=rows,
    existing_entries=['0x140eca026','0x140ec9ebb','0x140ec9f1d'],
    pdata_containing_actual_target=['0x140ec9f1d','0x140eca02b'],
    caller_return_offset_at_continuation='0x38',stack_top_is_local_slot=True,
    canonical_modified=False,original_bytes_executed=False,
    producer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    output_sha256=hashlib.sha256(output.read_bytes()).hexdigest())
(ROOT/'research/windows-switch-closure.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['missing_entries','jump_table_bytes']},indent=2))
