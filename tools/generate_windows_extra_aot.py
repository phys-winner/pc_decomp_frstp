import argparse,hashlib,json,sys
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from lift64_checked import CheckedLifter

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
parser=argparse.ArgumentParser();parser.add_argument('--candidate-dir',type=Path);args=parser.parse_args()
out=args.candidate_dir or ROOT/'native/windows';out.mkdir(parents=True,exist_ok=True)
test_cases=(args.candidate_dir or ROOT/'tests')/'windows_extra_aot_cases.h'
report_path=ROOT/'research/windows-omitted-ret-leaves.json'
assert sha(report_path)=='7237e37547315cd02b9837569215de2bcd6e642ee17c57926a921f7d5f6ce9d9'
report=json.loads(report_path.read_text());rows=report['candidates'];config=json.loads((ROOT/'port.json').read_text())
switch_report=json.loads((ROOT/'research/windows-switch-closure.json').read_text())
assert switch_report['original_exe_sha256']==config['original_exe_sha256']
assert sha(ROOT/'native/windows/switch_aot_bodies.inc')==switch_report['output_sha256']
original=Path(config['game_directory'])/'Frostpunk.exe'
assert sha(original)==config['original_exe_sha256']==report['original_exe_sha256']
assert len(rows)==12 and sum(r['size'] for r in rows)==160
pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
text=next(s for s in pe.sections if s.Name.rstrip(b'\0')==b'.text')
def read(va,n):
    if not base+text.VirtualAddress<=va or va+n>base+text.VirtualAddress+text.SizeOfRawData:return b''
    return pe.get_data(va-base,n)
lifter=CheckedLifter(image_size=pe.OPTIONAL_HEADER.SizeOfImage,read_va=read,image_base=base,
    original_sha256=config['original_exe_sha256'])
bodies=['/* Exact separate static lift of the 12 approved RET-adjacent leaves. */',
    '#include "switch_aot_bodies.inc"']
extension_report_path=(args.candidate_dir or ROOT/'research')/'windows-switch-extension.json'
extension_report=json.loads(extension_report_path.read_text()) if extension_report_path.exists() else None
if extension_report:
    assert extension_report['success'] and extension_report['count']==5
    assert extension_report['original_exe_sha256']==config['original_exe_sha256']
    assert sha(ROOT/'tools/generate_windows_switch_extension.py')==extension_report['producer_sha256']
    assert all(sha(ROOT/name)==expected for name,expected in extension_report['canonical_sources'].items())
    assert sha(out/'switch_extension_bodies.inc')==extension_report['outputs']['switch_extension_bodies.inc']
    bodies.append('#include "switch_extension_bodies.inc"')
entries=[];cases=[]
for row in rows:
    va=int(row['address'],16);code=bytes.fromhex(row['original_bytes'])
    assert read(va,len(code))==code
    assert hashlib.sha256(code).hexdigest()==row['original_bytes_sha256']
    name=f'ret_leaf_{va:X}';body=lifter.lift_function(code,va,name=name)
    assert 'RECOMP_TODO(' not in body
    assert hashlib.sha256(body.encode()).hexdigest()==row['lifted_C_sha256'],hex(va)
    bodies.append(body.replace('void '+name+'(', 'static void '+name+'(',1))
    literal='"'+''.join('\\x%02x'%b for b in code)+'"'
    entries.append(f'    {{UINT64_C(0x{va:x}),{len(code)},{literal},{name}}},')
    cases.append(f'    {{UINT64_C(0x{va:x}),UINT64_C({row["terminal"]["target"]}),{len(code)},{literal}}},')
callback_va=0x141947e84
callback_code=bytes.fromhex('488b01ffa008010000')
assert read(callback_va,len(callback_code))==callback_code
assert read(callback_va-3,3)==read(callback_va+len(callback_code),3)==b'\xcc'*3
decoder=Cs(CS_ARCH_X86,CS_MODE_64)
assert [(i.mnemonic,i.op_str) for i in decoder.disasm(callback_code,callback_va)]==[
    ('mov','rax, qword ptr [rcx]'),('jmp','qword ptr [rax + 0x108]')]
callback_refs=[0x14194820e,0x1419484d2,0x1419bc2ad,0x1419dd2cb,0x1419df049,0x141a2c39c,0x141a4294a]
callback_witnesses=[]
for address in callback_refs:
    ref=read(address,7);decoded=list(decoder.disasm(ref,address))
    assert len(decoded)==1 and decoded[0].mnemonic=='lea' and decoded[0].op_str.startswith('r9, [rip ')
    assert address+7+int.from_bytes(ref[3:],'little',signed=True)==callback_va
    callback_witnesses.append(dict(address=hex(address),original_bytes=ref.hex()))
