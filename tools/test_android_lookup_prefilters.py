"""Qualify exact-VA adapter prefilters using the existing dispatch trace oracle.

Production dispatch and the two real adapter lookup bodies are extracted. Only
the three target function bodies and unrelated providers are doubles; this is
not an API, frame-rate or startup-time measurement. No ADB commands are used.
"""
import difflib
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain
from test_android_dispatch_namespace import dispatch_addresses

OUT = ROOT / 'build/android-lookup-prefilters-test'
RESIZE_OLD = ('    void (*resize_function)(CPU *)=fp_android_string_resize_lookup(target);\n'
              '    if(resize_function)return resize_function;\n')
RESIZE_NEW = ('    if(target==UINT64_C(0x140e222e0)) {\n'
              '        void (*resize_function)(CPU *)=fp_android_string_resize_lookup(target);\n'
              '        if(resize_function)return resize_function;\n    }\n')
TEXTURE_OLD = ('    void (*texture_function)(CPU *)=fp_android_texture_formats_lookup(target);\n'
               '    if(texture_function)return texture_function;\n')
TEXTURE_NEW = ('    if(target==UINT64_C(0x140d95650)||target==UINT64_C(0x140f499a0)) {\n'
               '        void (*texture_function)(CPU *)=fp_android_texture_formats_lookup(target);\n'
               '        if(texture_function)return texture_function;\n    }\n')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def pair(text):
    if text.count(RESIZE_NEW) != 1 or text.count(TEXTURE_NEW) != 1:
        raise ValueError('Production prefilters differ from the exact two approved hunks')
    baseline = text.replace(RESIZE_NEW, RESIZE_OLD, 1).replace(TEXTURE_NEW, TEXTURE_OLD, 1)
    if baseline.count(RESIZE_OLD) != 1 or baseline.count(TEXTURE_OLD) != 1:
        raise ValueError('Ambiguous prefilter reversal')
    regenerated = baseline.replace(RESIZE_OLD, RESIZE_NEW, 1).replace(TEXTURE_OLD, TEXTURE_NEW, 1)
    if regenerated != text:
        raise ValueError('Whole production source did not regenerate exactly')
    return baseline, text


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Fixture/source extraction anchor changed: ' + before[:100])
    return text.replace(before, after, 1)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'android/host/bootstrap.c'
    resize = ROOT / 'android/host/string_resize_aot.c'
    texture = ROOT / 'android/host/texture_formats.c'
    fixture_source = ROOT / 'tests/android_dispatch_namespace_fixture.c'
    table_paths = [ROOT / 'generated/frostpunk-x64/recomp_dispatch.c',
                   ROOT / 'generated/supplemental/recomp_supplemental.c',
                   ROOT / 'generated/payload/recomp/recomp_dispatch.c',
                   ROOT / 'generated/dependencies/msvcp140/recomp/recomp_dispatch.c',
                   ROOT / 'build/openal-bridge/openal_original_entries.h']
    extra_paths = [ROOT / 'native/windows' / name for name in
                   ['extra_aot_bodies.inc', 'switch_aot_bodies.inc', 'switch_extension_bodies.inc']]
    pins = [source, resize, texture, fixture_source, Path(__file__).resolve(),
            ROOT / 'tools/test_android_dispatch_namespace.py', ROOT / 'tools/leaf_probe.py',
            ROOT / 'port.json', ROOT / 'native/runtime64/cpu64.h',
            ROOT / 'generated/frostpunk-x64/recomp_meta.json',
            ROOT / 'generated/frostpunk-x64/recomp_imports.c', *table_paths, *extra_paths]
    before = {str(path.relative_to(ROOT)): sha(path) for path in pins}
    cfg = json.loads((ROOT / 'port.json').read_text())
    compiler = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    compiler_sha = sha(compiler)
    original, current = pair(source.read_text())
    corruptions = [current.replace('0x140e222e0)) {', '0x140e222e1)) {', 1),
                   current.replace('0x140d95650)||', '0x140d95651)||', 1),
                   current.replace('0x140f499a0)) {', '0x140f499a1)) {', 1),
                   current.replace(RESIZE_NEW, RESIZE_OLD, 1),
                   current.replace(TEXTURE_NEW, TEXTURE_OLD, 1), current + RESIZE_NEW]
    for corrupt in corruptions:
        try:
            pair(corrupt)
        except ValueError:
            continue
        raise ValueError('Malformed prefilter was accepted')
    (OUT / 'bootstrap-prefilters.patch').write_text(''.join(difflib.unified_diff(
        original.splitlines(True), current.splitlines(True), fromfile='bootstrap-before.c', tofile='bootstrap.c')))
    for name, text in [('baseline', original), ('candidate', current)]:
        first = text.index('/* Only immutable main/supplemental AOT entries are cached.')
        last = text.index('void dispatch_jmp(CPU *c, uint64_t target)', first)
        body = text[first:last]
        for old, new in [('main_static_lookup', name + '_main_lookup'), ('dispatch', name + '_dispatch'),
                         ('call_depth', name + '_depth'), ('dispatch_calls', name + '_calls'),
                         ('dispatch_budget', name + '_budget'), ('trace_calls', name + '_trace')]:
            body = re.sub(r'\b' + old + r'\b', new, body)
        body = body.replace('void ' + name + '_dispatch(', 'FP_NOINLINE void ' + name + '_dispatch(')
        (OUT / ('dispatch_' + name + '.inc')).write_text(body)

    meta = json.loads((ROOT / 'generated/frostpunk-x64/recomp_meta.json').read_text())
    main_path, supplemental_path, payload_path, dependency_path, openal_path = table_paths
    imports_path = ROOT / 'generated/frostpunk-x64/recomp_imports.c'
    if meta['original_exe_sha256'] != cfg['original_exe_sha256'] or meta['size_of_image'] != 0x4539000 or any(
            sha(path) != meta['generated_files'][path.name] for path in (main_path, imports_path)):
        raise ValueError('Sealed main input identity changed')
    supplemental = [int(v, 16) for v in re.findall(r'^\s*\{0x([0-9A-Fa-f]+)ull,frostpunk_supplemental_', supplemental_path.read_text(), re.M)]
    extra = sorted({int(v, 16) for path in extra_paths for v in re.findall(r'\{UINT64_C\(0x([0-9a-fA-F]+)\),\d+,', path.read_text())})
    openal_text = openal_path.read_text()
    openal = [int(v, 16) for v in re.findall(r'^ UINT64_C\(0x([0-9a-fA-F]+)\)', openal_text, re.M)]
    typed = sorted(int(v, 16) for v in re.findall(r'X\([^\n]+UINT64_C\(0x([0-9a-fA-F]+)\)', openal_text))
    manual = sorted([0x140e222e0, 0x140d95650, 0x140f499a0, 0x1410596a9, 0x1410597e9])
    tables = [dispatch_addresses(main_path), supplemental, extra, dispatch_addresses(payload_path),
              dispatch_addresses(dependency_path), openal, typed, manual]
    if [len(table) for table in tables] != [399917, 1985, 35, 644, 2194, 157, 30, 5]:
        raise ValueError('Immutable namespace table counts changed')
    data_path = OUT / 'tables.bin'
    with data_path.open('wb') as stream:
        stream.write(b'FPNDISP1' + struct.pack('<8I', *(len(table) for table in tables)))
        for table in tables:
            if table != sorted(set(table)):
                raise ValueError('Noncanonical address table')
            for target in table:
                stream.write(struct.pack('<Q', target))
    imports = re.findall(r'\{\s*(0x[0-9a-fA-F]+)ull,\s*("[^"\n]+"),\s*("[^"\n]+")\s*\}', imports_path.read_text())
    if len(imports) != 557:
        raise ValueError('Main import count changed')
    (OUT / 'dispatch_namespaces.inc').write_text(
        '#define MAIN_BASE UINT64_C(0x140000000)\n#define MAIN_END UINT64_C(0x144539000)\n'
        '#define OPENAL_MIN UINT64_C(0x141babd20)\n#define OPENAL_MAX UINT64_C(0x141bc2d40)\n'
        'static const hle_import_t recomp_imports[]={\n' +
        ''.join('{UINT64_C(%s),%s,%s},\n' % row for row in imports) + '};\n')

    # Pin the real helper selection contracts, then compile those exact bodies.
    resize_lookup = ('void (*fp_android_string_resize_lookup(uint64_t va))(CPU *) {\n'
                     '    return verified&&va==UINT64_C(0x140e222e0)?string_resize:NULL;\n}\n')
    texture_lookup = ('void (*fp_android_texture_formats_lookup(uint64_t original_va))(CPU *) {\n'
                      '    if(!verified)return NULL;\n'
                      '    if(original_va==UINT64_C(0x140d95650))return map_format;\n'
                      '    if(original_va==UINT64_C(0x140f499a0))return format_layout;\n'
                      '    return NULL;\n}\n')
    fixture = fixture_source.read_text()
    for lookup, path, old_stub, event in [
        (resize_lookup, resize, 'static FP_NOINLINE fn_t fp_android_string_resize_lookup(uint64_t t){record(11);return manual_ready&&t==UINT64_C(0x140e222e0)?resize:NULL;}', 11),
        (texture_lookup, texture, 'static FP_NOINLINE fn_t fp_android_texture_formats_lookup(uint64_t t){record(12);return manual_ready&&(t==UINT64_C(0x140d95650)||t==UINT64_C(0x140f499a0))?texture:NULL;}', 12)]:
        if path.read_text().count(lookup) != 1 or not path.read_text().endswith(lookup):
            raise ValueError('Adapter lookup contract expanded or changed: ' + str(path))
        body = lookup.replace('void (*fp_', 'static FP_NOINLINE void (*fp_', 1)
        body = body.replace('{\n', '{\n    record(%d);\n' % event, 1)
        for old, new in [('verified', 'manual_ready'), ('string_resize', 'resize'), ('map_format', 'texture'), ('format_layout', 'texture')]:
            body = re.sub(r'\b' + old + r'\b', new, body)
        fixture = replace_once(fixture, old_stub, body.rstrip())
    trace_start = fixture.index('    if(eligible(t)&&(member(0,t)')
    trace_end = fixture.index('    same=same&&n==b.count', trace_start)
    fixture = fixture[:trace_start] + '''    for(unsigned i=0;i<a.count;++i){
        unsigned event=a.trace[i];
        if((event==11&&t!=UINT64_C(0x140e222e0))||
           (event==12&&t!=UINT64_C(0x140d95650)&&t!=UINT64_C(0x140f499a0)))continue;
        expected[n++]=event;
    }
    if(n<a.count)++fast_cases;else ++slow_cases;
''' + fixture[trace_end:]
    # Preserve the original all-table/negative/cold-cache cases, adding exact neighbors.
    fixture = replace_once(fixture, '    const uint64_t spaces[][2]=', '''    for(unsigned j=0;j<3;++j){
        uint64_t target=(uint64_t[]){UINT64_C(0x140e222e0),UINT64_C(0x140d95650),UINT64_C(0x140f499a0)}[j];
        for(unsigned ready=0;ready<2;++ready){manual_ready=(int)ready;
            for(int delta=-2;delta<=2;++delta)compare(target+delta,j+ready,0,0,0);}
    }manual_ready=1;
    const uint64_t spaces[][2]=''')
    fixture_path = OUT / 'lookup_prefilters_fixture.c'
    fixture_path.write_text(fixture)
    bins, env = windows_toolchain()
    commands = []

    def run(command):
        command = list(map(str, command)); commands.append(command)
        process = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=180)
        if process.returncode:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + process.stdout[-2000:] + process.stderr[-5000:])
        return process

    includes = ['-I' + str(ROOT / 'native/runtime64'), '-I' + str(OUT)]
    flags = ['-std=c11', '-O2', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off']
    host = OUT / 'lookup-prefilters-host.exe'; obj = OUT / 'lookup-prefilters-host.obj'
    run([compiler, '--target=x86_64-pc-windows-msvc', *flags, *includes, '-c', fixture_path, '-o', obj])
    run([bins / 'link.exe', '/nologo', '/out:' + str(host), obj, 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
    process = run([host, data_path]); host_result = json.loads(process.stdout)
    (OUT / 'host.log').write_text(process.stdout + process.stderr)
    if not host_result['success'] or host_result['all_main_entries'] != 399917 or host_result['failures']:
        raise ValueError('Paired control flow proof failed')
    arm = OUT / 'lookup-prefilters-arm64'
    run([compiler, '--target=aarch64-linux-android30', *flags, *includes, fixture_path,
         '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', arm])
    if before != {name: sha(ROOT / name) for name in before} or sha(compiler) != compiler_sha:
        raise ValueError('Source/compiler drift during proof')
    generated = {str(path.relative_to(ROOT)): sha(path) for path in OUT.iterdir()
                 if path.suffix in ('.inc', '.c', '.patch')}
    report = {'success': True, 'kind': 'android-exact-va-lookup-prefilters', 'source_hashes': before,
              'compiler_sha256': compiler_sha, 'drift_rejections': len(corruptions),
              'whole_bootstrap_regenerated_exactly': True, 'host': host_result,
              'host_binary_sha256': sha(host), 'arm': {'compiled': True, 'executed': False,
                  'path': str(arm.relative_to(ROOT)), 'sha256': sha(arm), 'args': ['tables.bin']},
              'dataset': {'path': str(data_path.relative_to(ROOT)), 'sha256': sha(data_path), 'bytes': data_path.stat().st_size},
              'generated_hashes': generated, 'commands': commands,
              'scope': 'Real bootstrap/adapter lookup bodies and immutable tables. Target bodies and other providers are traceable doubles. Initializers/byte gates are unchanged, not re-executed by this routing test.',
              'original_bytes_executed': False, 'device_executed': False, 'startup_or_FPS_claim': False}
    receipt = OUT / 'result.json'; receipt.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': host_result, 'arm': report['arm'],
                      'dataset': report['dataset'], 'receipt': str(receipt.relative_to(ROOT)),
                      'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
