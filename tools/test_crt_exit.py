"""Build production CRT exit callbacks; an explicit device also checks exit status."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args = parser.parse_args()
    config = json.loads((ROOT / 'port.json').read_text())
    sdk = Path(config['android_sdk'])
    clang = sdk / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out = ROOT / 'build/audit-20261108'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'android/host/win64.c').read_text()
    helpers = source[source.index('static int crt_atexit_register('):source.index('static int execute(CPU *c,')]
    route = source[source.index('        if (!strcmp(name,"_execute_onexit_table"))'):source.index('        if (!strcmp(name,"_initterm")')]
    (out / 'crt_exit_production.inc').write_text(helpers + '\nstatic int fixture_exit_route(CPU*c,const char*name){uint64_t a=c->rcx;\n' + route + '\nreturn 0;}\n')
    binary = out / 'crt-exit-arm64'
    subprocess.run([str(clang), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    '-I' + str(ROOT / 'native/runtime64'), '-I' + str(out), str(ROOT / 'tests/crt_exit_fixture.c'),
                    '-Wl,-z,max-page-size=16384', '-o', str(binary)], check=True)
    result = {'compiled': True, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'device_executed': False}
    from leaf_probe import windows_toolchain
    bins, env = windows_toolchain()
    host = out / 'crt-exit-host.exe'
    compile_host = subprocess.run([str(bins / 'cl.exe'), '/nologo', '/O2', '/std:c11', '/W3',
                                  '/I' + str(ROOT / 'native/runtime64'), '/I' + str(out),
                                  str(ROOT / 'tests/crt_exit_fixture.c'), '/Fo' + str(out / 'crt-exit-host.obj'),
                                  '/Fe' + str(host)], capture_output=True, text=True, env=env)
    assert compile_host.returncode == 0, compile_host.stdout + compile_host.stderr
    run = subprocess.run([str(host)], capture_output=True, text=True, check=True)
    result['host'] = json.loads(run.stdout)
    assert result['host']['success']
    for name in ['exit', '_exit', '_Exit']:
        child = subprocess.run([str(host), name], capture_output=True, text=True)
        assert child.returncode == 37, (name, child.returncode, child.stdout, child.stderr)
        assert ('callback=7' in child.stdout) == (name == 'exit'), (name, child.stdout)
        assert 'exiting=37' in child.stderr
    result['host_termination_status_and_callback_policy'] = True
    if args.serial:
        adb = [str(sdk / 'platform-tools/adb.exe'), '-s', args.serial]
        remote = '/data/local/tmp/frostpunk-crt-exit-oracle'
        subprocess.run(adb + ['push', str(binary), remote], capture_output=True, check=True)
        subprocess.run(adb + ['shell', 'chmod', '700', remote], capture_output=True, check=True)
        run = subprocess.run(adb + ['shell', remote], capture_output=True, text=True, timeout=30, check=True)
        result.update(device_executed=True, oracle=json.loads(run.stdout))
        assert result['oracle']['success']
        for name in ['exit', '_exit', '_Exit']:
            child = subprocess.run(adb + ['shell', remote, name], capture_output=True, text=True, timeout=30)
            assert child.returncode == 37, (name, child.returncode, child.stdout, child.stderr)
            assert ('callback=7' in child.stdout) == (name == 'exit'), (name, child.stdout)
            assert 'exiting=37' in child.stderr
        result['termination_status_and_callback_policy'] = True
    (out / 'crt-exit-result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))

if __name__ == '__main__':
    main()