callback_name=f'callback_leaf_{callback_va:X}'
callback_body=lifter.lift_function(callback_code,callback_va,name=callback_name)
assert 'RECOMP_TODO(' not in callback_body
bodies.append(callback_body.replace('void '+callback_name+'(', 'static void '+callback_name+'(',1))
callback_literal='"'+''.join('\\x%02x'%b for b in callback_code)+'"'
entries.append(f'    {{UINT64_C(0x{callback_va:x}),{len(callback_code)},{callback_literal},{callback_name}}},')
scene_table_va=0x14108e388
scene_table=read(scene_table_va,24)
import struct,re
scene_targets=[base+rva for rva in struct.unpack('<6I',scene_table)]
assert scene_targets==[0x14108db79,0x14108dbf7,0x14108dd5e,0x14108deea,0x14108e010,0x14108e1b7]
scene_prefix=read(0x14108db40,scene_table_va-0x14108db40)
scene_decode=list(decoder.disasm(scene_prefix,0x14108db40))
assert sum(i.size for i in scene_decode)==len(scene_prefix)
assert set(scene_targets)|{0x14108dea6}<=set(i.address for i in scene_decode)
scene_dispatch=[(i.mnemonic,i.op_str)for i in scene_decode if 0x14108db5b<=i.address<=0x14108db77]
assert scene_dispatch==[('cmp','edx, 5'),('ja','0x14108e33a'),('mov','eax, edx'),
    ('lea','rcx, [rip - 0x108db6d]'),('mov','edx, dword ptr [rcx + rax*4 + 0x108e388]'),('add','rdx, rcx'),('jmp','rdx')]
scene_canonical=ROOT/'generated/frostpunk-x64/recomp_funcs_0836.c'
scene_meta=json.loads((ROOT/'generated/frostpunk-x64/recomp_meta.json').read_text())
assert sha(scene_canonical)==scene_meta['generated_files'][scene_canonical.name]
scene_lines=scene_canonical.read_text().splitlines();scene_rows=[];scene_cases=[];scene_baselines=[]
for va,end,tail in [(0x14108dea6,0x14108deea,0x14108dbe5),(0x14108deea,0x14108e010,0x14108dea6),
                    (0x14108e010,0x14108e1b7,0x14108dbe5),(0x14108e1b7,0x14108e33a,0x14108dbe5)]:
    code=read(va,end-va);name=f'scene_switch_{va:X}';body=lifter.lift_function(code,va,name=name)
    assert 'RECOMP_TODO('not in body
    bodies.append(body.replace('void '+name+'(', 'static void '+name+'(',1))
    literal='"'+''.join('\\x%02x'%b for b in code)+'"'
    entries.append(f'    {{UINT64_C(0x{va:x}),{len(code)},{literal},{name}}},')
    scene_cases.append(f'    {{UINT64_C(0x{va:x}),UINT64_C(0x{tail:x}),{len(code)},{literal}}},')
    first=next(n for n,line in enumerate(scene_lines)if f'/* {va:012X}:'in line)
    last=next(n for n in range(first+1,len(scene_lines))if f'/* {end:012X}:'in scene_lines[n])
    while scene_lines[last-1].startswith('L_')or scene_lines[last-1].strip().startswith('c->rip ='):last-=1
    baseline='\n'.join(scene_lines[first:last]);labels=set(re.findall(r'^L_([0-9A-F]+):',baseline,re.M))
    baseline=re.sub(r'goto L_([0-9A-F]+);',lambda m:m[0]if m[1]in labels else '{ dispatch(c,0x%sull); return; }'%m[1],baseline)
    scene_baselines.append(f'static void baseline_{va:X}(CPU *c) {{\n    c->rip=0x{va:X}ull;\n'+baseline+'\n}\n')
    scene_rows.append(dict(address=hex(va),end=hex(end),tail=hex(tail),size=len(code),original_bytes=code.hex(),
        original_bytes_sha256=hashlib.sha256(code).hexdigest(),lifted_C_sha256=hashlib.sha256(body.encode()).hexdigest()))
