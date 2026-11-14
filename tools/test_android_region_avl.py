"""Isolated AVL augmentation propagation, exact current production region bodies.

No production edits, device actions or game. The baseline and candidate retain
the same allocation, ownership, cache and epoch implementation. ARM benchmarks
load two DSOs to retain ordinary dynamic TLS rather than CLI local-exec TLS.
"""
import difflib
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-region-avl-candidate'
BASELINE_SHA = 'a8381b4ad5d729e24e891cd31fd01827590e39563c1f0b350d02c537bc3498c0'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Exact AVL preimage differs: ' + old[:100])
    return text.replace(old, new, 1)


def transform(text):
    changes = []
    for operation in ('insert', 'remove'):
        old = ('    if(region_before(record,root))root->left=region_tree_' + operation + '(root->left,record);\n'
               '    else root->right=region_tree_' + operation + '(root->right,record);\n')
        new = ('    region **child=region_before(record,root)?&root->left:&root->right;\n'
               '    unsigned old_height=region_height(*child);\n'
               '    uint64_t old_max=region_max_end(*child);\n'
               '    *child=region_tree_' + operation + '(*child,record);\n'
               '    if(region_height(*child)==old_height && region_max_end(*child)==old_max)return root;\n')
        text = once(text, old, new)
        changes.append((old, new))
    return text, changes


