"""Validate native caches, concurrent invalidation and the native ABI on both compilers."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from test_windows_loader import compiler_environment

ROOT=Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def codegen(exe,map_path):
    import pefile
    from capstone import Cs,CS_ARCH_X86,CS_MODE_64
    pe=pefile.PE(str(exe),fast_load=True)
    symbols={name:int(address,16) for name,address in re.findall(
        r'^\s+\w+:\w+\s+(\S+)\s+([0-9a-fA-F]+)\s+f\b',map_path.read_text(),re.M)}
    addresses=sorted(set(symbols.values()));decoder=Cs(CS_ARCH_X86,CS_MODE_64);proof={}
    readers=['fp_native_target_valid','fp_native_stack_cached']
    for name in readers+['fp_native_validation_invalidate','fp_native_memory_mutation_begin','fp_native_memory_mutation_end']:
        address=symbols[name];end=next(value for value in addresses if value>address)
        instructions=decoder.disasm(pe.get_data(address-pe.OPTIONAL_HEADER.ImageBase,end-address),address)
        locks=[instruction.mnemonic+' '+instruction.op_str for instruction in instructions if instruction.mnemonic.startswith('lock ')]
        assert bool(locks)==(name not in readers),(name,locks)
        proof[name]=locks
    return proof


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    candidates=parser.add_mutually_exclusive_group()
    candidates.add_argument('--hook-candidate',action='store_true',help='Test the pinned isolated lower-bound index hint before promotion.')
    candidates.add_argument('--mutation-candidate',action='store_true',help='Test the pinned isolated mutation index hint before promotion.')
    args=parser.parse_args()
    compiler,environment=compiler_environment()
    output=ROOT/('build/windows-validation-mutation-candidate' if args.mutation_candidate else 'build/windows-validation-hook-candidate' if args.hook_candidate else 'build/windows-validation-test');output.mkdir(exist_ok=True)
    canonical=ROOT/'native/windows/native_validation.c'
    validation=ROOT/'build/windows-hook-name-candidate/native_validation.c' if args.hook_candidate else canonical
    if args.hook_candidate:assert sha(validation)=='a5d6cd5b0ddb162f961bb911ad0af509c9a049e6b6eadb325644d235190b9b0b','Hook candidate changed'
    if args.mutation_candidate:
        validation=ROOT/'build/windows-mutation-hint-candidate/native_validation.c'
        assert sha(validation)=='2750fe4082f4e957809e4231f0d8db81a66bbcacdc657c204af3e208f9ba8ac3','Mutation candidate changed'
    mutation_cases=ROOT/'tests/windows_validation_mutation_cases.inc'
    wrapped=output/'native_validation_cases.c'
    wrapped.write_text('#include "'+validation.as_posix()+'"\n#include "'+mutation_cases.as_posix()+'"\n',encoding='utf-8',newline='\n')
    fixture=ROOT/'tests/windows_validation_fixture.c'
    completed=ROOT/'tests/windows_validation_completed_fixture.c'
    hook=ROOT/'tests/windows_validation_epoch_hook.h'
    callbacks=ROOT/'native/windows/callback_stacks.c'
    inputs=[validation,wrapped,mutation_cases,fixture,completed,hook,callbacks,ROOT/'native/windows/bridge.asm',
            ROOT/'tests/windows_validation_target.c',Path(__file__).resolve(),
            ROOT/'native/windows/native_validation.h',ROOT/'native/windows/callback_stacks.h',canonical,
            ROOT/'port.json',ROOT/'tests/test_windows_loader.py',*sorted((ROOT/'native/runtime64').glob('*.h'))]
    pinned={str(path):sha(path) for path in inputs}
    # Negative control proves both race fixtures detect a missing final epoch check.
    text=validation.read_text();assert text.count(' && generation()==current')==2
    unfixed=output/'native_validation_unfixed.c'
    unfixed.write_text(text.replace(' && generation()==current',''),encoding='utf-8',newline='\n')
    config=json.loads((ROOT/'port.json').read_text())
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    def run(args):
        subprocess.run([str(x) for x in args],env=environment,cwd=output,check=True)
    dll=output/'validation_target.dll';bridge=output/'bridge.obj'
    run([compiler,'/nologo','/O2','/std:c11','/W4','/LD',ROOT/'tests/windows_validation_target.c',
         '/Fo'+str(output)+'/','/Fe'+str(dll)])
    run([compiler.parent/'ml64.exe','/nologo','/c','/Fo'+str(bridge),ROOT/'native/windows/bridge.asm'])
    results={}
    for name in ['msvc','clang']:
        folder=output/name;folder.mkdir(exist_ok=True)
        def compile_source(source,obj,with_hook=False):
            includes=[ROOT/'native/runtime64',ROOT/'native/windows']
            if name=='msvc':
                args=[compiler,'/nologo','/O2','/std:c11','/W4','/c',*['/I'+str(x) for x in includes]]
                if with_hook:args+=['/FI'+str(hook)]
                args+=[source,'/Fo'+str(obj)]
            else:
                args=[clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing',
                      '-fwrapv','-ffp-contract=off','-fno-math-errno','-D_CRT_SECURE_NO_WARNINGS',
                      *['-I'+str(x) for x in includes]]
                if with_hook:args+=['-include',hook]
                args+=['-c',source,'-o',obj]
            run(args)
        objects=[]
        for source in [fixture,wrapped,callbacks]:
            obj=folder/(source.stem+'.obj');compile_source(source,obj);objects.append(obj)
        exe=folder/'windows_validation_fixture.exe';map_path=folder/'windows_validation_fixture.map'
        run([compiler.parent/'link.exe','/NOLOGO',*objects,bridge,'libcmt.lib','oldnames.lib','kernel32.lib',
             '/OUT:'+str(exe),'/MAP:'+str(map_path)])
        result=subprocess.run([str(exe),str(dll)],capture_output=True,text=True,check=True,timeout=30)
        (folder/'stdout.log').write_text(result.stdout);(folder/'stderr.log').write_text(result.stderr)
        rows=[json.loads(line) for line in result.stdout.splitlines()]
        assert len(rows)==3 and rows[0]['owned_stack_cache'] and rows[1]['held_mutation_concurrency'] and rows[-1]['pass']
        assert rows[-1]['hook_lookup_cases']==37103 and rows[-1]['hook_thread_cases']==16384
        assert rows[-1]['hook_guard_endpoints'] and rows[-1]['hook_live_names_tables_counts']
        assert rows[-1]['mutation_lookup_cases']==21005 and rows[-1]['mutation_thread_cases']==16384
        assert rows[-1]['mutation_original_table_restored']
        proof=rows[-1];proof['owned_callback_stack']=rows[0];proof['held_mutation']=rows[1]
        proof['compiler_sha256']=sha(compiler if name=='msvc' else clang);proof['executable_sha256']=sha(exe)
        proof['locked_instructions']=codegen(exe,map_path)
        fixture_obj=folder/'completed_mutation.obj';compile_source(completed,fixture_obj)
        proof['completed_mutation']={}
        for variant,source in [('unfixed_control',unfixed),('fixed',validation)]:
            obj=folder/(variant+'.obj');compile_source(source,obj,with_hook=True)
            regression=folder/('completed_mutation_'+variant+'.exe')
            run([compiler.parent/'link.exe','/NOLOGO',fixture_obj,obj,objects[-1],
                 'libcmt.lib','oldnames.lib','kernel32.lib','/OUT:'+str(regression)])
            cases={}
            for case in ['code','stack']:
                result=subprocess.run([str(regression),str(dll),case],capture_output=True,text=True,timeout=30)
                expected=variant=='fixed'
                assert result.returncode==(0 if expected else 1),(variant,case,result.returncode,result.stderr)
                check=json.loads(result.stdout)
                assert check['pass']==expected and check['hook_calls']==1 and check['last_error_preserved']
                cases[case]=check
            proof['completed_mutation'][variant]={'executable_sha256':sha(regression),'cases':cases}
        results[name]=proof
    assert pinned=={str(path):sha(path) for path in inputs},'Validation inputs changed during the test'
    report={**results['msvc'],'compilers':results,'source_hashes':pinned,'game_launched':False}
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
