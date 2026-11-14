"""Prove a bootstrap-only namespace fast path without changing production.

The real current dispatch/main lookup bodies are extracted twice. Provider and
AOT bodies are traceable doubles, with real immutable address tables. A separate
trace-free ARM model benchmark is deliberately not a startup/FPS measurement.
"""
import difflib
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-dispatch-namespace-test'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def dispatch_addresses(path):
    pattern = re.compile(r'^\s*\{\s*0x([0-9a-fA-F]+)ull,\s*L_')
    result = []
    for line in Path(path).open():
        match = pattern.match(line)
        if match:
            result.append(int(match[1], 16))
    if not result or result != sorted(set(result)):
        raise ValueError('Unsorted/duplicate/empty dispatch table ' + str(path))
    return result


def dispatch_pair(current):
    """Reverse only the approved hunks, then compare the entire regenerated body."""
    old_comment = ('/* Only immutable main/supplemental AOT entries are cached. Provider and\n'
                   ' * dependency initialization checks still run before this lookup. */')
    new_comment = ('/* Only immutable main/supplemental AOT entries are cached. Exact main\n'
                   ' * namespace hits may skip disjoint provider declines; the entire\n'
                   ' * OpenAL envelope retains its existing provider precedence. */')
    declaration = '    void (*function)(CPU *) = NULL;\n'
    anchor = '#if defined(FROSTPUNK_HAVE_PAYLOAD)\n    if (frostpunk_payload_import(c,target)) return;'
    dynamic = '    if (win64_dispatch_dynamic(c,target)) return;\n'
    invoke = '    if (++call_depth > 4096)'
    addition = (declaration +
                '    if (target >= UINT64_C(0x140000000) && target < UINT64_C(0x144539000) &&\n'
                '        (target < UINT64_C(0x141babd20) || target > UINT64_C(0x141bc2d40))) {\n'
                '        function=main_static_lookup(target);\n'
                '        if(function)goto static_invoke;\n'
                '    }\n')
    if current.startswith(new_comment):
        if current.count(addition) != 1 or current.count('static_invoke:\n' + invoke) != 1 or current.count(dynamic) != 1:
            raise ValueError('Promoted dispatch differs from the exact approved hunks')
        baseline = current.replace(new_comment, old_comment, 1).replace(addition, '', 1)
        baseline = baseline.replace('static_invoke:\n' + invoke, invoke, 1)
        baseline = baseline.replace(dynamic, dynamic + declaration, 1)
        variant = 'exact-promoted'
    elif current.startswith(old_comment):
        baseline, variant = current, 'pre-promotion'
        if 'static_invoke' in baseline:
            raise ValueError('Unexpected partial dispatch promotion')
    else:
        raise ValueError('Unknown dispatch cache comment/variant')
    if baseline.count(declaration) != 1 or baseline.count(anchor) != 1 or baseline.count(invoke) != 1:
        raise ValueError('Dispatch extraction/patch anchor changed')
    candidate = baseline.replace(declaration, '').replace(anchor, addition + anchor)
    candidate = candidate.replace(invoke, 'static_invoke:\n' + invoke).replace(old_comment, new_comment, 1)
    if variant == 'exact-promoted' and candidate != current:
        raise ValueError('Whole regenerated candidate differs from current production')
    return baseline, candidate, variant