def extract(text):
    def block(first, last):
        return text[text.index(first):text.index(last)]
    preamble = '''#define _GNU_SOURCE
#define WIN64_REGION_CACHE_TEST 1
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#if defined(_WIN32)
#include "region_pthread_win32.h"
#else
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#define MODULE_CAPACITY 64
#define REGION_CACHE_CAPACITY 64
#define REGION_PROFILE(...) ((void)0)
static struct {uint64_t base;size_t bytes;} images[MODULE_CAPACITY];
static size_t image_count;
static _Thread_local uint32_t fixture_error;
static void win64_set_error(uint32_t value){fixture_error=value;}
'''
    result = preamble
    for first, last in [
        ('typedef struct region {', 'static module modules['),
        ('static pthread_mutex_t metadata_mutex;', 'static pthread_once_t pointer_cookie_once'),
        ('static _Noreturn void invalid(', '/* The list/index is authoritative'),
        ('/* The list/index is authoritative', '/* Optional one-run telemetry.'),
        ('/* Intrusive AVL address index:', 'static int host_protection('),
        ('typedef struct allocation {', 'uint64_t win64_argument('),
        ('static void region_cache_compare(', 'static void onexit_registration_self_test('),
    ]:
        result += block(first, last)
    return result


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'android/host/win64.c'
    raw = source.read_bytes()
    if sha(source) != BASELINE_SHA:
        raise ValueError('Current isolated baseline differs')
    text = source.read_text()
    newline = '\r\n' if b'\r\n' in raw else '\n'
    if text.replace('\n', newline).encode() != raw:
        raise ValueError('Unexpected production newline style')
    (OUT / 'win64.before.c').write_bytes(raw)
    candidate, changes = transform(text)
    restored = candidate
    for old, new in reversed(changes):
        restored = once(restored, new, old)
    if restored.replace('\n', newline).encode() != raw:
        raise ValueError('Candidate changed bytes outside two ancestor blocks')
    (OUT / 'win64.candidate.c').write_text(candidate, newline=newline)
    (OUT / 'candidate.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True), candidate.splitlines(True),
        fromfile='android/host/win64.c', tofile='android/host/win64.c')))
    pins = [source, ROOT / 'tools/test_android_region_tls.py', ROOT / 'tools/leaf_probe.py', Path(__file__),
            ROOT / 'tests/android_region_avl_fixture.c', ROOT / 'tests/android_region_avl_runner.c',
            ROOT / 'tests/android_region_tls_fixture.c', ROOT / 'tests/region_cache_byte_oracle.c',
            ROOT / 'tests/region_pthread_win32.h', ROOT / 'tests/region_atomic_host.h',
            ROOT / 'android/host/win64.h', ROOT / 'android/host/win64_kernel.c', ROOT / 'native/runtime64/cpu64.h']
    before = {str(path.relative_to(ROOT)): sha(path) for path in pins}
    config = json.loads((ROOT / 'port.json').read_text())
    ndk = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    compiler = ndk / 'clang.exe'
    bins, env = windows_toolchain()
    include = OUT / 'host-include';include.mkdir(exist_ok=True)
    (include / 'stdatomic.h').write_bytes((ROOT / 'tests/region_atomic_host.h').read_bytes())
    commands = []
    def run(command, expected=0):
        command = list(map(str, command));commands.append(command)
        p = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=240)
        if p.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps(dict(command=command, expected=expected,
                actual=p.returncode, stdout=p.stdout, stderr=p.stderr), indent=2) + '\n')
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + p.stdout[-2000:] + p.stderr[-6000:])
        return p
    def host_build(directory, item, name, extra=(), dll=False):
        obj = directory / (name + '.obj');binary = directory / (name + ('.dll' if dll else '.exe'))
        run([compiler, '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11', '-fno-strict-aliasing',
             '-I'+str(include), '-I'+str(directory), '-I'+str(ROOT/'tests'), '-Wno-deprecated-declarations',
             *extra, '-c', item, '-o', obj])
        run([bins/'link.exe', '/nologo', *(['/dll'] if dll else []), '/out:'+str(binary), obj,
             'libcmt.lib', 'oldnames.lib', 'kernel32.lib'])
        return binary
    def arm_build(directory, item, name, extra=()):
        binary = directory / name
        run([compiler, '--target=aarch64-linux-android30', '-std=c11', '-O2', '-fno-strict-aliasing',
             '-I'+str(directory), '-I'+str(ROOT/'tests'), *extra, item,
             '-Wl,-z,max-page-size=16384', '-pthread', '-ldl', '-lm', '-o', binary])
        return binary
    host, arms = {}, {}
    negatives = {'--bad-geometry':'region geometry', '--bad-overflow':'region geometry',
        '--bad-owner':'remove unknown region', '--bad-tree':'region address index missing record',
        '--bad-mutation':'unbalanced region mutation boundary', '--bad-epoch':'region mutation epoch exhausted or inconsistent'}
    for name, whole in [('baseline', text), ('candidate', candidate)]:
        directory = OUT/name;directory.mkdir(exist_ok=True)
        (directory/'production_region.inc').write_text(extract(whole), newline='\n')
        tls_text = (ROOT/'tests/android_region_tls_fixture.c').read_text()
        tls_text = once(tls_text, '#include "../android/host/win64.c"', '#include "production_region.inc"')
        (directory/'tls-fixture.c').write_text(tls_text)
        proof = host_build(directory, ROOT/'tests/android_region_avl_fixture.c', 'proof')
        p = run([proof]);(directory/'host-proof.log').write_text(p.stdout+p.stderr)
        semantic = json.loads(p.stdout.splitlines()[-1])
        for option, diagnostic in negatives.items():
            if diagnostic not in run([proof,option],expected=73).stderr:raise ValueError('Wrong negative diagnostic')
        tls = host_build(directory,directory/'tls-fixture.c','tls')
        p = run([tls]);(directory/'host-tls.log').write_text(p.stdout+p.stderr)
        tls_result = json.loads(p.stdout)
        host[name] = dict(semantic=semantic, tls={key:value for key,value in tls_result.items() if key not in ('fast_ns','locked_oracle_ns')},
            tls_model_timing={key:tls_result[key] for key in ('fast_ns','locked_oracle_ns')}, stop73=list(negatives))
        arm = arm_build(directory, ROOT/'tests/android_region_avl_fixture.c', 'proof-arm64')
        arm_tls = arm_build(directory,directory/'tls-fixture.c','tls-arm64')
        dso = arm_build(directory, ROOT/'tests/android_region_avl_fixture.c', 'libregion-avl.so',
            ['-fPIC','-shared','-DFP_AVL_BENCH_LIBRARY=1'])
        host_build(directory, ROOT/'tests/android_region_avl_fixture.c', 'region-avl', ['-DFP_AVL_BENCH_LIBRARY=1'], dll=True)
        arms[name] = {kind:dict(path=str(path.relative_to(ROOT)),sha256=sha(path)) for kind,path in [('proof',arm),('tls',arm_tls),('benchmark_dso',dso)]}
    if host['baseline']['semantic'] != host['candidate']['semantic'] or host['baseline']['tls'] != host['candidate']['tls']:
        raise ValueError('Paired host semantic proof differs')
    # This historical oracle specifically kills the unsafe pre-assignment exit
    # and the tempting height-only propagation variant.
    mutants = {}
    for name, mutated, option in [('lost-child', candidate.replace('*child=region_tree_insert(*child,record);',
            'region *new_child=region_tree_insert(*child,record);\n    if(region_height(new_child)==old_height && region_max_end(new_child)==old_max)return root;\n    *child=new_child;',1), '--rotations-only'),
        ('height-only', candidate.replace(' && region_max_end(*child)==old_max',''), '--maximum-only')]:
        directory=OUT/name;directory.mkdir(exist_ok=True)
        (directory/'production_region.inc').write_text(extract(mutated))
        binary=host_build(directory,ROOT/'tests/android_region_avl_fixture.c','proof')
        p=run([binary,option],expected=1);mutants[name]=dict(rejected=True,diagnostic=p.stderr.strip())
    runner=host_build(OUT,ROOT/'tests/android_region_avl_runner.c','benchmark-host')
    p=run([runner,OUT/'baseline/region-avl.dll',OUT/'candidate/region-avl.dll'])
    benchmark=json.loads(p.stdout)
    for row in benchmark['scenarios']:
        row['mode_description']=['warm new registration plus repeated snapshot validation',
            'mixed protection/decommit split and multi-fragment removal',
            'overlap crossing and reveal previous priority',
            'cold-start 32MiB eviction after setup; empty TLS snapshots per cycle'][row['mode']]
        row['median_baseline_ns_per_cycle']=statistics.median(row['baseline_ns'])/row['cycles']
        row['median_candidate_ns_per_cycle']=statistics.median(row['candidate_ns'])/row['cycles']
        row['median_speedup']=statistics.median(row['baseline_ns'])/statistics.median(row['candidate_ns'])
    (OUT/'host-benchmark.json').write_text(json.dumps(benchmark,indent=2)+'\n')
    arm_runner=arm_build(OUT,ROOT/'tests/android_region_avl_runner.c','benchmark-arm64')
    disassembly=run([ndk/'llvm-objdump.exe','-d','--no-show-raw-insn',OUT/'candidate/libregion-avl.so'])
    (OUT/'candidate-arm-dso-disassembly.txt').write_text(disassembly.stdout)
    # Check the candidate in its actual full translation unit, not extraction only.
    object_file=OUT/'win64-candidate-arm64.o'
    run([compiler,'--target=aarch64-linux-android30','-std=c11','-O2','-fno-strict-aliasing','-fPIC',
         '-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'generated/frostpunk-x64'),'-I'+str(ROOT/'generated/win64'),
         '-iquote',ROOT/'android/host','-c',OUT/'win64.candidate.c','-o',object_file])
    if before != {name:sha(ROOT/name) for name in before}:raise ValueError('Source drift during isolated qualification')
    generated={str(path.relative_to(ROOT)):sha(path) for path in OUT.rglob('*') if path.suffix in ('.c','.inc','.patch','.h')}
    result=dict(success=True,source_hashes=before,baseline_sha256=BASELINE_SHA,candidate_sha256=sha(OUT/'win64.candidate.c'),
        exact_two_block_roundtrip=True,host=host,mutation_tests=mutants,host_model_benchmark=benchmark,
        arm=arms,arm_benchmark=dict(path=str(arm_runner.relative_to(ROOT)),sha256=sha(arm_runner),
            args=['./baseline/libregion-avl.so','./candidate/libregion-avl.so']),
        whole_translation_unit=dict(path=str(object_file.relative_to(ROOT)),sha256=sha(object_file)),
        generated=generated,commands=commands,scope='Exact extracted production region functions; byte oracle independent; Windows pthread adapter only. ARM benchmark DSOs retain dynamic TLS. Synthetic occupancies do not assert actual game population or improvement; cold pass evicts32MiB after setup/outside timing and empties TLS snapshots per cycle, not each CPU cache line per iteration.',
        production_modified=False,device_executed=False,game_gain_proven=False)
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(success=True,host=host['candidate'],benchmark=[{k:v for k,v in row.items() if k not in ('baseline_ns','candidate_ns')} for row in benchmark['scenarios']],
        arm=arms,arm_benchmark=result['arm_benchmark'],receipt_sha256=sha(OUT/'result.json')),indent=2))


if __name__ == '__main__':
    main()
