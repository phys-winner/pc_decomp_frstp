"""Compare scalar helpers with native SSE, including unmasked exception delivery."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import capstone
import pefile
from leaf_probe import ROOT, windows_toolchain

CANDIDATE_SHA256='37ac8f98fb965c4c9ad9b4f9304c30167259c58636ee50adb84c361139715b0d'
RECIPROCAL_CANDIDATE_SHA256='72e4fad30ffc7f252743801ce90c39b5add109356178485d9e552a41eb89f06e'
CVTT_CANDIDATE_SHA256='a374ab30a3dc8278ca0bff5ea9335d5ecd3c6bbcbff3a02a60fa31e3e1b82c61'
TRAPS={'add_snan':0,'mul_snan':0,'mul_invalid':0,'add_denormal':1,
       'add_overflow':3,'mul_overflow':3,'mul_underflow':4,'add_inexact':5,'mul_inexact':5}


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    modes=parser.add_mutually_exclusive_group()
    modes.add_argument('--reciprocal',action='store_true',help='Run the independent RCPSS/RSQRTSS and packed oracle mode instead of add/mul tests.')
    modes.add_argument('--cvtt',action='store_true',help='Run float/double CVTTSS2SI/CVTTSD2SI i32/i64 oracles; rounded CVT callers are excluded.')
    parser.add_argument('--candidate',action='store_true',help='Test the pinned isolated header for the selected mode before promotion.')
    args=parser.parse_args()
    if args.cvtt:
        header=ROOT/('build/windows-cvtt-candidate/after/cpu64.h' if args.candidate else 'native/runtime64/cpu64.h')
    elif args.reciprocal:
        header=ROOT/('build/windows-reciprocal-candidate/after/cpu64.h' if args.candidate else 'native/runtime64/cpu64.h')
    else:
        header=ROOT/('build/windows-sse-scalar-candidate/sse_scalar.h' if args.candidate else 'native/runtime64/sse_scalar.h')
    if args.candidate:assert digest(header)==(CVTT_CANDIDATE_SHA256 if args.cvtt else RECIPROCAL_CANDIDATE_SHA256 if args.reciprocal else CANDIDATE_SHA256),'Isolated header changed'
    out=ROOT/('build/windows-cvtt-test' if args.cvtt else 'build/windows-reciprocal-test' if args.reciprocal else 'build/windows-sse-scalar-test')/('candidate' if args.candidate else 'production');out.mkdir(parents=True,exist_ok=True)
    include=out/'include';include.mkdir(exist_ok=True)
    cpu=include/'cpu64.h';cpu.write_bytes((header if args.reciprocal or args.cvtt else ROOT/'native/runtime64/cpu64.h').read_bytes())
    scalar=include/'sse_scalar.h';scalar.write_bytes((ROOT/'native/runtime64/sse_scalar.h' if args.reciprocal or args.cvtt else header).read_bytes())
    config=json.loads((ROOT/'port.json').read_text());binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    fixture=ROOT/'tests/windows_sse_scalar_fixture.c';oracle=ROOT/'tests/windows_sse_scalar_fixture.asm'
    inputs=[header,cpu,scalar,fixture,oracle,ROOT/'tests/windows_cvtt_cases.inc',ROOT/'native/runtime64/cpu64.h',ROOT/'native/runtime64/sse_scalar.h',ROOT/'port.json',ROOT/'tools/leaf_probe.py',Path(__file__).resolve()]
    hashes={str(p):digest(p) for p in inputs};commands=[]
    def run(command):
        commands.append(list(map(str,command)))
        return subprocess.run(commands[-1],env=env,cwd=out,capture_output=True,timeout=30)
    p=run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(out/'oracle.obj'),oracle]);assert p.returncode==0,p.stdout+p.stderr
    p=run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',
        *(['-fno-math-errno'] if args.reciprocal or args.cvtt else []),'-DFROSTPUNK_WINDOWS_HOST=1','-I'+str(include),'-I'+str(ROOT/'native/runtime64'),'-c',fixture,'-o',out/'fixture.obj'])
    assert p.returncode==0,p.stdout+p.stderr
    if args.cvtt:
        cpp=out/'cpp-fallback.cpp'
        cpp.write_text('extern "C" {\n#include "cpu64.h"\n}\nint32_t (*pointer32)(double)=sse_cvtt_i32;\nint64_t (*pointer64)(double)=sse_cvtt_i64;\nint32_t cpp_float(float v){return sse_cvtt_i32(v);}\n',encoding='utf-8')
        hashes[str(cpp)]=digest(cpp)
        p=run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c++17','-DFROSTPUNK_WINDOWS_HOST=1',
            '-I'+str(include),'-I'+str(ROOT/'native/runtime64'),'-c',cpp,'-o',out/'cpp-fallback.obj'])
        assert p.returncode==0,p.stdout+p.stderr
    exe=out/'fixture.exe';map_path=out/'fixture.map'
    p=run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/MAP:'+str(map_path),out/'fixture.obj',out/'oracle.obj','libcmt.lib','oldnames.lib','kernel32.lib'])
    assert p.returncode==0,p.stdout+p.stderr
    pe=pefile.PE(str(exe),fast_load=True);decoder=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    codegen={}
    operations=([(prefix+kind,'cvtt'+kind[:2]+'2si') for prefix in ['cvtt_after_','cvtt_native_'] for kind in ['ss32','ss64','sd32','sd64']] if args.cvtt else
                [('fp_scalar_rcp','rcpss'),('fp_scalar_rsqrt','rsqrtss'),('fp_rcpss_oracle','rcpss'),('fp_rsqrtss_oracle','rsqrtss'),
                 ('fp_rcpps_oracle','rcpps'),('fp_rsqrtps_oracle','rsqrtps')] if args.reciprocal else
                [('fp_scalar_add','addss'),('fp_scalar_mul','mulss'),('fp_addss_oracle','addss'),('fp_mulss_oracle','mulss')])
    for name,operation in operations:
        match=re.search(r'\s'+name+r'\s+([0-9a-fA-F]{16})\s',map_path.read_text());assert match,name
        address=int(match[1],16);instructions=[]
        for instruction in decoder.disasm(pe.get_data(address-pe.OPTIONAL_HEADER.ImageBase,256),address):
            instructions.append((instruction.mnemonic,instruction.op_str))
            if instruction.mnemonic=='ret':break
        if args.cvtt:
            assert instructions==[(operation,('rax' if name.endswith('64') else 'eax')+', xmm0'),('ret','')],instructions
        elif args.reciprocal:
            if operation in ['rcpps','rsqrtps']:
                assert instructions==[('movups','xmm0, xmmword ptr [rcx]'),(operation,'xmm0, xmm0'),('movups','xmmword ptr [rdx], xmm0'),('ret','')],instructions
            else:assert instructions==[(operation,'xmm0, xmm0'),('ret','')],instructions
        elif name.startswith('fp_scalar_'):assert instructions==[(operation,'xmm0, xmm1'),('ret','')],instructions
        else:assert [m for m,_ in instructions if m in ['stmxcsr','ldmxcsr',operation]]==['stmxcsr','ldmxcsr',operation,'stmxcsr','ldmxcsr'],instructions
        codegen[name]=instructions
    if args.cvtt:
        p=run([exe,'--cvtt']);assert p.returncode==0,p.stdout+p.stderr
        result=json.loads(p.stdout)
        assert result['success'] and result['masked_cases']==70144 and result['all64_masks_safe_cases']==8192
        assert result['candidate_native_differences']==0 and result['LastError_preserved'] and result['generic_evaluates_once']
        traps={};fallback={}
        for op in range(4):
            for case in ['qnan','snan','infinity','range','inexact','denormal']:
                pair=[]
                for implementation in [0,1]:
                    p=run([exe,'--cvtt','--trap',implementation,op,case]);assert not p.stderr,p.stderr
                    record=json.loads(p.stdout);assert p.returncode&0xffffffff==record['exception_code'];pair.append(record)
                assert pair[0]==pair[1],(op,case,pair)
                if case!='denormal':assert pair[0]['exception_code'] and pair[0]['destination']==0xe5ef1234aabbccdd
                traps[str(op)+'_'+case]=pair[0]
            p=run([exe,'--cvtt','--trap',2,op,'denormal'])
            fallback[str(op)]=dict(exit_code=p.returncode&0xffffffff,result=json.loads(p.stdout))
        assert hashes=={p:digest(Path(p)) for p in hashes},'Fixture inputs changed'
        report=dict(success=True,result=result,unmasked_native_comparisons=traps,legacy_denormal_observations=fallback,
            fallback_status_is_observation_only=True,codegen=codegen,source_hashes=hashes,selected_header=str(header),
            selected_header_sha256=digest(header),compiler=dict(path=str(clang),sha256=digest(clang)),commands=commands,
            executable_sha256=digest(exe),mode='cvtt',CXX_fallback_compiled=True,rounded_CVT_callers_qualified=False,game_boot_proven=False)
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({k:v for k,v in report.items() if k not in ['source_hashes','commands','codegen','unmasked_native_comparisons']},indent=2));return
    if args.reciprocal:
        p=run([exe,'--reciprocal']);assert p.returncode==0,p.stdout+p.stderr
        result=json.loads(p.stdout)
        assert result['success'] and result['MXCSR_configurations']==2048 and result['all64_exception_masks']
        assert result['scalar_cases']==2228224 and result['packed_cases']==2228224
        assert result['native_MXCSR_unchanged'] and result['LastError_preserved']
        assert hashes=={p:digest(Path(p)) for p in hashes},'Fixture inputs changed'
        report=dict(success=True,result=result,codegen=codegen,source_hashes=hashes,selected_header=str(header),
            selected_header_sha256=digest(header),compiler=dict(path=str(clang),sha256=digest(clang)),commands=commands,
            executable_sha256=digest(exe),legacy_equivalence_claimed=False,mode='reciprocal',game_boot_proven=False)
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({k:v for k,v in report.items() if k not in ['source_hashes','commands','codegen']},indent=2));return
    p=run([exe]);assert p.returncode==0,p.stdout+p.stderr
    result=json.loads(p.stdout);assert result['success'] and result['cases']==678912 and result['candidate_native_differences']==0
    assert result['safe_mask_cases']==2048 and result['full_CPU_aliases']==512
    traps={}
    for case,flag in TRAPS.items():
        pair={}
        for implementation in ['native','candidate']:
            p=run([exe,'--trap',implementation,case]);assert not p.stderr,p.stderr
            record=json.loads(p.stdout);code=p.returncode&0xffffffff
            assert code==record['exception_code'] and code in [0xc000008d,0xc000008f,0xc0000090,0xc0000091,0xc0000093,0xc00002b4,0xc00002b5],(case,record,code)
            assert record['mxcsr']&(1<<flag) and not record['mxcsr']&(1<<(7+flag)),(case,record)
            assert record['opcode']==('f30f58c1' if case.startswith('add_') else 'f30f59c1'),(case,record)
            pair[implementation]=record
        assert pair['native']==pair['candidate'],(case,pair)
        traps[case]=pair
    fallback={}
    for case in ['add_snan','mul_snan']:
        p=run([exe,'--trap','fallback',case]);fallback[case]=dict(exit_code=p.returncode&0xffffffff,result=json.loads(p.stdout))
    assert hashes=={p:digest(Path(p)) for p in hashes},'Fixture inputs changed'
    report=dict(success=True,result=result,unsafe_unmasked_traps=traps,fallback_snan_observed=fallback,
        fallback_status_is_observation_only=True,codegen=codegen,source_hashes=hashes,selected_header=str(header),
        selected_header_sha256=digest(header),compiler=dict(path=str(clang),sha256=digest(clang)),commands=commands,
        executable_sha256=digest(exe),original_game_bytes_executed=False,game_boot_proven=False)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ['source_hashes','commands','codegen']},indent=2))


if __name__=='__main__':main()
