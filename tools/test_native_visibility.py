"""Compare the typed visibility filter with original lifted instructions on host and ARM64."""
import argparse
import hashlib
import json
import statistics
import subprocess
from pathlib import Path
from fastbuild import ROOT, CLANG, COMPILE_FLAGS
from leaf_probe import windows_toolchain
from native_visibility_overlay import fixture


def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--compile-only', action='store_true')
    args = ap.parse_args()
    out = ROOT / 'build/perf/native-cpu-20261113/visibility-proof'
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'build/arm64/generated-overlay/recomp_funcs_0822.c'
    oracle = out / 'original-prefix.c'
    oracle.write_text(fixture(source.read_text()), encoding='utf-8', newline='\n')
    sources = [ROOT / 'tests/visibility_filter_fixture.c', oracle]
    pins = {str(p): sha(p) for p in [*sources, source, ROOT / 'android/host/visibility_filter_native.h', ROOT / 'native/runtime64/cpu64.h', Path(__file__),
                                    ROOT / 'tools/native_visibility_overlay.py']}
    bins, env = windows_toolchain()

    def run(command):
        r = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=180)
        if r.returncode: raise RuntimeError(r.stdout[-1000:] + r.stderr[-4000:])
        return r.stdout

    flags = ['-O2', '-std=c11', '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off',
             '-fno-math-errno', '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host')]
    objects = []
    for i, path in enumerate(sources):
        obj = out / ('host-%d.obj' % i)
        run([CLANG, '--target=x86_64-pc-windows-msvc', *flags, '-fms-runtime-lib=static', '-c', path, '-o', obj])
        objects.append(obj)
    host = out / 'visibility-host.exe'
    run([bins / 'link.exe', '/nologo', '/OUT:' + str(host), *objects,
         'libcmt.lib', 'libvcruntime.lib', 'libucrt.lib', 'oldnames.lib', 'kernel32.lib'])
    host_result = json.loads(run([host])); assert host_result['success']
    arm = out / 'visibility-arm64'
    run([CLANG, *COMPILE_FLAGS, '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'), *sources,
         '-Wl,-z,max-page-size=16384', '-lm', '-o', arm])
    result = dict(success=True, host=host_result, sources=pins, binary_sha256=sha(arm))
    if not args.compile_only:
        adb = ['adb', '-s', args.serial]
        assert not run([*adb, 'shell', 'pidof com.frostpunk.staticrecomp || true']).strip()
        remote = '/data/local/tmp/frostpunk-visibility-' + sha(arm)[:12]
        run([*adb, 'push', arm, remote]); run([*adb, 'shell', 'chmod', '700', remote])
        try:
            result['device'] = json.loads(run([*adb, 'shell', remote]))
            result['benchmark'] = json.loads(run([*adb, 'shell', remote, '--benchmark']))
        finally:
            run([*adb, 'shell', 'rm', '-f', remote])
        assert result['device']['success'] and result['device']['cases'] == 38400
        result['medians'] = {str(k): {v: statistics.median(p[v] for p in result['benchmark']['pairs'] if p['kind'] == k)
                                    for v in ('original_ns', 'native_ns')} for k in range(6)}
    assert pins == {p: sha(p) for p in pins}
    (out / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('sources', 'benchmark')}), flush=True)


if __name__ == '__main__': main()