def normalization_self_test(baseline, candidate):
    if dispatch_pair(baseline) != (baseline, candidate, 'pre-promotion'):
        raise ValueError('Pre-promotion round trip failed')
    if dispatch_pair(candidate) != (baseline, candidate, 'exact-promoted'):
        raise ValueError('Promoted round trip failed')
    unrelated = candidate.replace('    if (trace_calls)', '    /* Future unrelated body change must be retained. */\n    if (trace_calls)', 1)
    refreshed_baseline, refreshed_candidate, variant = dispatch_pair(unrelated)
    if refreshed_candidate != unrelated or 'Future unrelated body change' not in refreshed_baseline or variant != 'exact-promoted':
        raise ValueError('Unrelated current-body changes were ignored')
    corruptions = [candidate.replace('0x141babd20', '0x141babd21', 1),
                   candidate.replace('if(function)goto static_invoke;', 'if(function)return;', 1),
                   candidate.replace('static_invoke:\n', 'static_other:\n', 1),
                   candidate.replace('the entire\n', 'the partial\n', 1),
                   candidate.replace('    void (*function)(CPU *) = NULL;\n', '', 1)]
    for corrupt in corruptions:
        try:
            dispatch_pair(corrupt)
        except ValueError:
            continue
        raise ValueError('Malformed/partial promoted variant was accepted')
    return 3 + len(corruptions)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    compiler = (Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] /
                'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe')
    source = ROOT / 'android/host/bootstrap.c'
    text = source.read_text()
    first = text.index('/* Only immutable main/supplemental AOT entries are cached.')
    last = text.index('void dispatch_jmp(CPU *c, uint64_t target)', first)
    baseline, candidate, production_variant = dispatch_pair(text[first:last])
    normalization_cases = normalization_self_test(baseline, candidate)
    baseline_text = text[:first] + baseline + text[last:]
    patched = text[:first] + candidate + text[last:]
    (OUT / 'bootstrap-dispatch-candidate.patch').write_text(''.join(difflib.unified_diff(
        baseline_text.splitlines(True), patched.splitlines(True), fromfile='android/host/bootstrap.c', tofile='android/host/bootstrap.c')))
    for name, body in [('baseline', baseline), ('candidate', candidate)]:
        for old, new in [('main_static_lookup', name + '_main_lookup'), ('dispatch', name + '_dispatch'),
                         ('call_depth', name + '_depth'), ('dispatch_calls', name + '_calls'),
                         ('dispatch_budget', name + '_budget'), ('trace_calls', name + '_trace')]:
            body = re.sub(r'\b' + old + r'\b', new, body)
        body = body.replace('void ' + name + '_dispatch(', 'FP_NOINLINE void ' + name + '_dispatch(')
        (OUT / ('dispatch_' + name + '.inc')).write_text(body)

    main_path = ROOT / 'generated/frostpunk-x64/recomp_dispatch.c'
    meta = json.loads((ROOT / 'generated/frostpunk-x64/recomp_meta.json').read_text())
    if meta['original_exe_sha256'] != cfg['original_exe_sha256'] or sha(main_path) != meta['generated_files']['recomp_dispatch.c']:
        raise ValueError('Sealed main table identity changed')
    main_table = dispatch_addresses(main_path)
    if len(main_table) != 399917 or meta['size_of_image'] != 0x4539000:
        raise ValueError('Main namespace no longer matches candidate')
    supplemental_path = ROOT / 'generated/supplemental/recomp_supplemental.c'
    supplemental = [int(match, 16) for match in re.findall(r'^\s*\{0x([0-9A-Fa-f]+)ull,frostpunk_supplemental_', supplemental_path.read_text(), re.M)]
    extra_paths = [ROOT / 'native/windows' / name for name in ['extra_aot_bodies.inc', 'switch_aot_bodies.inc', 'switch_extension_bodies.inc']]
    extra = sorted({int(match, 16) for path in extra_paths for match in re.findall(r'\{UINT64_C\(0x([0-9a-fA-F]+)\),\d+,', path.read_text())})
    payload_path = ROOT / 'generated/payload/recomp/recomp_dispatch.c'
    dependency_path = ROOT / 'generated/dependencies/msvcp140/recomp/recomp_dispatch.c'
    openal_path = ROOT / 'build/openal-bridge/openal_original_entries.h'
    openal_text = openal_path.read_text()
    openal = [int(match, 16) for match in re.findall(r'^ UINT64_C\(0x([0-9a-fA-F]+)\)', openal_text, re.M)]
    typed = sorted(int(match, 16) for match in re.findall(r'X\([^\n]+UINT64_C\(0x([0-9a-fA-F]+)\)', openal_text))
    manual = sorted([0x140e222e0, 0x140d95650, 0x140f499a0, 0x1410596a9, 0x1410597e9])
    tables = [main_table, supplemental, extra, dispatch_addresses(payload_path), dispatch_addresses(dependency_path), openal, typed, manual]
    if len(supplemental) != 1985 or len(extra) != 35 or len(openal) != 157 or len(typed) != 30:
        raise ValueError('Namespace table counts changed: ' + repr([len(t) for t in tables]))
    if openal[0] != 0x141babd20 or openal[-1] != 0x141bc2d40 or not set(openal) <= set(main_table):
        raise ValueError('OpenAL precedence exception changed')
    if not set(typed) <= set(openal) or not set(manual) <= set(main_table) or any(not 0x140000000 <= t < 0x144539000 for t in main_table + supplemental + extra):
        raise ValueError('Unexpected overlap/range in static tables')
    fixture_data = OUT / 'tables.bin'
    with fixture_data.open('wb') as stream:
        stream.write(b'FPNDISP1' + struct.pack('<8I', *(len(table) for table in tables)))
        for table in tables:
            if table != sorted(set(table)):
                raise ValueError('Noncanonical fixture address table')
            for target in table:
                stream.write(struct.pack('<Q', target))
    imports_path = ROOT / 'generated/frostpunk-x64/recomp_imports.c'
    if sha(imports_path) != meta['generated_files']['recomp_imports.c']:
        raise ValueError('Sealed main imports changed')
    import_rows = re.findall(r'\{\s*(0x[0-9a-fA-F]+)ull,\s*("[^"\n]+"),\s*("[^"\n]+")\s*\}', imports_path.read_text())
    if len(import_rows) != 557:
        raise ValueError('Main IAT count changed')
    (OUT / 'dispatch_namespaces.inc').write_text(
        '#define MAIN_BASE UINT64_C(0x140000000)\n#define MAIN_END UINT64_C(0x144539000)\n'
        '#define OPENAL_MIN UINT64_C(0x141babd20)\n#define OPENAL_MAX UINT64_C(0x141bc2d40)\n'
        'static const hle_import_t recomp_imports[]={\n' +
        ''.join('{UINT64_C(%s),%s,%s},\n' % row for row in import_rows) + '};\n')
    proof = json.loads((ROOT / 'research/android-dispatch-namespace-analysis.json').read_text())
    pins = [source, ROOT / 'tests/android_dispatch_namespace_fixture.c', Path(__file__).resolve(),
            ROOT / 'native/runtime64/cpu64.h', ROOT / 'research/android-dispatch-namespace-analysis.json',
            main_path, imports_path, ROOT / 'generated/frostpunk-x64/recomp_meta.json', supplemental_path,
            payload_path, dependency_path, openal_path, *extra_paths]
    pins += [ROOT / name for name in proof['source_sha256'] if (ROOT / name).exists() and ROOT / name not in pins]
    pins += [ROOT / 'android/host' / name for name in ['string_resize_aot.c', 'texture_formats.c', 'extra_aot.c', 'shader_lookup_aot.c']]
    before = {str(path.relative_to(ROOT)): sha(path) for path in dict.fromkeys(pins)}
    bins, env = windows_toolchain()
    commands = []

    def run(command):
        command = list(map(str, command));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-1000:] + result.stderr[-7000:])
        return result

    includes = ['-I' + str(ROOT / 'native/runtime64'), '-I' + str(OUT)]
    fixture = ROOT / 'tests/android_dispatch_namespace_fixture.c'
    obj = OUT / 'namespace-host.obj';host = OUT / 'namespace-host.exe'
    run([compiler, '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2', *includes, '-c', fixture, '-o', obj])
    run([bins / 'link.exe', '/nologo', '/out:' + str(host), obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_process = run([host, fixture_data]);host_result = json.loads(host_process.stdout)
    (OUT / 'host.log').write_text(host_process.stdout + host_process.stderr)
    if not host_result['success'] or host_result['all_main_entries'] != 399917:
        raise ValueError('Host equivalence proof failed')
    benchmark_obj = OUT / 'namespace-bench-host.obj';benchmark_host = OUT / 'namespace-bench-host.exe'
    run([compiler, '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2', '-DFP_DISPATCH_BENCHMARK=1',
         *includes, '-c', fixture, '-o', benchmark_obj])
    run([bins / 'link.exe', '/nologo', '/out:' + str(benchmark_host), benchmark_obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_benchmark = json.loads(run([benchmark_host, fixture_data, '100000']).stdout)
    binaries = []
    for benchmark in [False, True]:
        binary = OUT / ('dispatch-bench-arm64' if benchmark else 'dispatch-proof-arm64')
        run([compiler, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-fno-strict-aliasing',
             '-ffunction-sections', '-fdata-sections', *(['-DFP_DISPATCH_BENCHMARK=1'] if benchmark else []),
             *includes, fixture, '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
        binaries.append({'path':str(binary.relative_to(ROOT)), 'sha256':sha(binary), 'bytes':binary.stat().st_size,
                         'args':['tables.bin', '1000000'] if benchmark else ['tables.bin']})
    if before != {name:sha(ROOT / name) for name in before}:
        raise ValueError('Pinned inputs changed during fixture build')
    generated = {str(path.relative_to(ROOT)):sha(path) for path in OUT.glob('*.inc')}
    generated[str((OUT / 'bootstrap-dispatch-candidate.patch').relative_to(ROOT))] = sha(OUT / 'bootstrap-dispatch-candidate.patch')
    report = {'success':True, 'kind':'dispatch_namespace_candidate_control_flow_proof', 'source_hashes':before,
              'production_variant':production_variant, 'normalization_cases':normalization_cases,
              'current_promoted_body_regenerated_exactly':production_variant == 'exact-promoted',
              'baseline_fragment_sha256':hashlib.sha256(baseline.encode()).hexdigest(),
              'candidate_fragment_sha256':hashlib.sha256(candidate.encode()).hexdigest(),
              'host':host_result, 'host_model_benchmark':host_benchmark,
              'table_counts':[len(table) for table in tables], 'generated_hashes':generated,
              'dataset':{'path':str(fixture_data.relative_to(ROOT)), 'sha256':sha(fixture_data), 'bytes':fixture_data.stat().st_size},
              'binaries':binaries, 'commands':commands, 'production_modified':False, 'device_executed':False,
              'scope':'Exact current dispatch/main lookup code and real table addresses; deterministic provider and function body doubles, no API/native rendering parity claim.',
              'benchmark_scope':'Trace-free isolated dispatch namespace model with real immutable tables. Provider hit bodies and native provider prologues are doubles. Does not measure actual runtime startup or camera FPS.',
              'startup_or_FPS_claim':False}
    path = OUT / 'result.json';path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success':True, 'host':host_result, 'binaries':binaries, 'dataset':report['dataset'], 'receipt':str(path)},indent=2))


if __name__ == '__main__':
    main()