scene_baseline_path=(args.candidate_dir or ROOT/'tests')/'windows_scene_switch_baseline.h'
scene_baseline_path.write_text('\n'.join(scene_baselines),encoding='utf-8')
compare_table_va=0x140dbd19c
compare_table=read(compare_table_va,24)
compare_targets=[base+rva for rva in struct.unpack('<6I',compare_table)]
assert compare_targets==[0x140dbd0f3,0x140dbd106,0x140dbd111,0x140dbd12c,0x140dbd134,0x140dbd13c]
compare_prefix=read(0x140dbd070,compare_table_va-0x140dbd070)
compare_decode=list(decoder.disasm(compare_prefix,0x140dbd070))
assert sum(i.size for i in compare_decode)==len(compare_prefix)
assert set(compare_targets)<=set(i.address for i in compare_decode)
assert [(i.mnemonic,i.op_str)for i in compare_decode if 0x140dbd0db<=i.address<=0x140dbd0f1]==[
    ('cmp','eax, 5'),('ja','0x140dbd141'),('lea','rdx, [rip - 0xdbd0e7]'),
    ('mov','ecx, dword ptr [rdx + rax*4 + 0xdbd19c]'),('add','rcx, rdx'),('jmp','rcx')]
compare_canonical=ROOT/'generated/frostpunk-x64/recomp_funcs_0796.c'
assert sha(compare_canonical)==scene_meta['generated_files'][compare_canonical.name]
compare_rows=[];compare_cases=[]
for va,end,tail,raw in [(0x140dbd134,0x140dbd13c,0x140dbd117,'0f2fc60f93c0ebdb'),
                       (0x140dbd13c,0x140dbd141,0x140dbd114,'0f2fc6ebd3')]:
    code=read(va,end-va);assert code.hex()==raw;name=f'compare_switch_{va:X}'
    body=lifter.lift_function(code,va,name=name);assert 'RECOMP_TODO('not in body
    bodies.append(body.replace('void '+name+'(', 'static void '+name+'(',1))
    literal='"'+''.join('\\x%02x'%b for b in code)+'"'
    entries.append(f'    {{UINT64_C(0x{va:x}),{len(code)},{literal},{name}}},')
    compare_cases.append(f'    {{UINT64_C(0x{va:x}),UINT64_C(0x{tail:x}),{len(code)},{literal}}},')
    compare_rows.append(dict(address=hex(va),end=hex(end),tail=hex(tail),size=len(code),original_bytes=raw,
        original_bytes_sha256=hashlib.sha256(code).hexdigest(),lifted_C_sha256=hashlib.sha256(body.encode()).hexdigest()))
bounded_specs=[(0x140dbce94,0x140dbce9b,0x140dbce78,'0796'),(0x140dbce9b,0x140dbcea2,0x140dbce78,'0796'),
    (0x140dbcea2,0x140dbcea9,0x140dbce78,'0796'),(0x140dbcea9,0x140dbceb0,0x140dbce78,'0796'),
    (0x140e54e92,0x140e54eac,0x140e54f12,'0806'),(0x140e54eac,0x140e54ec6,0x140e54f12,'0806'),
    (0x140e54ec6,0x140e54ee0,0x140e54f12,'0806'),(0x140e54ee0,0x140e54efa,0x140e54f12,'0806'),
    (0x140e54efa,0x140e54f12,0x140e54f12,'0806')]
bounded_tables=[]
for start,end,table_va,count in [(0x140dbce1e,0x140dbce73,0x140dbcf0c,6),(0x140e54d8f,0x140e54db9,0x140e54f5c,8)]:
    prefix=read(start,end-start);instructions=list(decoder.disasm(prefix,start));assert sum(i.size for i in instructions)==len(prefix)
    table_bytes=read(table_va,count*4);targets=[base+rva for rva in struct.unpack('<'+'I'*count,table_bytes)]
    expected=[0x140dbce73,0x140dbce8d,0x140dbce94,0x140dbce9b,0x140dbcea2,0x140dbcea9]if count==6 else [0x140e54db9,0x140e54dd9,0x140e54df9,0x140e54e92,0x140e54ec6,0x140e54eac,0x140e54ee0,0x140e54efa]
    assert targets==expected
    tail_instructions=[(i.mnemonic,i.op_str)for i in instructions if i.address>=(0x140dbce5a if count==6 else 0x140e54d9d)]
    assert tail_instructions==([('cmp','ecx, 5'),('ja','0x140dbceb0'),('lea','r8, [rip - 0xdbce66]'),
        ('mov','edx, dword ptr [r8 + rcx*4 + 0xdbcf0c]'),('add','rdx, r8'),('jmp','rdx')]if count==6 else
        [('cmp','eax, 7'),('ja','0x140e54f15'),('lea','rcx, [rip - 0xe54dad]'),
         ('mov','edx, dword ptr [rcx + rax*4 + 0xe54f5c]'),('add','rdx, rcx'),('jmp','rdx')])
    bounded_tables.append(dict(start=hex(start),end=hex(end),prefix_sha256=hashlib.sha256(prefix).hexdigest(),table=hex(table_va),
        count=count,table_bytes=table_bytes.hex(),targets=list(map(hex,targets)),prefix_complete_decode=True))
