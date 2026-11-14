"""Qualify the single sealed Windows E35 leaf against its native instructions."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

from leaf_probe import ROOT, windows_toolchain
import generate_windows_direct_dispatch as producer
import argparse
import capstone
import pefile

SOURCE_ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/windows-e35a50-test'
OUT.mkdir(parents=True,exist_ok=True)
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--benchmark',action='store_true',help='Optional isolated measurement, skipped if a compiler or game is active.')
args=parser.parse_args()

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def heavy_processes():
    p=subprocess.run(['powershell','-NoProfile','-Command',"Get-Process clang,clang++,cl,ml64,link,Frostpunk -ErrorAction SilentlyContinue | Select-Object ProcessName,Id | ConvertTo-Json -Compress"],capture_output=True,text=True,timeout=20)
    return json.loads(p.stdout) if p.stdout.strip() else []
def differences(a,b):
    assert len(a)==len(b)
    offsets=[i for i,(x,y) in enumerate(zip(a,b)) if x!=y]
    return dict(equal=not offsets,differing_bytes=len(offsets),first_offsets=offsets[:32])

config=json.loads((ROOT/'port.json').read_text());binaries,env=windows_toolchain()
clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
canonical=ROOT/'generated/frostpunk-x64/recomp_funcs_0804.c'
raw=canonical.read_bytes().decode();parts=producer.e35a50_parts(raw)
rewritten=producer.rewrite_e35a50(raw,canonical.name)
assert rewritten.count(parts['fragment'].replace('\n','\r\n'))==1
rejected=0
for text,name in [(raw.replace(parts['raw'],parts['raw'].replace('0x140E35A50ull','0x140E35A51ull',1)),canonical.name),
                  (raw.replace(parts['raw'],parts['raw'].replace('c->rsp += 8; return;','c->rsp += 16; return;',1)),canonical.name),
                  (raw+parts['raw'],canonical.name),
                  (raw.replace(parts['raw'],''),canonical.name),
                  (raw,'recomp_funcs_0803.c'),
                  (raw.replace(parts['raw'],parts['raw'].replace('c->xmm[0].f32[0]','c->xmm[1].f32[0]',1)),canonical.name),
                  (rewritten,canonical.name)]:
    try:producer.rewrite_e35a50(text,name)
    except ValueError:rejected+=1
    else:raise AssertionError('Accepted altered E35 source')
assert rejected==7
original=Path(config['game_directory'])/'Frostpunk.exe'
assert digest(original)==config['original_exe_sha256']
with pefile.PE(str(original),fast_load=True) as pe:code=producer.verify_e35a50_bytes(pe)
decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);decoder.detail=True
instructions=list(decoder.disasm(code,0x140e35a50))
comments=re.findall(r'/\*\s+([0-9A-F]+):\s*(\w+)(.*?)\*/',parts['body'])
assert len(instructions)==len(comments)==175
for instruction,(address,mnemonic,operands) in zip(instructions,comments):
    assert instruction.address==int(address,16) and instruction.mnemonic==mnemonic
    assert re.sub(r'\s+','',instruction.op_str)==re.sub(r'\s+','',operands)
    assert not instruction.mnemonic.startswith(('call','j','loop'))
    assert not any(operand.type==capstone.x86.X86_OP_MEM and operand.mem.base==capstone.x86.X86_REG_RIP for operand in instruction.operands)
assert instructions[-1].mnemonic=='ret'
include=OUT/'include';include.mkdir(exist_ok=True)
watched=[canonical,original,ROOT/'port.json',ROOT/'tools/leaf_probe.py',Path(producer.__file__),Path(producer.direct.__file__),Path(__file__),
    SOURCE_ROOT/'tools/build_windows.py',SOURCE_ROOT/'tests/windows_e35a50_fixture.c',SOURCE_ROOT/'tests/windows_e35a50_fixture.asm']
for name in ['cpu64.h','sse_scalar.h','eh64.h','bswap.h']:
    path=ROOT/'native/runtime64'/name;watched.append(path);text=path.read_text()
    if name=='cpu64.h':text=producer.direct.once(text,'#if defined(__aarch64__)\n/* Android host must supply','#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)\n/* Android host must supply')
    (include/name).write_text(text,encoding='utf-8',newline='\n');watched.append(include/name)
exports='''
int e35_admit(const CPU *c){return fp_e35_admit(c);}
int e35_test_stack_admit(const CPU *c,uint64_t a,uint64_t b,uint64_t sp){return fp_e35_stack_admit(c,a,b,sp);}
int e35_test_working_admit(const CPU *c,uint64_t start){return start<=UINT64_MAX-sizeof(CPU) && fp_e35_stack_admit(c,start,start+sizeof(CPU),start);}
void e35_candidate(CPU *c){L_000140E35A50(c);}
'''
contents={
    'production.c':'#include "cpu64.h"\n'+parts['fragment']+exports,
    'baseline.c':'#include "cpu64.h"\n'+parts['helpers']+producer.direct.once(parts['ordered'],'void L_000140E35A50(CPU *c)','__declspec(noinline) void e35_baseline(CPU *c)'),
    'legacy.c':'#include "cpu64.h"\n'+producer.direct.once(parts['body'],'void L_000140E35A50(CPU *c)','__declspec(noinline) void e35_legacy(CPU *c)'),
    'fixture.c':(SOURCE_ROOT/'tests/windows_e35a50_fixture.c').read_text(),
    'native_bytes.h':'static const unsigned char original_bytes[]={'+','.join(hex(b) for b in code)+'};\n'}
for name,text in contents.items():
    path=OUT/name;path.write_text(text,encoding='utf-8',newline='\n');watched.append(path)
sources=[OUT/name for name in ['baseline.c','production.c','legacy.c','fixture.c']]
before={str(path.resolve()):digest(path) for path in watched}
prepared=dict(contract=producer.e35a50_metadata(),production_fragment_sha256=hashlib.sha256(parts['fragment'].encode()).hexdigest())
commands=[]
def run(command,timeout=60):
    command=list(map(str,command));commands.append(command)
    p=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=timeout)
    assert p.returncode==0,(command,p.returncode,p.stdout,p.stderr)
    return p

flags=['--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-fno-math-errno','-DFROSTPUNK_WINDOWS_HOST=1','-I'+str(OUT/'include')]
objects=[];codegen={}
instruction_lists={}
for source in sources:
    obj=source.with_suffix('.obj');objects.append(obj)
    run([clang,*flags,'-c',source,'-o',obj])
    if source.name!='fixture.c':
        p=run([binaries/'dumpbin.exe','/nologo','/DISASM:NOBYTES',obj]);source.with_suffix('.disasm.txt').write_text(p.stdout)
        instructions=re.findall(r'^\s+[0-9A-Fa-f]+:\s+([^\r\n]+)',p.stdout,re.M)
        instruction_lists[source.stem]=instructions
        counts={}
        for instruction in instructions:
            mnemonic=instruction.split()[0];counts[mnemonic]=counts.get(mnemonic,0)+1
        codegen[source.stem]=dict(instruction_total=len(instructions),instructions=counts,object_sha256=digest(obj),disassembly_sha256=digest(source.with_suffix('.disasm.txt')))
native_obj=OUT/'native_oracle.obj'
run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(native_obj),SOURCE_ROOT/'tests/windows_e35a50_fixture.asm']);objects.append(native_obj)
exe=OUT/'fixture.exe'
run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/BASE:0x180000000','/DYNAMICBASE:NO','/STACK:16777216','/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup',*objects,'libcmt.lib','oldnames.lib','kernel32.lib'])
normal=run([exe,'--normal']);(OUT/'normal.log').write_text(normal.stdout+normal.stderr)
normal_rows=[json.loads(line) for line in normal.stdout.splitlines()]
legacy=run([exe,'--legacy']);(OUT/'legacy.log').write_text(legacy.stdout+legacy.stderr)
legacy_rows=[json.loads(line) for line in legacy.stdout.splitlines()]
admission=run([exe,'--admission']);admission=json.loads(admission.stdout)
native=run([exe,'--native']);(OUT/'native.log').write_text(native.stdout+native.stderr)
native_rows=[json.loads(line) for line in native.stdout.splitlines()]
stack_fallback=json.loads(run([exe,'--stack-fallback']).stdout)
faults={}
for kind in ['input','output','stack','stack-lower','invalid','overflow']:
    observations={}
    for variant in ['baseline','candidate']:
        p=run([exe,'--fault',variant,kind]);record=json.loads(p.stdout)
        cpu_path=OUT/(kind+'.'+variant+'.cpu.bin');arena_path=OUT/(kind+'.'+variant+'.arena.bin')
        shutil.copyfile(OUT/'fault-cpu.bin',cpu_path);shutil.copyfile(OUT/'fault-final-arena.bin',arena_path)
        observations[variant]=dict(record=record,cpu_sha256=digest(cpu_path),arena_sha256=digest(arena_path))
    cpu_comparison=differences((OUT/(kind+'.baseline.cpu.bin')).read_bytes(),(OUT/(kind+'.candidate.cpu.bin')).read_bytes())
    arena_comparison=differences((OUT/(kind+'.baseline.arena.bin')).read_bytes(),(OUT/(kind+'.candidate.arena.bin')).read_bytes())
    for name,offset,size in [('rax',0,8),('rsp',32,8),('r11',88,8),('rip',128,8),('flags',136,48),('xmm',184,256),('mxcsr',440,4)]:
        a=(OUT/(kind+'.baseline.cpu.bin')).read_bytes()[offset:offset+size]
        b=(OUT/(kind+'.candidate.cpu.bin')).read_bytes()[offset:offset+size]
        cpu_comparison[name]=dict(equal=a==b,baseline=a.hex(),candidate=b.hex())
    faults[kind]=dict(observations=observations,cpu_at_fault=cpu_comparison,final_arena_after_resume=arena_comparison)

processes_before=heavy_processes();bench=[]
if args.benchmark and not processes_before and not stack_fallback["memory_mismatches"] and not stack_fallback["environment_mismatches"] and not native_rows[-1]["memory_mismatches"] and not native_rows[-1]["environment_mismatches"] and not normal_rows[-1]["memory_mismatches"] and not normal_rows[-1]["environment_mismatches"]:
    for variant in ['legacy','baseline','candidate','native','native','candidate','baseline','legacy']:
        p=run([exe,'--bench',variant]);rows=[json.loads(line) for line in p.stdout.splitlines()]
        values=[row['ticks']*1e9/row['frequency']/row['iterations'] for row in rows]
        bench.append(dict(variant=variant,rows=rows,ns_per_call=values,median_ns=statistics.median(values[1:])))
processes_after=heavy_processes()
summary={variant:statistics.median([value for run in bench if run['variant']==variant for value in run['ns_per_call'][1:]]) for variant in ['baseline','candidate','legacy','native']} if bench else {}
if summary:
    summary['ratio']=summary['candidate']/summary['baseline']
    summary['vs_legacy_ratio']=summary['candidate']/summary['legacy']
assert before=={path:digest(Path(path)) for path in before}
report=dict(experiment_completed=True,stack_fallback=stack_fallback,native=native_rows[-1],native_first_mismatches=native_rows[:-1],legacy=legacy_rows[-1],legacy_first_mismatches=legacy_rows[:-1],normal=normal_rows[-1],normal_first_mismatches=normal_rows[:-1],admission=admission,
    
    faults=faults,codegen=codegen,benchmark=bench,benchmark_summary=summary,processes_before_micro=processes_before,processes_after_micro=processes_after,
    continuous_contention_checked=False,sources=before,prepared=prepared,compiler_sha256=digest(clang),executable_sha256=digest(exe),commands=commands,
    canonical_current_cpu64_sha256=digest(ROOT/'native/runtime64/cpu64.h'),frozen_cpu64_used=False,
    test_only_VEH=True,test_only_caller_TLS=True,production_exception_handling_changed=False,canonical_modified=False,gameplay_FPS_measured=False)
assert report['normal']['normal_cases']==3456 and report['normal']['fast']==2688 and report['normal']['fallback']==768
assert report['native']['native_cases']==1536 and report['native']['ordered_memory']==0 and report['native']['local_memory']==0
assert report['stack_fallback']['stack_fallback_cases']==384 and admission['admission_cases']==48
for key in ['normal','native','stack_fallback']:
    assert report[key]['memory_mismatches']==0 and report[key]['environment_mismatches']==0,key
for kind,value in faults.items():
    assert value['final_arena_after_resume']['equal'],kind
    for observation in value['observations'].values():
        row=observation['record']
        assert row['active_cpu_identity']==1 and row['depth']==4 and row['resumed_to_return'],kind
    if kind in ['invalid','overflow']:assert value['cpu_at_fault']['equal'],kind
assembly=(OUT/'production.disasm.txt').read_text()
match=re.search(r'(?m)^fp_e35_local:\n(.*?)(?=^[A-Za-z_]\w*:\n|^  Summary|\Z)',assembly,re.S)
assert match,'Working helper must remain a separately admitted noinline function'
operations=re.findall(r'^\s+[0-9A-Fa-f]+:\s+(\w+)',match[1],re.M)
for operation,count in dict(mulps=35,subps=13,addps=8,rcpss=1,mulss=2).items():
    assert operations.count(operation)==count,(operation,operations.count(operation))
assert 'memcpy' not in match[1], 'Working CPU failed to registerize'
report.update(success=True,contract=producer.e35a50_metadata(),source_hashes={path:value for path,value in before.items() if Path(path).is_relative_to(ROOT)},
    negative_source_mutations_rejected=rejected,codegen_qualified=True,
    local_codegen_instruction_count=len(operations),native_original_bytes_test_only=True,
    production_original_bytes_executed=0,production_fragment_sha256=hashlib.sha256(parts['fragment'].encode()).hexdigest())
(OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(dict(experiment_completed=True,normal=report['normal'],admission=admission,codegen=codegen,
    fault_summary={key:dict(cpu_at_fault_equal=value['cpu_at_fault']['equal'],final_arena_equal=value['final_arena_after_resume']['equal'],baseline=value['observations']['baseline']['record'],candidate=value['observations']['candidate']['record']) for key,value in faults.items()},
    benchmark_summary=summary,processes_before_micro=processes_before,processes_after_micro=processes_after),indent=2))
