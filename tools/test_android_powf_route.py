"""Isolated exact powf import route candidate; no production writes or ADB."""
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-powf-route-test'
PREIMAGES = {'android/host/win64.c': 'd26c0150aebbe0f5954e04e8c1103cded6a74fd9f95a7fc3dac4ec2a8fac10e6',
             'android/host/win64_stdio.c': 'd9315713f62f56aba1987e94a36bc0a5a4d15d3a2e42a45a1f2e498eca8343a4'}
PROFILE = {'build/app-game/title-profile/profile-full-tree.txt': '3ae4acdbbd2039e7bbe2c255a39954981449d1081fa9bbdcfca4b0a5bd8dc207',
           'build/app-game/title-profile/profile-symbols.txt': '536f77bd6cfb3f7193a57185a0e9d7a11861160aa6b70029a69ee1b8e8e56191',
           'build/app-game/title-profile/record.json': 'd255575f92abc8a5caf8c3d6793c0538d59d1e18dd587b218659526c53f87610'}
EARLY = '''    if (crt && !strcmp(name,"powf")) {
        if (!win64_stdio_call(c,name) && !win64_math_call(c,name)) return 0;
        c->rsp+=8;return 1;
    }
'''
STDIO_OLD = '       !strcmp(name,"strcpy_s")||!strcmp(name,"strncmp"))return 0;'
STDIO_NEW = '       !strcmp(name,"strcpy_s")||!strcmp(name,"strncmp")||!strcmp(name,"powf"))return 0;'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, expected in {**PREIMAGES, **PROFILE}.items():
        if sha(ROOT / name) != expected:
            raise ValueError('Frozen powf candidate input drift: ' + name)
    names = [*PREIMAGES, *PROFILE, 'android/host/win64_math.c', 'android/host/win64_math.h',
             'android/host/win64_string.c', 'android/host/win64_string.h', 'android/host/win64_stdio.h',
             'android/host/win64.h', 'tests/android_powf_route_fixture.c',
             'tests/android_crt_hotpath_fixture.c', 'tools/test_android_powf_route.py', 'port.json']
    names += [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / 'native/runtime64').glob('*.h'))]
    before = {name: sha(ROOT / name) for name in names}
    core = (ROOT / 'android/host/win64.c').read_text()
    anchor = '    const int crt=is_crt(dll);\n'
    if core.count(anchor) != 1:
        raise ValueError('Exact CRT router anchor changed')
    candidate = core.replace(anchor, anchor + EARLY)
    stdio = (ROOT / 'android/host/win64_stdio.c').read_text()
    if stdio.count(STDIO_OLD) != 1:
        raise ValueError('Exact stdio decline anchor changed')
    candidate_stdio = stdio.replace(STDIO_OLD, STDIO_NEW)
    core_path = OUT / 'candidate_win64.c'
    stdio_path = OUT / 'candidate_stdio.c'
    core_path.write_text(candidate, newline='\n')
    stdio_path.write_text(candidate_stdio, newline='\n')
    diff = ''.join(difflib.unified_diff(core.splitlines(True), candidate.splitlines(True),
                                      fromfile='android/host/win64.c', tofile='candidate_win64.c'))
    diff += ''.join(difflib.unified_diff(stdio.splitlines(True), candidate_stdio.splitlines(True),
                                       fromfile='android/host/win64_stdio.c', tofile='candidate_stdio.c'))
    patch = OUT / 'candidate.patch'
    patch.write_text(diff, newline='\n')
    original = core[core.index('int win64_dispatch_import('):core.index('int win64_dispatch_dynamic(')]
    modified = candidate[candidate.index('int win64_dispatch_import('):candidate.index('int win64_dispatch_dynamic(')]
    declarations = core[core.index('static int is_crt('):core.index('static uint64_t crt_allocate(')]
    for variant, body in [('baseline', original), ('candidate', modified)]:
        api = set(re.findall(r'\b(win64_[a-z0-9_]+_call)\(', body))
        imports = set(re.findall(r'\b(win64_[a-z0-9_]+_import)\(', body)) - {'win64_dispatch_import'}
        mapping = {'win64_stdio_call': 'old_stdio' if variant == 'baseline' else 'new_stdio',
                   'win64_string_call': 'real_string', 'win64_math_call': 'routed_math'}
        for symbol in sorted(api):
            declarations += '#define ' + symbol + ' ' + mapping.get(symbol, 'unrelated_api') + '\n'
        for symbol in sorted(imports): declarations += '#define ' + symbol + ' unrelated_import\n'
        declarations += '#define win64_threads_raise_exception unrelated_notification\n#define execute unrelated_import\n'
        declarations += body.replace('int win64_dispatch_import(', 'static int ' + variant + '_route(')
        for symbol in sorted(api | imports | {'win64_threads_raise_exception', 'execute'}):
            declarations += '#undef ' + symbol + '\n'
    routers = OUT / 'crt_routers.inc'
    routers.write_text(declarations, newline='\n')
    cfg = json.loads((ROOT / 'port.json').read_text())
    llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = llvm / 'clang.exe'
    bins, env = windows_toolchain()
    commands = []

    def run(args, expected=0):
        commands.append(list(map(str, args)))
        result = subprocess.run(commands[-1], capture_output=True, text=True, env=env, timeout=90)
        if result.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + result.stdout + result.stderr)
        return result

    includes = ['-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'), '-I' + str(OUT)]
    sources = [(ROOT / 'tests/android_powf_route_fixture.c', []),
               (ROOT / 'android/host/win64_stdio.c', ['-Dwin64_stdio_call=baseline_stdio_call', '-Dwin64_stdio_self_test=baseline_stdio_self_test', '-D_Exit=powf_route_fixture_stop']),
               (stdio_path, ['-D_Exit=powf_route_fixture_stop']),
               (ROOT / 'android/host/win64_string.c', []),
               (ROOT / 'android/host/win64_math.c', ['-D_Exit=powf_route_fixture_stop'])]
    for target, suffix in [('x86_64-pc-windows-msvc', '.obj'), ('aarch64-linux-android30', '.arm.o')]:
        objects = []
        for source, defines in sources:
            obj = OUT / (source.stem + suffix)
            objects.append(obj)
            run([compiler, '--target=' + target, '-O2', '-std=c11', '-fno-strict-aliasing',
                 '-D_CRT_SECURE_NO_WARNINGS', '-Wno-deprecated-declarations', *includes, *defines,
                 '-c', source, '-o', obj])
        if target.startswith('x86_64'):
            host_binary = OUT / 'powf-route-host.exe'
            run([bins / 'link.exe', '/nologo', '/OUT:' + str(host_binary), *objects,
                 'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
        else:
            arm = OUT / 'powf-route-arm64'
            run([compiler, '--target=' + target, *objects, '-lm', '-pthread',
                 '-Wl,-z,max-page-size=16384', '-o', arm])
    host_run = run([host_binary])
    output = [json.loads(line) for line in host_run.stdout.splitlines()]
    benchmark, host = output
    cold, negative = [], []
    for dll in ('ucrtbase.dll', 'msvcrt.dll', 'api-ms-win-crt-math-l1-1-0.dll', 'vcruntime140.dll', 'UcRtBaSe.DlL'):
        cold.append(json.loads(run([host_binary, '--cold', dll]).stdout))
    for name in ('precision', 'pole', 'negative', 'subnormal_base', 'subnormal_power', 'NaN',
                 'infinity', 'lower_band', 'upper_band', 'underflow', 'overflow'):
        pair = []
        for variant in ('baseline', 'candidate'):
            result = run([host_binary, '--negative', variant, name], 73)
            if 'POWF_ROUTE_NEGATIVE full_CPU=1 registrations=4 RSP_unpopped=1 FP_preserved=1' not in result.stderr:
                raise ValueError('Negative route bypassed cold stdio/CPU/FP/pop boundary')
            pair.append(result.stderr)
        if pair[0] != pair[1]:
            raise ValueError('Negative route failure changed: ' + name)
        negative.append({'name': name, 'exit_code': 73, 'stderr': pair[0]})
    if before != {name: sha(ROOT / name) for name in names}:
        raise ValueError('Powf route candidate input changed during proof')
    result = {'success': True, 'production_edited': False, 'host': host, 'benchmark': benchmark,
              'cold_first_calls': cold, 'negative': negative, 'source_hashes': before,
              'candidates': {path.relative_to(ROOT).as_posix(): sha(path) for path in (core_path, stdio_path, patch, routers)},
              'host_binary_sha256': sha(host_binary), 'compiler_sha256': sha(compiler),
              'arm': {'compiled': True, 'executed': False, 'binary': str(arm.relative_to(ROOT)), 'binary_sha256': sha(arm),
                      'argv': [], 'expected': host, 'cold_argv': ['--cold', 'CRT_DLL'],
                      'negative_argv': ['--negative', 'baseline|candidate', 'CASE']},
              'profile_scope': '3.94 percentage points is the total three-caller subtree budget, including real math, dispatch and arithmetic; not measured removable work.',
              'scope': 'Exact paired production routers and real stdio/string/math; other providers stub pure declines after independent source review. Math algorithm unchanged; no expf route.',
              'device_actions': False, 'game_executed': False, 'commands': commands}
    receipt = OUT / 'result.json'
    receipt.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'success': True, 'host': host, 'benchmark': benchmark, 'cold_cases': len(cold),
                      'negative_cases': len(negative) * 2, 'arm': result['arm'],
                      'receipt': str(receipt), 'receipt_sha256': sha(receipt)}, indent=2))


if __name__ == '__main__':
    main()
