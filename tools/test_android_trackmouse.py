"""Native hidden-window oracle and actual USER32 ARM fixture; never uses ADB."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-trackmouse-test'


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    compiler = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files = ['android/host/win64_window.c', 'android/host/win64_window.h', 'android/host/win64.c',
             'android/host/win64_kernel.c', 'tests/window_fixture.c', 'tests/window_oracle.c',
             'tests/window_title_return_width.c', 'tests/window_trackmouse_oracle.c',
             'tests/window_trackmouse_fixture.c', 'tools/test_android_trackmouse.py']
    before = {name: sha(ROOT / name) for name in files}
    commands = []
    def run(args):
        command = list(map(str, args));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=45)
        if result.returncode: raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout + result.stderr)
        return result.stdout
    def native(name):
        exe = OUT / (name + '.exe')
        run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', ROOT / ('tests/' + name + '.c'),
             '/Fo' + str(OUT) + '/', '/Fe' + str(exe), '/link', 'user32.lib'])
        return run([exe])
    import pefile
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']: raise ValueError('Original PE changed')
    pe = pefile.PE(str(original), fast_load=True)
    expected = bytes.fromhex('4883ec48488b82c00000004885c0742b48837808007424c744242018000000488d4c2420c744242402000000488b40084889442428ff15c5871a004883c448c3')
    if pe.get_data(0x141b1eee0 - pe.OPTIONAL_HEADER.ImageBase, len(expected)) != expected:
        raise ValueError('Original SDL TrackMouseEvent caller changed')
    oracle = [json.loads(line) for line in native('window_trackmouse_oracle').splitlines()]
    if [(r['result'], r['error']) for r in oracle] != [(1,0x5a17)] * 5 + [(0,87),(0,1400)]:
        raise ValueError('Unexpected native TrackMouseEvent results: ' + repr(oracle))
    for i in (0,2,4):
        if not (oracle[i]['flags']==0 and oracle[i]['hwnd_zero'] and oracle[i]['hover']==0 and oracle[i]['padding']==0):
            raise ValueError('Native empty query layout differs')
    if oracle[1]['leaves'] != 1: raise ValueError('Native outside registration did not post one leave')
    route = '    if (user && win64_window_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT / 'android/host/win64.c').read_text().count(route) != 1:
        raise ValueError('Production USER32 import route changed')
    (OUT / 'track_route.inc').write_text(route + '\n')
    scans = native('window_oracle')
    rows = re.findall(r'^KEYSCAN (AKEYCODE_\w+) (\d+) (\d+)$', scans, re.M)
    if not rows: raise ValueError('Native keyboard mapping fixture is missing')
    (OUT / 'window_scans.h').write_text('static const struct {unsigned android_key,vk,scan;} window_key_scans[]={\n' +
        ''.join('{%s,%s,%s},\n' % row for row in rows) + '};\n')
    titles = json.loads(native('window_title_return_width'))
    (OUT / 'window_title_returns.h').write_text('static const struct {uint64_t lresult,result;uint32_t error;int native_errno;} window_title_returns[]={\n' +
        ''.join('{UINT64_C(0x%s),UINT64_C(%d),%d,%d},\n' % (r['callback_LRESULT_hex'],r['BOOL'],r['LastError'],r['errno']) for r in titles) + '};\n')
    binary = OUT / 'trackmouse-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
         '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'generated/frostpunk-x64'),
         '-I' + str(ROOT / 'android/host'), '-I' + str(ROOT / 'android/app'), '-I' + str(ROOT / 'tests'), '-I' + str(OUT),
         ROOT / 'tests/window_trackmouse_fixture.c', ROOT / 'android/host/win64_window.c', ROOT / 'android/host/win64_kernel.c',
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', binary])
    if before != {name: sha(ROOT / name) for name in files}: raise ValueError('TrackMouseEvent sources changed during fixtures')
    report = {'success': True, 'native_oracle': oracle, 'original_caller': {'va': '0x141b1eee0', 'bytes': expected.hex(),
              'sha256': hashlib.sha256(expected).hexdigest(), 'cbSize': 24, 'flags': 2, 'hover_uninitialized': True},
              'sources': before, 'commands': commands, 'binary': str(binary.relative_to(ROOT)), 'binary_sha256': sha(binary),
              'device_executed': False, 'scope': 'client LEAVE, QUERY, CANCEL; hover/nonclient rejected; original bytes remain NX'}
    path = OUT / 'result.json';path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'receipt': str(path), 'binary': str(binary), 'binary_sha256': sha(binary)}, indent=2))


if __name__ == '__main__': main()
