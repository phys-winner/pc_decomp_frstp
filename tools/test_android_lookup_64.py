"""Isolated current256 versus64-byte bucket geometry; no production writes/ADB."""
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

OUT = ROOT / 'build/android-lookup-64-test'
BASELINE_SHA = '03d5362bbce29f5c7791f86b62e9a0bd2fda9317b31377de5d82d831f57be901'
BENCHMARK = ROOT / 'tests/android_lookup_64_benchmark.inc'
def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Source extraction anchor differs: ' + before[:90])
    return text.replace(before, after, 1)


def function(text, signature):
    start = text.index(signature)
    at = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[start:at]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = ndk / 'clang.exe'
    source = ROOT / 'android/host/bootstrap.c'
    raw = source.read_bytes();text = source.read_text();newline = '\r\n' if b'\r\n' in raw else '\n'
    if text.replace('\n', newline).encode() != raw:
        raise ValueError('Unexpected mixed bootstrap newline style')
    if sha(source)!=BASELINE_SHA:raise ValueError('Current256 bootstrap baseline differs')
    (OUT / 'bootstrap.before.c').write_bytes(raw)
    candidate=once(text,'#define MAIN_BUCKET_SHIFT 8u','#define MAIN_BUCKET_SHIFT 6u')
    old_count='#define MAIN_BUCKET_COUNT ((MAIN_BUCKET_BYTES+255u)>>MAIN_BUCKET_SHIFT)'
    new_count='#define MAIN_BUCKET_COUNT ((MAIN_BUCKET_BYTES+((UINT64_C(1)<<MAIN_BUCKET_SHIFT)-1u))>>MAIN_BUCKET_SHIFT)'
    candidate=once(candidate,old_count,new_count)
    restored=once(once(candidate,'#define MAIN_BUCKET_SHIFT 6u','#define MAIN_BUCKET_SHIFT 8u'),new_count,old_count)
    if restored.replace('\n',newline).encode()!=raw:raise ValueError('Geometry reversal differs outside approved lines')
    (OUT / 'bootstrap_candidate.c').write_text(candidate,newline=newline)
    (OUT / 'candidate.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),candidate.splitlines(True),fromfile='android/host/bootstrap.c',tofile='android/host/bootstrap.c')))
    start=text.index('/* Isolated candidate: immutable after check_dispatch_table')
    end=text.index('void dispatch_jmp(CPU *c, uint64_t target)',start)
    for name,whole in [('baseline',text),('candidate',candidate)]:
        body=whole[start:whole.index('void dispatch_jmp(CPU *c, uint64_t target)',start)]
        body+='\n'+function(whole,'static void check_dispatch_table(void)')+'\n'
        for old,new in [('main_static_lookup',name+'_main_lookup'),('dispatch',name+'_dispatch'),('call_depth',name+'_depth'),('dispatch_calls',name+'_calls'),('dispatch_budget',name+'_budget'),('trace_calls',name+'_trace'),('check_dispatch_table',name+'_check_dispatch_table')]:
            body=re.sub(r'\b'+old+r'\b',new,body)
        if name=='baseline':
            body=re.sub(r'\bMAIN_BUCKET_', 'BASELINE_MAIN_BUCKET_',body)
            body=re.sub(r'\bmain_bucket_', 'baseline_main_bucket_',body)
        body=once(body,'void '+name+'_dispatch(','FP_NOINLINE void '+name+'_dispatch(')
        (OUT / ('dispatch_'+name+'.inc')).write_text(body)

    main_path = ROOT / 'generated/frostpunk-x64/recomp_dispatch.c'
    supplemental_path = ROOT / 'generated/supplemental/recomp_supplemental.c'
    payload_path = ROOT / 'generated/payload/recomp/recomp_dispatch.c'
    dependency_path = ROOT / 'generated/dependencies/msvcp140/recomp/recomp_dispatch.c'
    openal_path = ROOT / 'build/openal-bridge/openal_original_entries.h'
    imports_path = ROOT / 'generated/frostpunk-x64/recomp_imports.c'
    extra_paths = [ROOT / 'native/windows' / name for name in ['extra_aot_bodies.inc', 'switch_aot_bodies.inc', 'switch_extension_bodies.inc']]
    meta_path = ROOT / 'generated/frostpunk-x64/recomp_meta.json';meta = json.loads(meta_path.read_text())
    if meta['original_exe_sha256'] != cfg['original_exe_sha256'] or sha(main_path) != meta['generated_files']['recomp_dispatch.c'] or meta['size_of_image'] != 0x4539000:
        raise ValueError('Sealed main table identity differs')
    supplemental = [int(x, 16) for x in re.findall(r'^\s*\{0x([0-9A-Fa-f]+)ull,frostpunk_supplemental_', supplemental_path.read_text(), re.M)]
    extra = sorted({int(x, 16) for path in extra_paths for x in re.findall(r'\{UINT64_C\(0x([0-9a-fA-F]+)\),\d+,', path.read_text())})
    openal = [int(x, 16) for x in re.findall(r'^ UINT64_C\(0x([0-9a-fA-F]+)\)', openal_path.read_text(), re.M)]
    typed = sorted(int(x, 16) for x in re.findall(r'X\([^\n]+UINT64_C\(0x([0-9a-fA-F]+)\)', openal_path.read_text()))
    manual = sorted([0x140e222e0, 0x140d95650, 0x140f499a0, 0x1410596a9, 0x1410597e9])
    tables = [dispatch_addresses(main_path), supplemental, extra, dispatch_addresses(payload_path), dispatch_addresses(dependency_path), openal, typed, manual]
    if [len(table) for table in tables] != [399917, 1985, 35, 644, 2194, 157, 30, 5]:
        raise ValueError('Namespace table counts differ')
    data_path = OUT / 'tables.bin'
    with data_path.open('wb') as stream:
        stream.write(b'FPNDISP1' + struct.pack('<8I', *(len(table) for table in tables)))
        for table in tables:
            if table != sorted(set(table)):
                raise ValueError('Immutable table is not unique/sorted')
            stream.write(struct.pack('<' + 'Q' * len(table), *table))
    imports = re.findall(r'\{\s*(0x[0-9a-fA-F]+)ull,\s*("[^"\n]+"),\s*("[^"\n]+")\s*\}', imports_path.read_text())
    if len(imports) != 557:
        raise ValueError('Main import count differs')
    (OUT / 'dispatch_namespaces.inc').write_text('#define MAIN_BASE UINT64_C(0x140000000)\n#define MAIN_END UINT64_C(0x144539000)\n#define OPENAL_MIN UINT64_C(0x141babd20)\n#define OPENAL_MAX UINT64_C(0x141bc2d40)\nstatic const hle_import_t recomp_imports[]={\n' + ''.join('{UINT64_C(%s),%s,%s},\n' % row for row in imports) + '};\n')
    template_path = ROOT / 'tests/android_dispatch_namespace_fixture.c'
    extension_path = ROOT / 'tests/android_lookup_bucket_extension.inc'
    fixture = template_path.read_text()
    fixture = once(fixture, '#include <windows.h>', '#include <windows.h>\n#include <xmmintrin.h>')
    trace_start = fixture.index('    if(eligible(t)&&(member(0,t)')
    trace_end = fixture.index('    same=same&&n==b.count', trace_start)
    fixture = fixture[:trace_start] + '''    unsigned actual[128],actual_count=0;
    for(unsigned i=0;i<a.count;++i)expected[n++]=a.trace[i];
    for(unsigned i=0;i<b.count;++i)actual[actual_count++]=b.trace[i];
''' + fixture[trace_end:]
    fixture = once(fixture, 'n==b.count&&!memcmp(expected,b.trace,n*sizeof(*expected))',
        'n==actual_count&&!memcmp(expected,actual,n*sizeof(*expected))')
    fixture = once(fixture, 'static void bench(unsigned iterations){', '#include "android_lookup_bucket_extension.inc"\nstatic void unused_old_bench(unsigned iterations){')
    fixture = once(fixture, '    if(argc<2||!load(argv[1]))return 2;', '''    if(argc<2||!load(argv[1]))return 2;
    if(argc==3&&!strncmp(argv[2],"--bad-",6))return bucket_negative(argv[2]);
    if(argc==3&&!strcmp(argv[2],"--pointers"))return pointer_proof(0);
    if(argc==3&&!strcmp(argv[2],"--aliases"))return pointer_proof(1);
    for(unsigned i=0;i<32;++i)compare(tables[0][i*997u%counts[0]],i,0,0,0);
    bucket_initialize();''')
    fixture = once(fixture, 'bench(iterations);return 0;', 'bucket_bench(iterations);return 0;')
    fixture = once(fixture, '    for(unsigned k=1;k<=4;++k)', '''    for(unsigned i=0;i<counts[0];++i){compare(tables[0][i]-1,i,0,0,0);compare(tables[0][i]+1,i,0,0,0);}
    uint64_t random_state=UINT64_C(0x256b0c37);for(unsigned i=0;i<65536;++i)compare(MAIN_BASE+random_step(&random_state)%MAIN_BUCKET_BYTES,i,0,0,0);
    for(unsigned k=1;k<=4;++k)''')
    fixture = once(fixture, '    const uint64_t spaces[][2]=', '''    for(unsigned ready=0;ready<2;++ready){manual_ready=(int)ready;for(unsigned j=0;j<3;++j){uint64_t t=(uint64_t[]){UINT64_C(0x140e222e0),UINT64_C(0x140d95650),UINT64_C(0x140f499a0)}[j];for(int d=-2;d<=2;++d)compare(t+d,j,0,0,0);}}manual_ready=1;
    const uint64_t spaces[][2]=''')
    final_line = next(line for line in fixture.splitlines() if line.startswith('    printf("{\\"success\\":%s,\\"cases\\":'))
    fixture = once(fixture, final_line, '    bucket_fp_cases();bucket_parallel();bucket_report();return failures?1:0;')
    fixture_path = OUT / 'lookup_bucket_fixture.c';fixture_path.write_text(fixture)
    extension=extension_path.read_text()
    extension=once(extension,'pointer_pair(target+255)','pointer_pair(target+((UINT64_C(1)<<MAIN_BUCKET_SHIFT)-1u))')
    extension=extension.replace('if(a.trace[j]!=13&&a.trace[j]!=14)','').replace('if(b.trace[j]!=13&&b.trace[j]!=14)','')
    extension=extension.replace('static void bucket_bench(unsigned iterations){','static void unused_old_bucket_bench(unsigned iterations){',1)
    extension+='\n#include "android_lookup_64_benchmark.inc"\n'
    (OUT / extension_path.name).write_text(extension)
    (OUT / BENCHMARK.name).write_bytes(BENCHMARK.read_bytes())
    runner_source = ROOT / 'build/android-static-lookup-analysis/dso_runner.c'
    profile_record=ROOT / 'build/app-game/ultralow-active-run/clear-city-profile-record.json'
    residency=ROOT / 'build/app-game/ultralow-active-run/clear-city-lookup-residency.json'
    residency_info=json.loads(residency.read_text())
    if residency_info['profile_sha256']!='72916fc725b8474fb06314036d3f6a69bb014ebc8e8e1ec41372bbc191859c3a':
        raise ValueError('Fixed hot-entry provenance differs')
    benchmark_inputs=dict(profile_path=residency_info['profile_path'],profile_sha256=residency_info['profile_sha256'],
        sampled_cycles=residency_info['cycles'],source='Pinned clear-city self-sample caller residency; target frequencies not recovered',
        fixed={},random_seed='0x64256b0c37',weights='Synthetic; no actual target-frequency/FPS claim',
        cold=dict(eviction_bytes=33554432,eviction_outside_timing=True,one_pass=8192))
    benchmark_text=BENCHMARK.read_text()
    for name in ['renderer','main_entries']:
        body=re.search(r'static const uint64_t '+name+r'\[\]=\{(.*?)\};',benchmark_text,re.S).group(1)
        targets=[int(value,16) for value in re.findall(r'UINT64_C\((0x[0-9a-f]+)\)',body)]
        if len(targets)!=8 or not set(targets)<=set(tables[0]):raise ValueError('Fixed benchmark entries differ')
        benchmark_inputs['fixed'][name]=[hex(value) for value in targets]
    benchmark_inputs_path=OUT / 'benchmark-inputs.json'
    benchmark_inputs_path.write_text(json.dumps(benchmark_inputs,indent=2)+'\n')
    pins = [source, BENCHMARK, profile_record, residency, template_path, extension_path, runner_source, Path(__file__), ROOT / 'tools/test_android_lookup_buckets.py', ROOT / 'tools/test_android_dispatch_namespace.py',
            ROOT / 'tools/leaf_probe.py', ROOT / 'native/runtime64/cpu64.h', ROOT / 'android/host/extra_aot.c',
            ROOT / 'android/host/string_resize_aot.c', ROOT / 'android/host/texture_formats.c', *extra_paths,
            main_path, supplemental_path, payload_path, dependency_path, openal_path, imports_path, meta_path]
    before = {str(path.relative_to(ROOT)): sha(path) for path in pins}
    bins, env = windows_toolchain();commands = []
    def run(command, expected=0):
        command = list(map(str, command));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=180)
        if result.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps(dict(command=command, expected=expected, actual=result.returncode,
                stdout=result.stdout, stderr=result.stderr), indent=2) + '\n')
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-3000:] + result.stderr[-5000:])
        return result
    flags = ['-std=c11', '-O2', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off',
             '-I' + str(ROOT / 'native/runtime64'), '-I' + str(OUT)]
    host = OUT / 'lookup-buckets-host.exe';obj = OUT / 'lookup-buckets-host.obj'
    run([compiler, '--target=x86_64-pc-windows-msvc', *flags, '-c', fixture_path, '-o', obj])
    run([bins / 'link.exe', '/nologo', '/out:' + str(host), obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_run = run([host, data_path]);host_proof = json.loads(host_run.stdout)
    (OUT / 'host.log').write_text(host_run.stdout + host_run.stderr)
    if not host_proof['success'] or host_proof['all_main_entries'] != 399917 or host_proof['failures']:
        raise ValueError('Full CPU/namespace comparison failed')
    pointers = json.loads(run([host, data_path, '--pointers']).stdout)
    aliases = json.loads(run([host, data_path, '--aliases']).stdout)
    negatives = ['--bad-null', '--bad-order', '--bad-low', '--bad-high']
    for option in negatives:
        run([host, data_path, option], expected=73)
    benchmark_host = OUT / 'lookup-buckets-bench-host.exe';bench_obj = OUT / 'lookup-buckets-bench-host.obj'
    run([compiler, '--target=x86_64-pc-windows-msvc', *flags, '-DFP_DISPATCH_BENCHMARK=1', '-c', fixture_path, '-o', bench_obj])
    run([bins / 'link.exe', '/nologo', '/out:' + str(benchmark_host), bench_obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    bench_run = run([benchmark_host, data_path, '200000']);bench = json.loads(bench_run.stdout)
    for row in bench['scenarios']:
        row['median_baseline_ns_per_call'] = statistics.median(row['baseline_ns']) / row['calls_per_round']
        row['median_candidate_ns_per_call'] = statistics.median(row['candidate_ns']) / row['calls_per_round']
        row['median_speedup'] = row['median_baseline_ns_per_call'] / row['median_candidate_ns_per_call']
    (OUT / 'host-benchmark.json').write_text(json.dumps(bench, indent=2) + '\n')
    arm = OUT / 'lookup-buckets-arm64';arm_bench = OUT / 'lookup-buckets-bench-arm64'
    for path, options in [(arm, []), (arm_bench, ['-DFP_DISPATCH_BENCHMARK=1'])]:
        run([compiler, '--target=aarch64-linux-android30', *flags, *options, fixture_path,
             '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', path])
    disassembly = run([ndk / 'llvm-objdump.exe', '-d', '--no-show-raw-insn', arm_bench])
    (OUT / 'arm-disassembly.txt').write_text(disassembly.stdout)
    arm_bench_dso = OUT / 'liblookup-buckets-bench.so'
    arm_proof_dso = OUT / 'liblookup-buckets-proof.so'
    run([compiler, '--target=aarch64-linux-android30', *flags, '-fPIC', '-shared',
         '-Dmain=fp_bucket_fixture_main', fixture_path,
         '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', arm_proof_dso])
    run([compiler, '--target=aarch64-linux-android30', *flags, '-fPIC', '-shared',
         '-DFP_DISPATCH_BENCHMARK=1', '-Dmain=fp_bucket_fixture_main', fixture_path,
         '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', arm_bench_dso])
    runner = OUT / 'lookup-buckets-dso-runner'
    run([compiler, '--target=aarch64-linux-android30', '-std=c11', '-O2', runner_source,
         '-Wl,-z,max-page-size=16384', '-ldl', '-o', runner])
    dso_disassembly = run([ndk / 'llvm-objdump.exe', '-d', '--no-show-raw-insn', arm_bench_dso])
    (OUT / 'arm-dso-disassembly.txt').write_text(dso_disassembly.stdout)
    if before != {name: sha(ROOT / name) for name in before}:
        raise ValueError('Qualification source drift')
    report = dict(success=True, source_pins=before, whole_bootstrap_round_trip_sha256=hashlib.sha256(raw).hexdigest(),
        original_exe_sha256=cfg['original_exe_sha256'], namespace_counts=[len(table) for table in tables],
        host=host_proof, unique_pointer_proof=pointers, shared_alias_proof=aliases, negative_args=negatives,
        host_model_benchmark=bench, commands=commands,
        dataset=dict(path=str(data_path.relative_to(ROOT)), sha256=sha(data_path)),
        benchmark_inputs=dict(path=str(benchmark_inputs_path.relative_to(ROOT)),sha256=sha(benchmark_inputs_path)),
        arm=dict(path=str(arm.relative_to(ROOT)), sha256=sha(arm), args=['tables.bin']),
        arm_benchmark=dict(path=str(arm_bench.relative_to(ROOT)), sha256=sha(arm_bench), args=['tables.bin', '200000']),
        arm_dynamic_TLS_proof=dict(runner=dict(path=str(runner.relative_to(ROOT)), sha256=sha(runner)),
            dso=dict(path=str(arm_proof_dso.relative_to(ROOT)), sha256=sha(arm_proof_dso)),
            args=['./liblookup-buckets-proof.so', 'tables.bin'],
            pointer_args=['./liblookup-buckets-proof.so', 'tables.bin', '--pointers'],
            alias_args=['./liblookup-buckets-proof.so', 'tables.bin', '--aliases'],
            negative_args=['--bad-null', '--bad-order', '--bad-low', '--bad-high']),
        arm_dynamic_TLS_benchmark=dict(runner=dict(path=str(runner.relative_to(ROOT)), sha256=sha(runner)),
            dso=dict(path=str(arm_bench_dso.relative_to(ROOT)), sha256=sha(arm_bench_dso)),
            args=['./liblookup-buckets-bench.so', 'tables.bin', '200000'],
            reason='Dlopen shared fixture preserves global-dynamic TLSDESC; CLI local-exec TLS is not the actual game TLS cost.'),
        generated={str(path.relative_to(ROOT)): sha(path) for path in OUT.iterdir() if path.suffix in ('.inc', '.c', '.patch')},
        arm_dynamic_TLS_codegen=dict(path=str((OUT / 'arm-dso-disassembly.txt').relative_to(ROOT)),
            sha256=sha(OUT / 'arm-dso-disassembly.txt'),
            baseline='Current256 main-hit returns before fallback TLSDESC; separately initialized1134148B index.',
            candidate='Candidate64 main-hit returns before identical fallback TLSDESC;4536580B index.'),
        index_contract='Baseline256B/1134148B versus candidate64B/4536580B over exact unchanged399917keys; identical lower_bound/adapters/fallback and init. Only2geometrydefines differ; source archived byteexact.',
        trace_contract='Exact complete ordered provider/adapter/cache trace equality; no event filtering. FullCPU/errors/FP; cold global binary and checked initialized immutable geometries.',
        scope='Existing namespace fixture and real current bootstrap bodies. Main AOT and unrelated provider bodies are traceable doubles; exact table pointer mode uses unique/shared nonexecuted tokens. Host Win64 TLS differs from AndroiddynamicTLS; microbenchmarks are model costs, not actualgameCPU/FPS/startuptime.',
        production_modified=False, device_executed=False, original_bytes_executed=False)
    receipt = OUT / 'result.json';receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(success=True, host=host_proof, pointers=pointers, aliases=aliases,
        benchmark=[{key:value for key,value in row.items() if key not in ('baseline_ns','candidate_ns')} for row in bench['scenarios']],
        arm=report['arm'], arm_benchmark=report['arm_benchmark'], arm_dynamic_TLS_proof=report['arm_dynamic_TLS_proof'], arm_dynamic_TLS_benchmark=report['arm_dynamic_TLS_benchmark'],
        dataset=report['dataset'], receipt_sha256=sha(receipt)), indent=2))


if __name__ == '__main__':
    main()
