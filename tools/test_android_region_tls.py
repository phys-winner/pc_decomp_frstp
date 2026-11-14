"""Actual Android region code: host byte oracle, forced race, and standalone ARM CLI.

No fetch/device/game. Host substitutes pthread primitives only; all region/cache
functions and mutation boundaries are extracted verbatim from current win64.c.
The ARM CLI includes the entire production win64.c, dead-stripping unused APIs.
"""
import hashlib
import json
from pathlib import Path
import subprocess

from leaf_probe import ROOT, windows_toolchain


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    out = ROOT / 'build/android-region-tls-test'
    out.mkdir(parents=True, exist_ok=True)
    atomic_include = out / 'host-include'
    atomic_include.mkdir(exist_ok=True)
    (atomic_include / 'stdatomic.h').write_bytes((ROOT / 'tests/region_atomic_host.h').read_bytes())
    config = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version']
    compiler = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    source = ROOT / 'android/host/win64.c'
    text = source.read_text()

    def block(first, last):
        return text[text.index(first):text.index(last)]

    preamble = '''#define WIN64_REGION_CACHE_TEST 1
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include "region_pthread_win32.h"
#define MODULE_CAPACITY 64
#define REGION_CACHE_CAPACITY 64
#define REGION_PROFILE(...) ((void)0)
static struct {uint64_t base;size_t bytes;} images[MODULE_CAPACITY];
static size_t image_count;
static _Thread_local uint32_t fixture_error;
static void win64_set_error(uint32_t value){fixture_error=value;}
'''
    extracted = preamble + block('typedef struct region {', 'static module modules[')
    extracted += block('static pthread_mutex_t metadata_mutex;', 'static pthread_once_t pointer_cookie_once')
    extracted += block('static _Noreturn void invalid(', '/* The list/index is authoritative')
    extracted += block('/* The list/index is authoritative', '/* Optional one-run telemetry.')
    extracted += block('/* Intrusive AVL address index:', 'static int host_protection(')
    extracted += block('typedef struct allocation {', 'uint64_t win64_argument(')
    extracted += block('static void region_cache_compare(', 'static void onexit_registration_self_test(')
    include = out / 'production_region.inc'
    include.write_text(extracted, newline='\n')
    independent = out / 'byte-oracle.c'
    independent.write_text('#include "production_region.inc"\n' +
                           (ROOT / 'tests/region_cache_byte_oracle.c').read_text(), newline='\n')
    files = [source, ROOT / 'android/host/win64.h', ROOT / 'android/host/win64_kernel.c',
             ROOT / 'tests/android_region_tls_fixture.c', ROOT / 'tests/region_cache_byte_oracle.c',
             ROOT / 'tests/region_pthread_win32.h', ROOT / 'tests/region_atomic_host.h', Path(__file__).resolve()]
    before = {str(p.relative_to(ROOT)): sha(p) for p in files}
    bins, env = windows_toolchain()
    commands = []

    def run(command, expected=0):
        commands.append(list(map(str, command)))
        p = subprocess.run(commands[-1], env=env, cwd=out, capture_output=True,
                           text=True, encoding='utf-8', errors='replace', timeout=90)
        if p.returncode != expected:
            raise RuntimeError(subprocess.list2cmdline(commands[-1]) + '\n' + p.stdout + p.stderr)
        return p

    def host_build(item, name):
        obj, exe = out / (name + '.obj'), out / (name + '.exe')
        run([compiler, '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11',
             '-I' + str(atomic_include), '-I' + str(out), '-I' + str(ROOT / 'tests'), '-Wno-deprecated-declarations',
             '-c', item, '-o', obj])
        run([bins / 'link.exe', '/nologo', '/out:' + str(exe), obj,
             'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
        return exe

    fixture = host_build(ROOT / 'tests/android_region_tls_fixture.c', 'region-tls-host')
    host = json.loads(run([fixture]).stdout)
    negative = run([fixture, '--epoch-overflow'], expected=73)
    if 'region mutation epoch exhausted or inconsistent' not in negative.stderr:
        raise ValueError('Wrong epoch exhaustion diagnostic')
    oracle = host_build(independent, 'byte-oracle-host')
    reference = json.loads(run([oracle]).stdout)
    arm = out / 'region-tls-arm64'
    run([compiler, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-fno-strict-aliasing',
         '-ffunction-sections', '-fdata-sections', '-I' + str(ROOT / 'native/runtime64'),
         '-I' + str(ROOT / 'generated/frostpunk-x64'), '-I' + str(ROOT / 'generated/win64'),
         '-I' + str(ROOT / 'android/host'), ROOT / 'tests/android_region_tls_fixture.c',
         '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-lm', '-o', arm])
    if before != {str(p.relative_to(ROOT)): sha(p) for p in files}:
        raise ValueError('Source changed while region tests ran')
    # Physical mutation sites must remain inside the exclusion boundary. Other
    # registered releases were audited to remove their region before OS release.
    kernel = (ROOT / 'android/host/win64_kernel.c').read_text()
    if not ('win64_region_mutation_begin();\n        if(munmap(v->base,v->bytes))' in kernel and
            '*p=v->next;win64_region_remove(c->rcx);win64_region_mutation_end();' in kernel):
        raise ValueError('UnmapViewOfFile lost its physical mutation boundary')
    result = dict(success=True, host=host, independent_byte_oracle=reference,
                  epoch_overflow_exit73=True, slow_oracle_preserved=True,
                  arm_binary=str(arm.relative_to(ROOT)), arm_binary_sha256=sha(arm),
                  source_hashes=before, extracted_source_sha256=sha(include),
                  device_executed=False, game_boot_proven=False, commands=commands)
    receipt = out / 'result.json'
    receipt.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(success=True, host=host, independent_queries=reference['legacy_queries'] + reference['extra_queries'],
                          arm=str(arm.relative_to(ROOT)), arm_sha256=sha(arm))))


if __name__ == '__main__':
    main()
