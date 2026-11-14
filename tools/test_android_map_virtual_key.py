"""Native US MapVirtualKeyExA oracle, source-bound host proof and ARM USER32 fixture.

This tool never uses ADB or executes original game code.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-mapvirtualkey-test'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def function(source, declaration):
    start = source.index(declaration)
    if source.count(declaration) != 1:
        raise ValueError('Ambiguous production function: ' + declaration)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    compiler = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_window.c', 'android/host/win64_window.h', 'android/host/win64.c',
             'android/host/win64_kernel.c', 'tests/window_fixture.c', 'tests/window_properties_fixture.c',
             'tests/window_oracle.c', 'tests/window_title_return_width.c',
             'tests/map_virtual_key_oracle.c', 'tests/window_mapvirtualkey_fixture.c',
             'tools/test_android_map_virtual_key.py', 'native/runtime64/cpu64.h']
    before = {name: sha(ROOT / name) for name in files}
    commands = []

    def run(args):
        command = list(map(str, args))
        commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout + result.stderr)
        return result.stdout

    def native(name, extra=()):
        exe = OUT / (name + '.exe')
        run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', '/W3',
             '/I' + str(ROOT / 'native/runtime64'), '/I' + str(OUT),
             ROOT / ('tests/' + name + '.c'), '/Fo' + str(OUT) + '/', '/Fe' + str(exe),
             '/link', 'user32.lib', *extra])
        return run([exe])

    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE changed')
    import pefile
    pe = pefile.PE(str(original), fast_load=True)
    original_call = bytes.fromhex('4533c0418bca418d5004ff15be78bc00')
    base = pe.OPTIONAL_HEADER.ImageBase
    if pe.get_data(0x14110008A - base, len(original_call)) != original_call:
        raise ValueError('Original MapVirtualKeyExA caller changed')
    spans = []
    for va, size, role in [(0x141100000, 0x205, 'engine map and subscriber delivery'),
                           (0x141100208, 14 * 4, 'bounded navigation exception table'),
                           (0x1410F8020, 0x41, 'engine forwarding wrapper')]:
        data = pe.get_data(va - base, size)
        spans.append({'va': hex(va), 'size': size, 'role': role, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': data.hex()})
    oracle = json.loads(native('map_virtual_key_oracle'))
    if not (oracle['success'] and oracle['current_layout_unchanged'] and oracle['checks'] == 73988
            and oracle['unmapped_high_codes'] == 73476 and len(oracle['table']) == 256):
        raise ValueError('Unexpected native oracle result')
    source = (ROOT / files[0]).read_text()
    helper = function(source, 'static uint64_t map_virtual_key_ex(CPU *cpu)')
    table = re.search(r'us_vk_to_vsc_ex\[256\]=\{(.*?)\};', helper, re.S)
    if not table or [int(v, 16) for v in re.findall(r'0x[0-9a-f]+', table.group(1))] != oracle['table']:
        raise ValueError('Production table differs from native US oracle')
    branches = ['    if(!strcmp(name,"MapVirtualKeyW")){cpu->rax=failure(50,0);return 1;}',
                '    if(!strcmp(name,"MapVirtualKeyExA")){cpu->rax=map_virtual_key_ex(cpu);return 1;}']
    if any(source.count(line) != 1 for line in branches):
        raise ValueError('Production API branch changed')
    wrapper = function(source, 'int win64_window_call(CPU *cpu,const char *name)')
    (OUT / 'map_extract.inc').write_text(helper + '\nstatic void window_enter(void){}\nstatic void window_leave(void){}\n'
        + 'static int window_call(CPU *cpu,const char *name){\n' + '\n'.join(branches) + '\nreturn 0;\n}\n' + wrapper + '\n')
    (OUT / 'map_native.h').write_text('static const uint16_t map_native[256]={\n' +
        ','.join(str(v) for v in oracle['table']) + '};\n')
    route = '    if (user && win64_window_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Production USER32 import route changed')
    (OUT / 'map_route.inc').write_text(route + '\n')
    host = json.loads(native('window_mapvirtualkey_fixture'))
    if not (host['success'] and host['calls'] == 90395 and host['parallel_calls'] == 16384):
        raise ValueError('Unexpected full CPU fixture result')
    scans = native('window_oracle')
    rows = re.findall(r'^KEYSCAN (AKEYCODE_\w+) (\d+) (\d+)$', scans, re.M)
    if not rows:
        raise ValueError('Native input oracle data is missing')
    (OUT / 'window_scans.h').write_text('static const struct {unsigned android_key,vk,scan;} window_key_scans[]={\n' +
        ''.join('{%s,%s,%s},\n' % row for row in rows) + '};\n')
    titles = json.loads(native('window_title_return_width'))
    (OUT / 'window_title_returns.h').write_text('static const struct {uint64_t lresult,result;uint32_t error;int native_errno;} window_title_returns[]={\n' +
        ''.join('{UINT64_C(0x%s),UINT64_C(%d),%d,%d},\n' % (r['callback_LRESULT_hex'],r['BOOL'],r['LastError'],r['errno']) for r in titles) + '};\n')
    binary = OUT / 'mapvirtualkey-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
         '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'generated/frostpunk-x64'),
         '-I' + str(ROOT / 'android/host'), '-I' + str(ROOT / 'android/app'), '-I' + str(ROOT / 'tests'), '-I' + str(OUT),
         ROOT / 'tests/window_mapvirtualkey_fixture.c', ROOT / 'android/host/win64_window.c', ROOT / 'android/host/win64_kernel.c',
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
    if before != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Sources changed during fixtures')
    outputs = {p.name: sha(p) for p in [OUT / 'map_extract.inc', OUT / 'map_native.h', OUT / 'map_route.inc', binary]}
    report = {'success': True, 'sources': before, 'native_oracle': oracle, 'host_fixture': host, 'commands': commands,
              'original_exe_sha256': cfg['original_exe_sha256'], 'original_spans': spans,
              'original_call': {'va': '0x14110008a', 'bytes': original_call.hex(), 'mapType': 4, 'HKL': 0, 'code': 'uint32 incoming R8D', 'result': 'unmodified uint32 subscriber EDX'},
              'original_executed': False, 'generated_outputs': outputs, 'binary': str(binary.relative_to(ROOT)), 'binary_sha256': sha(binary),
              'device_executed': False, 'scope': 'virtual US keyboard; type4/NULL HKL only; high UINT codes unmapped; other modes/layouts honest error50; MapVirtualKeyW remains error50; no gameplay claim'}
    path = OUT / 'result.json'
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'receipt': str(path), 'binary': str(binary), 'binary_sha256': sha(binary), 'production_sha256': before[files[0]], 'host_calls': host['calls']}, indent=2))


if __name__ == '__main__':
    main()
