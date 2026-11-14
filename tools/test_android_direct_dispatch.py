"""Isolated shared-entry proof. No production edits, device actions or AOT rebuild."""
import difflib
import hashlib
import json
from pathlib import Path
import re
import statistics
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain
from test_android_dispatch_namespace import dispatch_addresses
import generate_android_direct_dispatch as direct

OUT=direct.OUT


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def once(text,old,new):return direct.once(text,old,new)
def function(text,signature):
    start=text.index(signature);at=text.index('{',start)+1;depth=1
    while depth:depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[start:at]


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    bootstrap=ROOT/'android/host/bootstrap.c';raw=bootstrap.read_bytes();text=bootstrap.read_text()
    newline='\r\n' if b'\r\n' in raw else '\n'
    if text.replace('\n',newline).encode()!=raw:raise ValueError('Mixed bootstrap newlines')
    candidate=direct.bootstrap_candidate(text)
    (OUT/'bootstrap.before.c').write_bytes(raw);(OUT/'bootstrap.candidate.c').write_text(candidate,newline=newline)
    (OUT/'bootstrap.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),candidate.splitlines(True),fromfile='android/host/bootstrap.c',tofile='android/host/bootstrap.c')))
    (OUT/'aot_dispatch.h').write_text(direct.HEADER,newline='\n')
    main_path=ROOT/'generated/frostpunk-x64/recomp_dispatch.c';header=ROOT/'generated/frostpunk-x64/recomp_funcs.h'
    meta_path=ROOT/'generated/frostpunk-x64/recomp_meta.json';meta=json.loads(meta_path.read_text())
    if sha(main_path)!=meta['generated_files'][main_path.name] or sha(header)!=meta['generated_files'][header.name]:raise ValueError('Sealed dispatch metadata differs')
    bindings=direct.binding_table(main_path.read_text(),header.read_text())
    normalization_tests=0
    sample='#include "recomp_funcs.h"\nvoid sample(CPU*c){dispatch(c, 0x140E33250ull);dispatch(c, c->rax);dispatch(c, 0x140D95650ull);dispatch(c, 0x141BABD20ull);dispatch(c, 0x140000001ull);}\n'
    rewritten,counts=direct.rewrite_unit(sample,bindings)
    if counts!={'eligible':1,'manual':1,'openal':1,'other':1,'ordered_bindings_sha256':counts['ordered_bindings_sha256']}:raise ValueError('Source exclusion fixture differs')
    for bad in [rewritten.replace('L_000140E33250','L_000140E33410'),rewritten.replace('0x140E33250ull','0x140E33251ull'),rewritten+'\n/* unapproved drift */',rewritten.replace('dispatch(c, c->rax)','dispatch(c, c->rbx)')]:
        try:direct.verify_unit(sample,bad,bindings,1)
        except ValueError:normalization_tests+=1
        else:raise ValueError('Wrong pair/source drift was accepted before compilation')
    first=text.index('/* Isolated candidate: immutable after check_dispatch_table')
    end=text.index('\ntypedef struct {',first)
    for name,whole in [('baseline',text),('candidate',candidate)]:
        body=whole[first:whole.index('\ntypedef struct {',first)]+'\n'+function(whole,'static void check_dispatch_table(void)')+'\n'
        for old,new in [('main_static_lookup',name+'_main_lookup'),('fp_dispatch_main',name+'_fp_dispatch_main'),
            ('dispatch_resolved',name+'_dispatch_resolved'),('dispatch_jmp',name+'_dispatch_jmp'),('dispatch',name+'_dispatch'),
            ('call_depth',name+'_depth'),('dispatch_calls',name+'_calls'),('dispatch_budget',name+'_budget'),('trace_calls',name+'_trace'),
            ('check_dispatch_table',name+'_check_dispatch_table')]:body=re.sub(r'\b'+old+r'\b',new,body)
        if name=='baseline':
            body=re.sub(r'\bMAIN_BUCKET_','BASELINE_MAIN_BUCKET_',body);body=re.sub(r'\bmain_bucket_','baseline_main_bucket_',body)
        for entry in [name+'_dispatch',name+'_dispatch_jmp']+([name+'_fp_dispatch_main'] if name=='candidate' else []):
            body=once(body,'void '+entry+'(','FP_NOINLINE void '+entry+'(')
        body=once(body,'    function(c);','    capture_call(function,c);')
        (OUT/('dispatch_'+name+'.inc')).write_text(body)
    # Reuse the established provider/namespace fixture. Only its call boundary
    # is captured for exact function identities; providers remain trace doubles.
    template=ROOT/'tests/android_dispatch_namespace_fixture.c';extension=ROOT/'tests/android_direct_dispatch_extension.inc'
    base=template.read_text();prefix=base[:base.index('#include "dispatch_baseline.inc"')]
    prefix=once(prefix,'#include <windows.h>','#include <windows.h>\n#include <xmmintrin.h>\n#include "region_pthread_win32.h"')
    prefix=once(prefix,'static void body(CPU *c,unsigned kind){record(20+kind);selected_body=kind;c->rax+=c->rip+kind;c->rsp+=8;}',
        'static void body(CPU *c,unsigned kind);')
    prefix+='#define DIRECT_EARLY 1\n#include "android_direct_dispatch_extension.inc"\n#undef DIRECT_EARLY\n#include "dispatch_baseline.inc"\n#include "dispatch_candidate.inc"\n'
    middle=base[base.index('typedef struct {CPU cpu;'):base.index('static void bench(unsigned iterations)')]
    middle=once(middle,'typedef struct {CPU cpu;','typedef struct {CPU cpu;fn_t called;')
    middle=once(middle,'    event_count=selected_body=0;fault_code=0;errno=2718;',
        '    event_count=selected_body=0;fault_code=0;errno=2718;captured_function=NULL;active_variant=candidate;recursive_remaining=recursive_limit;')
    middle=once(middle,'candidate_dispatch(&working,target)','direct_route(&working,target)')
    middle=once(middle,'outcome o={0};o.cpu=working;','outcome o={0};o.cpu=working;o.called=captured_function;')
    middle=once(middle,'&&a.error==2718&&b.error==2718;','&&a.error==2718&&b.error==2718&&a.called==b.called;')
    begin=middle.index('    if(eligible(t)&&(member(0,t)');last=middle.index('    same=same&&n==b.count',begin)
    middle=middle[:begin]+'''    for(unsigned i=0;i<a.count;++i)expected[n++]=a.trace[i];
'''+middle[last:]
    fixture=OUT/'direct-dispatch-fixture.c';fixture.write_text(prefix+middle+'\n#include "android_direct_dispatch_extension.inc"\n')
    # Canonical tables and provider envelopes are the same as the previous
    # namespace proof, regenerated from their pinned sources rather than copied.
    supplemental=ROOT/'generated/supplemental/recomp_supplemental.c'
    payload=ROOT/'generated/payload/recomp/recomp_dispatch.c';dependency=ROOT/'generated/dependencies/msvcp140/recomp/recomp_dispatch.c'
    audio=ROOT/'build/openal-bridge/openal_original_entries.h';imports_path=ROOT/'generated/frostpunk-x64/recomp_imports.c'
    extras=[ROOT/'native/windows'/n for n in ('extra_aot_bodies.inc','switch_aot_bodies.inc','switch_extension_bodies.inc')]
    audio_text=audio.read_text()
    tables=[list(bindings),[int(x,16) for x in re.findall(r'^\s*\{0x([0-9A-Fa-f]+)ull,frostpunk_supplemental_',supplemental.read_text(),re.M)],
        sorted({int(x,16) for path in extras for x in re.findall(r'\{UINT64_C\(0x([0-9a-fA-F]+)\),\d+,',path.read_text())}),
        dispatch_addresses(payload),dispatch_addresses(dependency),[int(x,16) for x in re.findall(r'^ UINT64_C\(0x([0-9a-fA-F]+)\)',audio_text,re.M)],
        sorted(int(x,16) for x in re.findall(r'X\([^\n]+UINT64_C\(0x([0-9a-fA-F]+)\)',audio_text)),sorted(direct.MANUAL)]
    if [len(t) for t in tables]!=[399917,1985,35,644,2194,157,30,5]:raise ValueError('Namespace counts changed')
    dataset=OUT/'tables.bin'
    with dataset.open('wb') as stream:
        stream.write(b'FPNDISP1'+struct.pack('<8I',*(len(t) for t in tables)))
        for table in tables:
            if table!=sorted(set(table)):raise ValueError('Unsorted/duplicate namespace keys')
            stream.write(struct.pack('<'+'Q'*len(table),*table))
    imports=re.findall(r'\{\s*(0x[0-9a-fA-F]+)ull,\s*("[^"\n]+"),\s*("[^"\n]+")\s*\}',imports_path.read_text())
    if len(imports)!=557 or sha(imports_path)!=meta['generated_files'][imports_path.name]:raise ValueError('Main IAT differs')
    (OUT/'dispatch_namespaces.inc').write_text('#define MAIN_BASE UINT64_C(0x140000000)\n#define MAIN_END UINT64_C(0x144539000)\n#define OPENAL_MIN UINT64_C(0x141babd20)\n#define OPENAL_MAX UINT64_C(0x141bc2d40)\nstatic const hle_import_t recomp_imports[]={\n'+''.join('{UINT64_C(%s),%s,%s},\n'%row for row in imports)+'};\n')
    # A real canonical TU proves the new prototype and symbol-address PIC
    # relocations without recompiling the full game before hardware timing.
    representative=ROOT/'generated/frostpunk-x64/recomp_funcs_0000.c'
    if sha(representative)!=meta['generated_files'][representative.name]:raise ValueError('Canonical representative changed')
    unit_text,unit_counts=direct.rewrite_unit(representative.read_bytes().decode(),bindings)
    representative_out=OUT/representative.name;representative_out.write_bytes(unit_text.encode())
    config=json.loads((ROOT/'port.json').read_text());llvm=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler=llvm/'clang.exe';bins,env=windows_toolchain();commands=[]
    pins=[bootstrap,main_path,header,meta_path,representative,template,extension,supplemental,payload,dependency,audio,imports_path,*extras,
          ROOT/'tools/generate_android_direct_dispatch.py',Path(__file__),ROOT/'tools/test_android_dispatch_namespace.py',
          ROOT/'tools/leaf_probe.py',ROOT/'tests/region_pthread_win32.h',ROOT/'native/runtime64/cpu64.h']
    before={str(path.relative_to(ROOT)):sha(path) for path in pins}
    if before[str(bootstrap.relative_to(ROOT))]!=hashlib.sha256(raw).hexdigest():raise ValueError('Bootstrap changed before qualification')
    for path in (main_path,header,representative,imports_path):
        if before[str(path.relative_to(ROOT))]!=meta['generated_files'][path.name]:raise ValueError('Canonical input changed before qualification')
    map_pins={};map_replacements,map_report=direct.verify_source_map(config,map_pins)
    if len(map_replacements)!=1000 or map_report['counts']!={'eligible':988974,'manual':41,'openal':3422,'other':37}:
        raise ValueError('Complete constant-call source-map scope differs')
    if map_replacements[representative.resolve()].read_bytes()!=representative_out.read_bytes():raise ValueError('Compiled representative is not the exact final overlay')
    before.update({str(path.relative_to(ROOT)):value for path,value in map_pins.items()})
    def run(command,expected=0):
        command=list(map(str,command));commands.append(command)
        p=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=180)
        if p.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps(dict(command=command,expected=expected,actual=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2)+'\n')
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+p.stdout[-2000:]+p.stderr[-7000:])
        return p
    flags=['-std=c11','-O2','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-I'+str(ROOT/'native/runtime64'),'-I'+str(OUT),'-I'+str(ROOT/'tests')]
    def host_build(name,extra=()):
        obj=OUT/(name+'.obj');binary=OUT/(name+'.exe')
        run([compiler,'--target=x86_64-pc-windows-msvc',*flags,*extra,'-c',fixture,'-o',obj])
        run([bins/'link.exe','/nologo','/out:'+str(binary),obj,'libcmt.lib','oldnames.lib','kernel32.lib']);return binary
    host=host_build('direct-host');p=run([host,dataset]);(OUT/'host.log').write_text(p.stdout+p.stderr);semantic=json.loads(p.stdout)
    if not semantic['success'] or semantic['failures'] or semantic['parallel_cases']!=8192:raise ValueError('Shared-entry host proof failed')
    pointers={option:json.loads(run([host,dataset,option]).stdout) for option in ('--pointers','--aliases')}
    bench_host=host_build('benchmark-host',['-DFP_DISPATCH_BENCHMARK=1']);bench=json.loads(run([bench_host,dataset,'200000']).stdout)
    for row in bench['scenarios']:row['median_speedup']=statistics.median(row['baseline_ns'])/statistics.median(row['candidate_ns'])
    (OUT/'host-benchmark.json').write_text(json.dumps(bench,indent=2)+'\n')
    arms={}
    for name,extra in [('proof',[]),('benchmark',['-DFP_DISPATCH_BENCHMARK=1'])]:
        binary=OUT/('libdirect-'+name+'.so')
        run([compiler,'--target=aarch64-linux-android30',*flags,*extra,'-fPIC','-shared','-Dmain=fp_bucket_fixture_main',fixture,
             '-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
        arms[name]=dict(path=str(binary.relative_to(ROOT)),sha256=sha(binary))
    runner_source=ROOT/'build/android-static-lookup-analysis/dso_runner.c';runner=OUT/'direct-runner-arm64'
    before[str(runner_source.relative_to(ROOT))]=sha(runner_source)
    run([compiler,'--target=aarch64-linux-android30','-std=c11','-O2',runner_source,'-ldl','-Wl,-z,max-page-size=16384','-o',runner])
    unit_object=OUT/'representative-arm64.o'
    run([compiler,'--target=aarch64-linux-android30',*flags,'-fPIC','-fvisibility=hidden','-I'+str(ROOT/'generated/frostpunk-x64'),'-c',representative_out,'-o',unit_object])
    reloc=run([llvm/'llvm-readelf.exe','-r',unit_object]);(OUT/'representative-relocations.txt').write_text(reloc.stdout)
    # Undefined L declarations initially use normal GOT relocations. Hidden
    # definitions in the full game let lld relax them to local addresses. Prove
    # that final-link property with nonexecuted definitions for this one TU.
    undefined=run([llvm/'llvm-nm.exe','-u',unit_object]).stdout
    external_symbols=sorted(set(re.findall(r'\bL_[0-9A-Fa-f]+\b',undefined)))
    symbol_source=OUT/'pic-link-symbols.c'
    symbol_source.write_text('#include "aot_dispatch.h"\n'+''.join('__attribute__((visibility("hidden"))) void '+symbol+'(CPU*c){(void)c;}\n' for symbol in external_symbols)+
        'void fp_dispatch_main(CPU*c,uint64_t t,void(*linked)(CPU*)){(void)c;(void)t;(void)linked;}\n')
    pic_library=OUT/'libdirect-pic-link.so'
    run([compiler,'--target=aarch64-linux-android30',*flags,'-fPIC','-shared','-fvisibility=hidden',unit_object,symbol_source,
        '-Wl,-z,max-page-size=16384','-o',pic_library])
    final_relocations=run([llvm/'llvm-readelf.exe','-r',pic_library]).stdout
    if re.search(r'\b(?:L_[0-9A-Fa-f]+|fp_dispatch_main)\b',final_relocations):raise ValueError('Known linked symbols acquired dynamic relocations')
    (OUT/'final-pic-relocations.txt').write_text(final_relocations)
    disasm=run([llvm/'llvm-objdump.exe','-d','--no-show-raw-insn',OUT/'libdirect-benchmark.so']);(OUT/'arm-dso-disassembly.txt').write_text(disasm.stdout)
    if before!={name:sha(ROOT/name) for name in before}:raise ValueError('Source drift while qualifying shared dispatcher')
    generated={str(path.relative_to(ROOT)):sha(path) for path in OUT.iterdir() if path.suffix in ('.c','.inc','.h','.patch','.bin')}
    result=dict(success=True,source_hashes=before,bootstrap_before_sha256=sha(bootstrap),bootstrap_candidate_sha256=sha(OUT/'bootstrap.candidate.c'),
        exact_shared_body_roundtrip=True,normalization_negative_tests=normalization_tests,host=semantic,pointer_proofs=pointers,host_model_benchmark=bench,
        namespace_counts=[len(t) for t in tables],representative=dict(canonical=str(representative.relative_to(ROOT)),counts=unit_counts,
            object_path=str(unit_object.relative_to(ROOT)),object_sha256=sha(unit_object),relocations=str((OUT/'representative-relocations.txt').relative_to(ROOT)),
            final_link_probe=str(pic_library.relative_to(ROOT)),final_link_sha256=sha(pic_library),hidden_external_definitions=len(external_symbols),
            known_symbol_dynamic_relocations=0,probe_executed=False),
        arm=dict(runner=dict(path=str(runner.relative_to(ROOT)),sha256=sha(runner)),**arms,
            proof_args=['./libdirect-proof.so','tables.bin'],pointer_args=['./libdirect-proof.so','tables.bin','--pointers'],
            alias_args=['./libdirect-proof.so','tables.bin','--aliases'],benchmark_args=['./libdirect-benchmark.so','tables.bin','200000']),
        dataset=dict(path=str(dataset.relative_to(ROOT)),sha256=sha(dataset)),generated=generated,commands=commands,
        source_map=dict(path=str((OUT/'source-map.json').relative_to(ROOT)),sha256=sha(OUT/'source-map.json'),units=1000,
            counts=map_report['counts'],unique_targets=map_report['unique_target_count'],upstream_units=53,
            exact_all_units_reconstructed=True,all_TUs_compiled=False),
        production_modified=False,device_executed=False,game_gain_proven=False,
        scope='Actual current shared bootstrap bodies. Providers/AOT bodies are ordered doubles; separate synthetic pointer identities never execute. Known VA/function pairing is a checked source-generation contract; invalid pairs reject before compilation. No native binding table or lookup on known hits.')
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(success=True,host=semantic,pointers=pointers,benchmark=[dict(name=r['name'],speedup=r['median_speedup']) for r in bench['scenarios']],
        arm=result['arm'],receipt_sha256=sha(OUT/'result.json')),indent=2))


if __name__=='__main__':main()
