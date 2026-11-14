"""Prove the generated Windows direct-entry ABI with the production compiler and host."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/windows-direct-dispatch-runtime'
TESTS=ROOT/'tests'
from leaf_probe import windows_toolchain
from generate_android_direct_dispatch import once


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def fixed_gva_sources(header,host):
    """Use exact generated bodies; only the function names differ between TUs."""
    source=ROOT/'generated/frostpunk-x64/recomp_funcs_0820.c'
    assert digest(source)=='50375e9cf6dcfe2fd6a07d9213608df3f68f120dc72fabe539635dcd25ece1aa'
    text=source.read_text();names=['000140F6D9EB','000140F6D9F3','000140F6EDD9','000140F6EDE1']
    bodies=[]
    for name in names:
        start=text.index('void L_'+name+'(CPU *c)\n{')
        end=text.index('\n}\n',start)+3
        body=text[start:end]
        assert 'dispatch(' not in body.split('c->rsp += 8; return;',1)[0] and body.count('GVA(')==5
        bodies.append(body)
    outputs=[]
    for mode in ['dynamic','fixed']:
        preamble='#include "cpu64.h"\n'
        if mode=='fixed':preamble+='#include "'+header.as_posix()+'"\n'
        output=OUT/('fixed_gva_'+mode+'.c')
        output.write_text(preamble+'\n'.join(body.replace('void L_'+name+'(',
            'void fixed_gva_'+mode+'_'+str(index)+'(',1)
            for index,(name,body) in enumerate(zip(names,bodies))),encoding='utf-8',newline='\n')
        outputs.append(output)
    gate='    g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)game.preferred_base;\n    if(g_image_delta)stop("preferred guest address occupied; full relocation not validated");\n'
    assert host.read_text().count(gate)==1,'Actual Windows startup admission changed'
    declarations='\n'.join('void fixed_gva_'+mode+'_'+str(i)+'(CPU *c);'
        for mode in ['dynamic','fixed'] for i in range(4))
    glue=declarations+'\nuint64_t test_main_GVA_probe(uint64_t value);\nstatic void fixed_gva_actual_startup_gate(void) {\n'+gate+'}\n'
    return outputs,source,glue,dict(source_sha256=digest(source),entry_points=names,
        body_sha256={name:hashlib.sha256(body.encode()).hexdigest() for name,body in zip(names,bodies)},
        startup_gate_sha256=hashlib.sha256(gate.encode()).hexdigest())


def main(fixed_gva_header=None):
    OUT.mkdir(parents=True,exist_ok=True)
    candidate=ROOT/'build/windows-direct-dispatch-candidate'
    host=candidate/'host.direct.c'
    assert digest(host)=='7a086d63253d906073f29189f3ddd66228f24bd27e60d7cd08df19554aa7a913'
    manifest=json.loads((candidate/'source-map.json').read_text())
    assert digest(host)==manifest['host']['host_candidate_sha256'],'Direct host differs from source map'
    assert digest(ROOT/'native/windows/host.c')==manifest['host']['host_before_sha256'],'Direct canonical preimage differs'
    header=Path(fixed_gva_header) if fixed_gva_header else candidate/'generated-overlay/aot_dispatch.h'
    fixed_gva='#define GVA(abs) ((uint64_t)(abs))' in header.read_text().splitlines()
    assert fixed_gva_header is None or fixed_gva,'Requested fixed-GVA header is dynamic'
    config=json.loads((ROOT/'port.json').read_text())
    binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    include=OUT/'include';include.mkdir(exist_ok=True)
    overlay=include/'cpu64.h'
    overlay.write_text((ROOT/'native/runtime64/cpu64.h').read_text().replace(
        '#if defined(__aarch64__)','#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)',1),encoding='utf-8',newline='\n')
    comparator=include/'qsort_original_comparator.h'
    comparator_input=ROOT/'build/windows-qsort-nx-test/include/qsort_original_comparator.h'
    qsort_receipt=ROOT/'build/windows-qsort-nx-test/result.json'
    qsort_proof=json.loads(qsort_receipt.read_text())
    assert qsort_proof['success'] and digest(comparator_input)==qsort_proof['source_sha256'].get(str(comparator_input)), 'Qsort comparator differs from its native receipt'
    comparator.write_bytes(comparator_input.read_bytes())
    fixture=OUT/'fixture.c'
    text=(ROOT/'tests/windows_qsort_nx_fixture.c').read_text()
    text=once(text,'#include "../native/windows/host.c"','#include "'+host.as_posix()+'"')
    text=once(text,'#include "qsort_original_comparator.h"','#include "qsort_original_comparator.h"\n#include "'+(ROOT/'native/windows/shader_lookup.c').as_posix()+'"')
    text=once(text,'static unsigned expected_static_depth;','void test_direct_entry(CPU *c,uint64_t target,void (*linked)(CPU *));\nvoid test_normal_entry(CPU *c,uint64_t target);\nstatic unsigned direct_nested_mode;\nstatic void direct_forbidden(CPU *c);\nstatic void shader_append(CPU *c);\nstatic unsigned expected_static_depth;')
    text=once(text,'    dispatch(c,GVA(0x141c56220));','    if(direct_nested_mode)test_direct_entry(c,GVA(0x141c56220),static_leaf);\n    else test_normal_entry(c,GVA(0x141c56220));')
    text=once(text,'    {UINT64_C(0x141c561a0),comparator_entry}',
        '    {UINT64_C(0x141058000),shader_append},{UINT64_C(0x1410596a9),direct_forbidden},\n    {UINT64_C(0x1410597e9),direct_forbidden},\n    {UINT64_C(0x141c561a0),comparator_entry}')
    text=once(text,'const uint32_t recomp_dispatch_count=8;','const uint32_t recomp_dispatch_count=11;')
    fixed_sources=[];fixed_info={};fixed_glue=''
    if fixed_gva:fixed_sources,fixed_source,fixed_glue,fixed_info=fixed_gva_sources(header,host)
    text=once(text,'int wmain(void) {',fixed_glue+'#define TEST_FIXED_GVA '+str(int(fixed_gva))+'\n#include "'+(TESTS/'windows_direct_dispatch_cases.inc').as_posix()+'"\n\nint wmain(int argc,wchar_t **argv) {')
    marker='    HMODULE crt=LoadLibraryW('
    if text.count(marker)!=1:raise ValueError('Fixture insertion marker differs')
    index=text.index(marker)
    text=text[:index]+'''    if(argc==2 && !wcsncmp(argv[1],L"--range-",8))direct_range_reject(argv[1]);
    #if TEST_FIXED_GVA
    if(argc==2 && !wcsncmp(argv[1],L"--gva-delta-",12))fixed_gva_reject(argv[1]);
    #endif
    if(argc==2 && !wcscmp(argv[1],L"--noncode")) {
        section.characteristics=IMAGE_SCN_MEM_READ;CPU c={0};uint64_t ret=0xe5efcafe;c.rsp=(uintptr_t)&ret;
        test_direct_entry(&c,(uintptr_t)image,direct_forbidden);assert(!"noncode admission accepted");
    }
'''+text[index:]
    text=once(text,'    self_test_mode=0;static_dispatch_cases(stack);','    self_test_mode=0;static_dispatch_cases(stack);direct_entry_cases(stack);direct_shader_cases_run(stack);direct_range_cases(stack);direct_range_metadata_cases();')
    if fixed_gva:text=once(text,'direct_range_metadata_cases();','direct_range_metadata_cases();fixed_gva_cases_run(stack);')
    text=once(text,'    FreeLibrary(implementation);FreeLibrary(crt);return 0;',
        '    FreeLibrary(implementation);FreeLibrary(crt);\n    printf("{\\"direct_static_cases\\":%u,\\"direct_native_cases\\":%u,\\"direct_self_cases\\":%u,\\"direct_shader_cases\\":%u}\\n",direct_static_cases,direct_native_cases,direct_self_cases,direct_shader_cases);return 0;')
    if fixed_gva:text=once(text,'direct_shader_cases);return 0;',
        'direct_shader_cases);printf("{\\"cases\\":%u,\\"scope_probes\\":%u}\\n",fixed_gva_cases,fixed_gva_scope_probes);return 0;')
    fixture.write_text(text,encoding='utf-8',newline='\n')
    entry=OUT/'entry.c'
    entry.write_text(once((TESTS/'windows_direct_dispatch_entry.c').read_text(),
        '#include "../build/windows-direct-dispatch-candidate/generated-overlay/aot_dispatch.h"',
        '#include "'+header.as_posix()+'"'),encoding='utf-8',newline='\n')
    sources=[fixture,entry,*fixed_sources,ROOT/'native/runtime64/eh64.c',
        *[p for p in sorted((ROOT/'native/windows').glob('*.c')) if p.name not in ('host.c','shader_lookup.c')],
        *sorted((ROOT/'native/windows').glob('*.asm'))]
    watched=set([*sources,host,candidate/'host.before.c',ROOT/'native/windows/host.c',
        ROOT/'tools/generate_windows_direct_dispatch.py',candidate/'source-map.json',candidate/'generated-overlay/aot_dispatch.h',ROOT/'tools/generate_android_direct_dispatch.py',
        ROOT/'native/windows/shader_lookup.c',ROOT/'tests/windows_qsort_nx_fixture.c',
        TESTS/'windows_direct_dispatch_cases.inc',TESTS/'windows_direct_dispatch_entry.c',header,Path(__file__).resolve(),ROOT/'tools/leaf_probe.py',ROOT/'port.json',overlay,comparator,comparator_input,qsort_receipt,
        *sorted((ROOT/'native/windows').glob('*.h')),*sorted((ROOT/'native/windows').glob('*.inc')),
        *sorted((ROOT/'native/runtime64').glob('*.h'))])
    if fixed_gva:watched.add(fixed_source)
    before={str(p):digest(p) for p in sorted(watched)}
    commands=[]
    def run(command):
        commands.append(list(map(str,command)))
        return subprocess.run(commands[-1],cwd=OUT,env=env,capture_output=True,text=True,
                              encoding='utf-8',errors='replace',timeout=60)
    objects=[]
    for source in sources:
        obj=OUT/(source.stem+'.obj');objects.append(obj)
        if source.suffix=='.asm':p=run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(obj),source])
        else:p=run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing',
            '-fwrapv','-ffp-contract=off','-DFROSTPUNK_WINDOWS_HOST=1','-include',overlay,
            *['-I'+str(p) for p in [include,ROOT/'native/runtime64',ROOT/'native/windows',ROOT/'generated/frostpunk-x64',ROOT/'generated/supplemental']],
            '-c',source,'-o',obj])
        assert p.returncode==0,p.stdout+p.stderr
    exe=OUT/'fixture.exe'
    p=run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/BASE:0x180000000','/DYNAMICBASE:NO',
        '/STACK:16777216','/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup',*objects,'libcmt.lib','oldnames.lib',
        'kernel32.lib','user32.lib','advapi32.lib','shell32.lib','ole32.lib','winmm.lib','version.lib','dbghelp.lib','d3d11.lib','dxgi.lib'])
    assert p.returncode==0,p.stdout+p.stderr
    p=run([exe]);assert p.returncode==0,p.stdout+p.stderr
    (OUT/'fixture.log').write_text(p.stdout+p.stderr,encoding='utf-8')
    lines=p.stdout.splitlines();qsort=json.loads(lines[0]);counts=json.loads(lines[1])
    assert qsort['success'] and counts==dict(direct_static_cases=18,direct_native_cases=9,direct_self_cases=9,direct_shader_cases=6)
    p=run([exe,'--noncode']);assert p.returncode==73 and 'STOP missing static target=' in p.stderr,p.stdout+p.stderr
    assert 'excluded linked entry' not in p.stderr
    (OUT/'noncode.log').write_text(p.stdout+p.stderr,encoding='utf-8')
    range_rejections={}
    for mode in ['--range-before','--range-gap','--range-data','--range-header','--range-after']:
        rejected=run([exe,mode])
        assert rejected.returncode==73 and 'STOP missing static target=' in rejected.stderr,(mode,rejected.stdout,rejected.stderr)
        assert 'excluded linked entry' not in rejected.stderr
        range_rejections[mode]=rejected.returncode
    fixed_gva_rejections={}
    if fixed_gva:
        fixed_info.update(json.loads(lines[2]))
        assert fixed_info['cases']==32 and fixed_info['scope_probes']==6
        for mode in ['--gva-delta-positive','--gva-delta-negative']:
            rejected=run([exe,mode])
            assert rejected.returncode==73 and 'STOP preferred guest address occupied; full relocation not validated' in rejected.stderr,(mode,rejected.stdout,rejected.stderr)
            assert 'FIXED_GVA_ENTRY_REACHED' not in rejected.stderr
            fixed_gva_rejections[mode]=rejected.returncode
            (OUT/(mode[2:]+'.log')).write_text(rejected.stdout+rejected.stderr,encoding='utf-8')
    assert before=={p:digest(Path(p)) for p in before},'Source changed during test'
    coverage={k:True for k in ['full_CPU_stack_LastError_MXCSR','recursive_depth_active_CPU','null_fallback',
        'self_fallback','nonzero_delta','noncode_native_fallback','shader_override_exclusions','initialized_zero_delta_fast_path','half_open_range_bounds',
        'header_data_gap_rejection','other_exec_lookup_fallback','uninitialized_lookup_fallback',
        'reset_unload_reinitialize','invalid_metadata_fail_closed','original_PE_endpoints']}
    if fixed_gva:coverage.update(fixed_GVA_actual_generated_full_CPU_stack_globals_LastError_MXCSR=True,
        fixed_GVA_private_main_scope=True,fixed_GVA_actual_startup_nonzero_rejected=True)
    proof=dict(success=True,coverage=coverage,counts=counts,source_hashes=before,
        executable_sha256=digest(exe),compiler={'path':str(clang),'sha256':digest(clang)},
        commands=commands,noncode_exit_code=p.returncode,qsort_regression=qsort,
        shader_code='production implementation with both real byte gates and synthetic append boundary',
        range_rejections=range_rejections,separate_TU_production_ABI=True,shader_oracle_equivalence=False,shader_body_zero_delta_only=True,canonical_modified=False,game_boot_proven=False,gameplay_FPS_measured=False)
    if fixed_gva:proof.update(fixed_gva=fixed_info,fixed_gva_rejections=fixed_gva_rejections)
    (OUT/'result.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in proof.items() if k not in ['source_hashes','commands','qsort_regression']},indent=2))


if __name__=='__main__':main()
