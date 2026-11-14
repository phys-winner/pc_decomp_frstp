"""Compare the three complete current particle extents in an isolated live arena."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import statistics

import capstone
import pefile
from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-particle-functions-test'
SCOPES = [('140F63A85',0x140f642c5,39),('140F6460C',0x140f64b00,36),('140F66DC6',0x140f6754f,21)]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    config=json.loads((ROOT/'port.json').read_text())
    llvm=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    bins,env=windows_toolchain();original=Path(config['game_directory'])/'Frostpunk.exe'
    if sha(original)!=config['original_exe_sha256']:raise ValueError('Original PE changed')
    canonical=ROOT/'generated/frostpunk-x64/recomp_funcs_0820.c'
    overlay=ROOT/'build/arm64/generated-overlay/recomp_funcs_0820.c'
    approved=json.loads((ROOT/'research/android-aot-overlay.json').read_text())
    row=next(row for row in approved['units'] if Path(row['canonical']['path']).name==canonical.name)
    if sha(canonical)!=row['canonical']['sha256'] or sha(overlay)!=row['overlay']['sha256']:
        raise ValueError('Current canonical/ARM overlay source identity changed')
    scalar=ROOT/'build/android-particle-scalar-guarded-test/result.json'
    scalar_report=json.loads(scalar.read_text());candidate_header=ROOT/scalar_report['candidate_header']['path']
    if not scalar_report['host']['success'] or sha(candidate_header)!=scalar_report['candidate_header']['sha256']:
        raise ValueError('Guarded helper lacks its isolated proof')
    names=['native/runtime64/cpu64.h','native/runtime64/sse_scalar.h','native/runtime64/eh64.h',
           'android/host/aot_cpu.h','android/host/aot_cpu.c','android/host/aot_cpu_float.c','android/host/aot_cpu_sites.inc',
           'research/android-aot-overlay.json','generated/frostpunk-x64/recomp_funcs_0820.c',
           'build/arm64/generated-overlay/recomp_funcs_0820.c','tools/test_android_particle_functions.py',
           'tests/android_particle_functions_fixture.c',scalar.relative_to(ROOT).as_posix(),candidate_header.relative_to(ROOT).as_posix()]
    before={name:sha(ROOT/name) for name in names}
    text=overlay.read_text();baseline=[];candidate=[];scopes=[];constants=set()
    pe=pefile.PE(str(original),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
    decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    for va,end,count in SCOPES:
        symbol='L_000'+va;begin=text.index('void '+symbol+'(CPU *c)');finish=text.find('\nvoid L_',begin+1)
        if finish<0:raise ValueError('Missing extent delimiter')
        body=text[begin:finish]
        if body.count('frostpunk_addss(')+body.count('frostpunk_mulss(')!=count:
            raise ValueError('Scalar helper scope changed: '+va)
        calls=[]
        for line in body.splitlines():
            if 'frostpunk_addss(' not in line and 'frostpunk_mulss(' not in line:continue
            address=int(re.search(r'/\* ([0-9A-F]+):',line)[1],16)
            instruction=next(decoder.disasm(pe.get_data(address-base,15),address),None)
            expected='addss' if 'frostpunk_addss('in line else 'mulss'
            if instruction is None or instruction.mnemonic!=expected:raise ValueError('Original scalar opcode mismatch')
            calls.append(dict(va=hex(address),bytes=bytes(instruction.bytes).hex(),instruction=instruction.mnemonic+' '+instruction.op_str))
        constants.update(int(va,16)for va in re.findall(r'GVA\((0x[0-9A-Fa-f]+)\)',body))
        baseline.append(body.replace('void '+symbol+'(', 'void baseline_'+va[3:]+'(',1))
        candidate.append(body.replace('void '+symbol+'(', 'void candidate_'+va[3:]+'(',1).replace('frostpunk_addss(', 'fp_particle_addss(').replace('frostpunk_mulss(', 'fp_particle_mulss('))
        scopes.append(dict(va=hex(int(va,16)),end=hex(end),original_bytes_sha256=hashlib.sha256(pe.get_data(int(va,16)-base,end-int(va,16))).hexdigest(),
                           body_sha256=hashlib.sha256(body.encode()).hexdigest(),scalar_calls=count,sites=calls))
    (OUT/'particle_baseline.inc').write_text('\n'.join(baseline))
    (OUT/'particle_candidate.inc').write_text('\n'.join(candidate))
    image=pe.get_memory_mapped_image(max_virtual_address=pe.OPTIONAL_HEADER.SizeOfImage).ljust(pe.OPTIONAL_HEADER.SizeOfImage,b'\0')
    header=image[:pe.OPTIONAL_HEADER.SizeOfHeaders]
    rows=['#define FIXTURE_IMAGE_BYTES '+str(pe.OPTIONAL_HEADER.SizeOfImage),
          'static const unsigned char fixture_pe_header[]={'+','.join(str(b)for b in header)+'};',
          'static const uint64_t fixture_targets[]={'+','.join('UINT64_C(0x'+va+')'for va,_,_ in SCOPES)+'};',
          'static const struct {uint64_t va;unsigned char bytes[16];} fixture_constants[]={']
    constant_rows=[]
    for address in sorted(constants):
        data=image[address-base:address-base+16]
        if len(data)!=16:raise ValueError('Original data address escapes image')
        rows.append('{UINT64_C('+hex(address)+'),{'+','.join(str(b)for b in data)+'}},')
        constant_rows.append(dict(va=hex(address),bytes=data.hex()))
    rows.append('};');(OUT/'particle_original_data.inc').write_text('\n'.join(rows)+'\n')
    commands=[]
    def run(args,check=True):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if check and result.returncode:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+result.stdout[-3000:]+result.stderr[-3000:])
        return result
    flags=['-std=c11','-O2','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-fno-math-errno',
           '-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(candidate_header.parent),'-I'+str(OUT)]
    source=ROOT/'tests/android_particle_functions_fixture.c';obj=OUT/'functions-host.obj';host=OUT/'functions-host.exe'
    run([llvm/'clang.exe','--target=x86_64-pc-windows-msvc',*flags,'-fms-runtime-lib=static','-c',source,'-o',obj])
    run([bins/'link.exe','/nologo','/OUT:'+str(host),obj,'libcmt.lib','libvcruntime.lib','libucrt.lib','oldnames.lib','kernel32.lib'])
    host_run=run([host],False);(OUT/'host.stdout.json').write_text(host_run.stdout);(OUT/'host.stderr.txt').write_text(host_run.stderr)
    if host_run.returncode:raise RuntimeError('Whole-extent host proof failed: '+host_run.stderr[-3000:])
    host_result=json.loads(host_run.stdout)
    host_benchmark=None
    if host_result['success']:
        host_benchmark=json.loads(run([host,'--benchmark','256']).stdout)
        for entry in host_benchmark['entries']:
            entry['baseline_median_ns']=statistics.median(row['baseline_ns']for row in entry['samples'])
            entry['candidate_median_ns']=statistics.median(row['candidate_ns']for row in entry['samples'])
    arm=OUT/'particle-functions-arm64'
    run([llvm/'clang.exe','--target=aarch64-linux-android30',*flags,'-fPIC','-fvisibility=hidden','-g0','-w',source,
         '-Wl,-z,max-page-size=16384','-lm','-o',arm])
    disassembly=OUT/'arm-disassembly.txt'
    disassembly.write_text(run([llvm/'llvm-objdump.exe','-d','--no-show-raw-insn',arm]).stdout)
    if before!={name:sha(ROOT/name)for name in names}:raise ValueError('Proof sources changed during isolated build')
    outputs={p.relative_to(ROOT).as_posix():sha(p)for p in [OUT/'particle_baseline.inc',OUT/'particle_candidate.inc',OUT/'particle_original_data.inc',host,arm,disassembly]}
    report=dict(success=host_result.get('success')is True,phase='host-proof-arm-build',source_hashes=before,outputs=outputs,
        original_exe_sha256=config['original_exe_sha256'],original_bytes_executed=False,scopes=scopes,original_data=constant_rows,
        compiler_sha256=sha(llvm/'clang.exe'),commands=commands,host=host_result,host_benchmark=host_benchmark,host_exit_code=host_run.returncode,
        arm=dict(compiled=True,binary=arm.relative_to(ROOT).as_posix(),sha256=sha(arm),args=[],benchmark_args=['--benchmark','256'],executed=False,
                 expected_common_result={key:host_result[key]for key in ['success','cases','alias_cases','native_FP_modes','per_entry','per_alias','memory_CPU_mismatches','FP_error_mismatches','trace_status_mismatches']},
                 terminal_path_counts_platform_diagnostic=True),
        external_boundary='Sincos141AE1130 deterministic return/FP/errno test double with full CPU+64KiB memory/nativeFP input trace; real packed compare helpers; TLS58 deterministic provider; original tail targets captured.',
        production_changed=False,canonical_modified=False,overlay_promoted=False,
        limitation='Exact three current generated extents; caller prologue, tail bodies, and actual sincos implementation are outside this relational fixture.')
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(success=report['success'],host=host_result,benchmark_medians=None if host_benchmark is None else
        [{k:v for k,v in entry.items()if k!='samples'}for entry in host_benchmark['entries']],
        host_stderr=host_run.stderr[:2000],arm=report['arm'],receipt=str(path),sha256=sha(path)),indent=2))


if __name__=='__main__':main()
