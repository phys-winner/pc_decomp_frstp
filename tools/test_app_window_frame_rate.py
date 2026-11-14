"""Source-backed window-generation/120Hz desktop regression, without ADB."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from leaf_probe import ROOT, windows_toolchain


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate-dir', type=Path)
    args = parser.parse_args()
    source = (args.candidate_dir.resolve() if args.candidate_dir else ROOT)
    out = source / 'persistent-regression' if args.candidate_dir else ROOT / 'build/app-window-frame-rate-test'
    out.mkdir(parents=True, exist_ok=True)
    app = source / 'android/app/app.c'
    runtime = source / 'android/host/android_runtime.c'
    fixture = ROOT / 'tests/app_window_frame_rate_fixture.c'
    paths = [app, runtime, fixture, Path(__file__).resolve(), ROOT / 'android/host/win64_display.c',
             ROOT / 'android/host/win64_display.h', ROOT / 'native/runtime64/cpu64.h',
             ROOT / 'tests/display_fixture.c', ROOT / 'tests/display_change_fixture.c',
             ROOT / 'tests/environment_windows_host/pthread.h']
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    text = app.read_text(encoding='utf-8')
    start = text.index('#ifdef FP_GAME_RUNTIME\nstatic void request_window_frame_rate(')
    end = text.index('\nstatic int write_all(', start)
    extracted = text[start:end]
    if (extracted.count('ANativeWindow_setFrameRate(window,120.0f,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT)') != 1
            or extracted.count('request_window_frame_rate(window,c->generation);') != 1):
        raise ValueError('Window generation vote contract changed')
    policy = runtime.read_text(encoding='utf-8')
    if any(policy.count(x) != 1 for x in ['win64_display_configure(2944,1840,120);',
            'win64_d3d11_set_display_mode(2944,1840,120);', 'guest_virtual_display=2944x1840@120']):
        raise ValueError('Runtime display policies differ')
    (out / 'window_generation.inc').write_text(extracted + '\n', encoding='utf-8')
    for name in ['display_fixture.c', 'display_change_fixture.c']:
        data = (ROOT / 'tests' / name).read_text(encoding='utf-8')
        for old, new in [(960, 2944), (540, 1840), (959, 2943), (539, 1839), (1000, 2945), (1001, 2946), (60, 120)]:
            data = re.sub(r'\b' + str(old) + r'\b', str(new), data)
        data = data.replace('virtual960x540x32at60', 'virtual2944x1840x32at120')
        if name == 'display_change_fixture.c':
            if data.count('int main(int argc,char **argv)') != 1:
                raise ValueError('Display matrix entry changed')
            data = data.replace('int main(int argc,char **argv)', 'int display_matrix_main(int argc,char **argv)')
        (out / name).write_text(data, encoding='utf-8')
    bins, env = windows_toolchain()
    cfg = json.loads((ROOT / 'port.json').read_text(encoding='utf-8'))
    clang = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    commands = []

    def run(args, expected=0):
        command = list(map(str, args));commands.append(command)
        p = subprocess.run(command, cwd=out, env=env, capture_output=True, text=True, timeout=60)
        if p.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + p.stdout + p.stderr)
        return {'exit_code': p.returncode, 'stdout': p.stdout, 'stderr': p.stderr}

    includes = [ROOT / 'native/runtime64', ROOT / 'android/host', out]
    sources = [fixture, out / 'display_change_fixture.c', ROOT / 'android/host/win64_display.c']
    host = {}
    for game in (1, 0):
        exe = out / ('window-game.exe' if game else 'window-probe.exe')
        run([bins / 'cl.exe', '/nologo', '/O2', '/std:c11', *(['/DFP_GAME_RUNTIME=1'] if game else []),
             *['/I' + str(p) for p in includes], '/I' + str(ROOT / 'tests/environment_windows_host'),
             *sources, '/Fo' + str(out) + '/', '/Fe' + str(exe)])
        host[str(game)] = run([exe])
    negatives = {arg: run([out / 'window-game.exe', arg], 73)
                 for arg in ['--flags', '--reserved', '--size', '--extra', '--fields']}
    if any('STOP Win64 display' not in x['stderr'] for x in negatives.values()):
        raise ValueError('Display negative boundary differs')
    arm = out / 'window-frame-rate-arm64'
    run([clang, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-fno-strict-aliasing',
         '-ffunction-sections', '-fdata-sections', '-DFP_GAME_RUNTIME=1',
         *['-I' + str(p) for p in includes], *sources, '-Wl,--gc-sections',
         '-Wl,-z,max-page-size=16384', '-pthread', '-lm', '-o', arm])
    if pins != {str(p.relative_to(ROOT)): sha(p) for p in paths}:
        raise ValueError('Source drift during regression')
    result = {'success': True, 'candidate_source': bool(args.candidate_dir), 'production_modified': False,
              'device_executed': False, 'sources': pins, 'host': host, 'negative_cases': negatives,
              'window_generation_extracted_sha256': sha(out / 'window_generation.inc'),
              'ARM': {'path': str(arm.relative_to(ROOT)), 'sha256': sha(arm), 'arguments': []},
              'commands': commands, 'physical_mode_proven': False, 'fps90_proven': False}
    path = out / 'result.json';path.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'success': True, 'receipt': str(path), 'sha256': sha(path), 'ARM': result['ARM']}, indent=2))


if __name__ == '__main__':
    main()
