"""Check live mobile effects and cached diagnostic overrides on Android ARM64."""
import argparse
import json
import subprocess
from pathlib import Path
import fastbuild as fb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--compile-only', action='store_true')
    args = parser.parse_args()
    folder = fb.ROOT / 'build/fast/mobile-quality/proof'
    folder.mkdir(parents=True, exist_ok=True)
    sources = [fb.ROOT / 'tests/mobile_quality_fixture.c', fb.ROOT / 'android/host/mobile_quality.c']
    pins = {str(p): fb.sha(p) for p in sources}
    binary = folder / 'quality-arm64'
    subprocess.run([str(fb.CLANG), *fb.COMPILE_FLAGS, *['-I' + str(fb.ROOT / p) for p in fb.INCLUDES],
                    *map(str, sources), '-Wl,-z,max-page-size=16384', '-o', str(binary)], check=True)
    if args.compile_only:
        print('Mobile effects and particle gate fixture compiled')
        return
    adb = ['adb', '-s', args.serial]
    def run(*command):
        return subprocess.check_output([*adb, *map(str, command)], text=True)
    assert not run('shell', 'pidof com.frostpunk.staticrecomp || true').strip()
    previous = {p: run('shell', 'getprop ' + p).strip() for p in ('debug.fp.quality', 'debug.fp.extra_fx')}
    remote = '/data/local/tmp/frostpunk-quality-' + fb.sha(binary)[:12]
    results = []
    try:
        run('push', binary, remote)
        run('shell', 'chmod 700 ' + remote)
        for quality, mode in (('', -1), ('0', 0), ('1', 1), ('2', 2), ('3', 3), ('9999999999999999999999999999', 3)):
            run('shell', "setprop debug.fp.quality '" + quality + "'")
            for particles, disabled in (('', -1), ('0', 0), ('4', 1)):
                run('shell', "setprop debug.fp.extra_fx '" + particles + "'")
                result = json.loads(run('shell', remote, mode, disabled))
                assert result['success'] and result['cases'] == 1024
                results.append(dict(quality=quality, particles=particles, **result))
    finally:
        for prop, value in previous.items():
            run('shell', "setprop " + prop + " '" + value + "'")
        run('shell', 'rm -f ' + remote)
    assert pins == {name: fb.sha(Path(name)) for name in pins}
    report = dict(success=True, results=results, sources=pins, arm_binary_sha256=fb.sha(binary))
    (folder / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(dict(success=True, modes=len(results), cases=sum(r['cases'] for r in results))))


if __name__ == '__main__':
    main()