bounded_rows=[];bounded_cases=[];bounded_baselines=[];bounded_source_hashes={}
for va,end,tail,unit in bounded_specs:
    code=read(va,end-va);name=f'bounded_switch_{va:X}';body=lifter.lift_function(code,va,name=name)
    assert 'RECOMP_TODO('not in body
    assert not any(i.mnemonic.endswith(('ps','pd'))or i.mnemonic=='cvttpd2dq'for i in decoder.disasm(code,va))
    bodies.append(body.replace('void '+name+'(', 'static void '+name+'(',1))
    literal='"'+''.join('\\x%02x'%b for b in code)+'"'
    entries.append(f'    {{UINT64_C(0x{va:x}),{len(code)},{literal},{name}}},')
    bounded_cases.append(f'    {{UINT64_C(0x{va:x}),UINT64_C(0x{tail:x}),{len(code)},{literal}}},')
    canonical=ROOT/f'generated/frostpunk-x64/recomp_funcs_{unit}.c';assert sha(canonical)==scene_meta['generated_files'][canonical.name]
    bounded_source_hashes[canonical.relative_to(ROOT).as_posix()]=sha(canonical);lines=canonical.read_text().splitlines()
    first=next(n for n,line in enumerate(lines)if f'/* {va:012X}:'in line)
    last=next(n for n in range(first+1,len(lines))if f'/* {end:012X}:'in lines[n])
    while lines[last-1].startswith('L_')or lines[last-1].strip().startswith('c->rip ='):last-=1
    baseline='\n'.join(lines[first:last]);labels=set(re.findall(r'^L_([0-9A-F]+):',baseline,re.M))
    baseline=re.sub(r'goto L_([0-9A-F]+);',lambda m:m[0]if m[1]in labels else '{ dispatch(c,0x%sull); return; }'%m[1],baseline)
    bounded_baselines.append(f'static void baseline_{va:X}(CPU *c) {{\n    c->rip=0x{va:X}ull;\n'+baseline+f'\n    dispatch(c,0x{tail:X}ull);\n}}\n')
    bounded_rows.append(dict(address=hex(va),end=hex(end),tail=hex(tail),size=len(code),original_bytes=code.hex(),
        original_bytes_sha256=hashlib.sha256(code).hexdigest(),lifted_C_sha256=hashlib.sha256(body.encode()).hexdigest()))
bounded_baseline_path=(args.candidate_dir or ROOT/'tests')/'windows_bounded_extension_baseline.h'
bounded_baseline_path.write_text('\n'.join(bounded_baselines),encoding='utf-8')
bodies+=['static const extra_entry entries[]={',*entries,'    SWITCH_EXTRA_ENTRIES']
if extension_report:bodies.append('    SWITCH_EXTENSION_ENTRIES')
bodies+=['};','#undef SWITCH_EXTRA_ENTRIES']
if extension_report:bodies.append('#undef SWITCH_EXTENSION_ENTRIES')
(out/'extra_aot_bodies.inc').write_text('\n\n'.join(bodies)+'\n',encoding='utf-8')
test_cases.write_text('static const test_case cases[]={\n'+'\n'.join(cases)+'\n};\n'+
    f'static const test_case callback_cases[]={{\n    {{UINT64_C(0x{callback_va:x}),0,{len(callback_code)},{callback_literal}}},\n}};\n'+
    'static const test_case scene_cases[]={\n'+'\n'.join(scene_cases)+'\n};\n'+
    'static const test_case compare_cases[]={\n'+'\n'.join(compare_cases)+'\n};\n'+
    'static const test_case bounded_cases[]={\n'+'\n'.join(bounded_cases)+'\n};\n',encoding='ascii')
(out/'extra_aot.c').write_text('''#include <string.h>
#include "extra_aot.h"
typedef struct {uint64_t va;size_t size;const char *original;void(*fn)(CPU*);} extra_entry;
#include "extra_aot_bodies.inc"
static int verified;
int fp_extra_aot_init(const fp_image *image) {
    verified=0;
    if(image->preferred_base!=UINT64_C(0x140000000))return 0;
    for(size_t i=0;i<sizeof(entries)/sizeof(*entries);++i) {
        const extra_entry *e=&entries[i];uint64_t at=(uintptr_t)image->base+e->va-image->preferred_base;
        if(!fp_image_code(image,at) || !fp_image_code(image,at+e->size-1) ||
           memcmp((void *)(uintptr_t)at,e->original,e->size))return 0;
    }
    verified=1;return 1;
}
void (*fp_extra_aot_lookup(uint64_t original))(CPU *) {
    if(verified)for(size_t i=0;i<sizeof(entries)/sizeof(*entries);++i)
        if(entries[i].va==original)return entries[i].fn;
    return NULL;
}
''',encoding='utf-8')
receipt=dict(original_exe_sha256=sha(original),census_sha256=sha(report_path),count=len(rows)+18+(5 if extension_report else 0),bytes=169+185+1172+13+156+(extension_report['bytes'] if extension_report else 0),
    ret_adjacent_leaves=len(rows),switch_continuations=2,switch_provenance_sha256=sha(ROOT/'research/windows-switch-closure.json'),
    body_hashes_match_census=True,producer_sha256=sha(Path(__file__)),
    outputs={p.name:sha(p) for p in [out/'extra_aot.c',out/'extra_aot_bodies.inc',out/'switch_aot_bodies.inc',test_cases]})
receipt['observed_callback']=dict(address=hex(callback_va),size=len(callback_code),original_bytes=callback_code.hex(),
    original_bytes_sha256=hashlib.sha256(callback_code).hexdigest(),lifted_C_sha256=hashlib.sha256(callback_body.encode()).hexdigest(),
    boundary='three_INT3_bytes_before_and_after',lea_references=callback_witnesses,observed_return='0x140ef3e90',
    static_semantics='RAX=read64(RCX); tail_dispatch(read64(RAX+0x108)); no_stack_pop')
receipt['scene_switch']=dict(table=hex(scene_table_va),table_bytes=scene_table.hex(),table_targets=list(map(hex,scene_targets)),
    prefix_start='0x14108db40',prefix_sha256=hashlib.sha256(scene_prefix).hexdigest(),prefix_complete_decode=True,
    bounded_index_max=5,existing_local_targets=['0x14108db79','0x14108dbf7'],existing_global_target='0x14108dd5e',
    missing_table_targets=['0x14108deea','0x14108e010','0x14108e1b7'],required_shared_tail='0x14108dea6',
    canonical_source=scene_canonical.relative_to(ROOT).as_posix(),canonical_source_sha256=sha(scene_canonical),
    entries=scene_rows,stack_top_is_local=True,stack_pop_added=False)
receipt['outputs'][scene_baseline_path.name]=sha(scene_baseline_path)
receipt['compare_switch']=dict(table=hex(compare_table_va),table_bytes=compare_table.hex(),table_targets=list(map(hex,compare_targets)),
    prefix_start='0x140dbd070',prefix_sha256=hashlib.sha256(compare_prefix).hexdigest(),prefix_complete_decode=True,bounded_index_max=5,
    existing_local_targets=['0x140dbd0f3','0x140dbd106','0x140dbd111'],existing_global_target='0x140dbd12c',
    missing_table_targets=['0x140dbd134','0x140dbd13c'],existing_shared_epilogs=['0x140dbd114','0x140dbd117'],
    canonical_source=compare_canonical.relative_to(ROOT).as_posix(),canonical_source_sha256=sha(compare_canonical),
    entries=compare_rows,stack_top_is_local=True,stack_pop_added=False)
receipt['bounded_table_extension']=dict(tables=bounded_tables,entries=bounded_rows,canonical_sources=bounded_source_hashes,
    candidate_census_sha256='0494c8db8fbe4afee47b76ff838d7f9eb2efebe624ff159c406ab7f50b2b390a',
    existing_shared_tails=['0x140dbce78','0x140e54f12'],count=9,bytes=156,stack_pop_added=False)
receipt['outputs'][bounded_baseline_path.name]=sha(bounded_baseline_path)
if extension_report:
    receipt['switch_extension_provenance_sha256']=sha(extension_report_path)
    receipt['switch_extensions']=extension_report['count']
    receipt['outputs']['switch_extension_bodies.inc']=sha(out/'switch_extension_bodies.inc')
((args.candidate_dir or ROOT/'research')/'windows-extra-aot.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
print(json.dumps(receipt,indent=2))
