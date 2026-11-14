"""Isolated locked range-history experiment. Never writes production sources."""
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-region-journal-candidate'
BASE_SHA = '72726db44adea6107ac44b6536dc15873847626434fe22ab018c74db686672be'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def candidate_source(source):
    def replace(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise ValueError('Exact journal preimage changed: ' + old[:80])
        source = source.replace(old, new)

    replace('static _Thread_local unsigned region_mutation_depth;', '''static _Thread_local unsigned region_mutation_depth;
#define REGION_JOURNAL_CAPACITY 16
typedef struct { uint64_t epoch,low,high; unsigned known; } region_change;
static region_change region_journal[REGION_JOURNAL_CAPACITY];
static _Thread_local region_change region_pending;
static void region_cache_sync(uint64_t current);''')
    replace('''void win64_region_mutation_begin(void) {
    metadata_lock();
    if(!region_mutation_depth++) {''', '''/* Journal and pending spans are accessed only while metadata_mutex is held.
 * Only a controlled top-level add/remove can prove the complete changed span.
 * Any nested boundary deliberately poisons that proof, including known calls. */
static void region_mutation_start(unsigned known,uint64_t low,uint64_t high) {
    metadata_lock();
    if(!region_mutation_depth++) {''')
    # There are two identical reset statements; change the begin and slow path
    # explicitly together, with an exact count assertion.
    old = 'if(region_cache_epoch!=current){region_cache_count=0;region_cache_epoch=current;}'
    if source.count(old) != 2:
        raise ValueError('Exact locked cache reset sites changed')
    source = source.replace(old, 'region_cache_sync(current);')
    replace('''        region_cache_sync(current);
        atomic_store_explicit(&region_epoch,current+1,memory_order_release);''', '''        region_cache_sync(current);
        region_pending=(region_change){.low=low,.high=high,.known=known};
        atomic_store_explicit(&region_epoch,current+1,memory_order_release);''')
    replace('''        atomic_thread_fence(memory_order_seq_cst);
    }
}
void win64_region_mutation_end(void)''', '''        atomic_thread_fence(memory_order_seq_cst);
    } else region_pending.known=0;
}
void win64_region_mutation_begin(void) { region_mutation_start(0,0,0); }
static void region_mutation_span(uint64_t low,uint64_t high) {
    if(!region_pending.known)return;
    if(low>=high){region_pending.known=0;return;}
    if(region_pending.low>=region_pending.high){region_pending.low=low;region_pending.high=high;}
    else {if(low<region_pending.low)region_pending.low=low;if(high>region_pending.high)region_pending.high=high;}
}
void win64_region_mutation_end(void)''')
    replace('''        region_cache_epoch=current+1;
        atomic_store_explicit(&region_epoch,current+1,memory_order_release);''', '''        region_pending.epoch=current+1;
        if(region_pending.low>=region_pending.high)region_pending.known=0;
        region_journal[((current+1)>>1)&(REGION_JOURNAL_CAPACITY-1)]=region_pending;
        if(!region_pending.known)region_cache_count=0;
        region_cache_epoch=current+1;
        atomic_store_explicit(&region_epoch,current+1,memory_order_release);''')
    replace('''void win64_region_add(uint64_t address, size_t bytes, int writable) {''', '''/* The caller holds metadata_mutex. Missing, odd, future or overwritten history
 * retains the old complete invalidation. Snapshots never retain node pointers.
 * shortcut: retain at most 16 proven disjoint mutations; use a larger history
 * only after a measured workload justifies its locked replay cost. */
static void region_cache_sync(uint64_t current) {
    uint64_t previous=region_cache_epoch;
    if(previous==current && !(current&1))return;
    if(!region_cache_count){region_cache_epoch=current;return;}
    if((previous|current)&1 || previous>current || (current-previous)/2>REGION_JOURNAL_CAPACITY)goto clear;
    unsigned steps=(unsigned)((current-previous)/2);
    for(unsigned i=1;i<=steps;++i){
        uint64_t epoch=previous+2*(uint64_t)i;
        region_change change=region_journal[(epoch>>1)&(REGION_JOURNAL_CAPACITY-1)];
        if(change.epoch!=epoch || !change.known || change.low>=change.high)goto clear;
        region_cache_invalidate(change.low,(size_t)(change.high-change.low));
    }
    region_cache_epoch=current;return;
clear:
    region_cache_count=0;region_cache_epoch=current;
}

void win64_region_add(uint64_t address, size_t bytes, int writable) {''')
    replace('win64_region_mutation_begin(); region_cache_invalidate(address,bytes);',
            'region_mutation_start(1,address,address+bytes); region_cache_invalidate(address,bytes);')
    replace('''void win64_region_remove(uint64_t address) {
    win64_region_mutation_begin();''', '''void win64_region_remove(uint64_t address) {
    region_mutation_start(1,0,0);''')
    replace('''        region_cache_invalidate(r->base,r->size);free(r);removed=1;''', '''        region_mutation_span(r->base,r->base+r->size);
        region_cache_invalidate(r->base,r->size);free(r);removed=1;''')
    return source


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'android/host/win64.c'
    if sha(source) != BASE_SHA:
        raise ValueError('Reviewed 72726 region baseline changed')
    text = source.read_text(encoding='utf-8')
    candidate = candidate_source(text)
    # The lock-free read path and all atomic operations/fences remain identical.
    start, end = text.index('#ifndef WIN64_REGION_PROFILE\n    uint64_t version='), text.index('    metadata_lock();\n    uint64_t current=', text.index('static int accessible('))
    if text[start:end] not in candidate:
        raise ValueError('Atomic double-epoch fast path changed')
    atomic_lines = lambda s: [line.strip() for line in s.splitlines() if 'atomic_' in line]
    if atomic_lines(text) != atomic_lines(candidate):
        raise ValueError('Atomic operations/fences changed')
    paths = [source, ROOT/'android/host/win64.h', ROOT/'android/host/win64_kernel.c',
             ROOT/'tests/region_journal_fixture.c', ROOT/'tests/region_journal_loader.c', ROOT/'tests/region_layout_fixture.c',
             ROOT/'tests/region_cache_byte_oracle.c', ROOT/'tests/android_region_tls_fixture.c',
             ROOT/'tests/region_pthread_win32.h', ROOT/'tests/region_atomic_host.h',
             ROOT/'android/dxvk/bridge.h', ROOT/'android/steam/bridge.h', Path(__file__).resolve()]
    paths=sorted(set(paths)|set((ROOT/'android/host').glob('*.h'))|set((ROOT/'native/runtime64').glob('*.h'))|
                 {ROOT/'generated/frostpunk-x64/recomp_dispatch.h',ROOT/'generated/win64/exports.h'},key=str)
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    cfg = json.loads((ROOT/'port.json').read_text())
    ndk = Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']
    clang = ndk/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    bins, env = windows_toolchain(); commands=[]

    def run(args, expected=0):
        command = list(map(str,args)); commands.append(command)
        p = subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=180)
        row = {'command':command,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
        if p.returncode != expected:
            (OUT/'failure.json').write_text(json.dumps(row,indent=2)+'\n')
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr)
        return row

    preamble='''#ifndef REGION_LAYOUT_RELEASE_BENCH
#define WIN64_REGION_CACHE_TEST 1
#endif
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
    tls=(ROOT/'tests/android_region_tls_fixture.c').read_text();tls=tls[tls.index('#include <time.h>'):]
    layout=(ROOT/'tests/region_layout_fixture.c').read_text()
    if layout.count('int main(int argc,char**argv){')!=1:raise ValueError('Existing fixture entry changed')
    layout=layout.replace('int main(int argc,char**argv){','int layout_fixture_main(int argc,char**argv){')
    results={}; arms={}
    for variant, body in [('baseline',text),('candidate',candidate)]:
        folder=OUT/variant; folder.mkdir(exist_ok=True)
        inc=folder/'host-include'; inc.mkdir(exist_ok=True)
        (inc/'stdatomic.h').write_bytes((ROOT/'tests/region_atomic_host.h').read_bytes())
        mirror=folder/'android/host/win64.c';mirror.parent.mkdir(parents=True,exist_ok=True);mirror.write_text(body,newline='\n')
        for name in ['android/dxvk/bridge.h','android/steam/bridge.h']:
            dest=folder/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(ROOT/name,dest)
        def block(a,b):return body[body.index(a):body.index(b)]
        extracted=preamble+block('typedef struct region {','static module modules[')
        extracted+=block('static pthread_mutex_t metadata_mutex;','static pthread_once_t pointer_cookie_once')
        extracted+=block('static _Noreturn void invalid(','/* The list/index is authoritative')
        extracted+=block('/* The list/index is authoritative','/* Optional one-run telemetry.')
        extracted+=block('/* Intrusive AVL address index:','static int host_protection(')
        extracted+=block('typedef struct allocation {','uint64_t win64_argument(')
        extracted+=block('static void region_cache_compare(','static void onexit_registration_self_test(')
        (folder/'production_region.inc').write_text(extracted,newline='\n');(folder/'tls_body.inc').write_text(tls,newline='\n')
        (folder/'layout_body.inc').write_text(layout,newline='\n')
        (folder/'win64.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),body.splitlines(True),fromfile='android/host/win64.c',tofile='android/host/win64.c')))
        defines=['-DREGION_LAYOUT_CANDIDATE=1']+(['-DREGION_JOURNAL_CANDIDATE=1']if variant=='candidate'else[])
        includes=[ROOT/'native/runtime64',ROOT/'generated/frostpunk-x64',ROOT/'generated/win64',ROOT/'android/host',ROOT/'tests',folder]
        host=[]
        for bench in [False,True]:
            name='journal-bench'if bench else'journal-proof';obj=folder/(name+'.obj');exe=folder/(name+'.exe')
            extra=['-DREGION_LAYOUT_RELEASE_BENCH=1']if bench else[]
            run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11',*defines,*extra,'-I'+str(inc),'-I'+str(folder),'-I'+str(ROOT/'tests'),'-Wno-deprecated-declarations','-c',ROOT/'tests/region_journal_fixture.c','-o',obj])
            run([bins/'link.exe','/nologo','/out:'+str(exe),obj,'libcmt.lib','oldnames.lib','kernel32.lib'])
            host.append({'release_benchmark':bench,'runs':[run([exe])]})
            arm=folder/(name+'-arm64')
            flags=['--target=aarch64-linux-android30','-std=c11','-O2','-fno-strict-aliasing','-fwrapv','-ffunction-sections','-fdata-sections',*defines,*extra,
                   '-DREGION_LAYOUT_SOURCE="'+mirror.as_posix()+'"',*['-I'+str(p)for p in includes]]
            run([clang,*flags,ROOT/'tests/region_journal_fixture.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',arm])
            arms[variant+'_'+name]={'path':str(arm.relative_to(ROOT)),'sha256':sha(arm),'device_executed':False}
            if bench:
                shared=folder/'libregion-journal-bench.so'
                run([clang,*flags,'-fPIC','-shared','-fvisibility=hidden','-DREGION_JOURNAL_SHARED=1',ROOT/'tests/region_journal_fixture.c',
                     '-Wl,--gc-sections','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',shared])
                elf=run([clang.parent/'llvm-readelf.exe','-r',shared])
                if 'R_AARCH64_TLSDESC'not in elf['stdout']:raise ValueError('Benchmark lost real dynamic TLS')
                arms[variant+'_shared_release_bench']={'path':str(shared.relative_to(ROOT)),'sha256':sha(shared),'real_dynamic_TLS':True,'device_executed':False}
        negative={arg:run([folder/'journal-proof.exe',arg],73)for arg in ['--unknown-owner','--bad-geometry','--epoch-overflow','--unbalanced-end']}
        mutants={}
        if variant=='candidate':
            mutations={
                'unknown_falsely_disjoint':('region_journal[((current+1)>>1)&(REGION_JOURNAL_CAPACITY-1)]=region_pending;',
                    'if(!region_pending.known)region_pending=(region_change){.epoch=current+1,.low=1,.high=2,.known=1};\n        region_journal[((current+1)>>1)&(REGION_JOURNAL_CAPACITY-1)]=region_pending;'),
                'remove_only_first_fragment':('if(!region_pending.known)return;','if(!region_pending.known || region_pending.low<region_pending.high)return;')}
            for name,(old,new)in mutations.items():
                if extracted.count(old)!=1:raise ValueError('Mutant anchor changed')
                mutant=folder/name;mutant.mkdir(exist_ok=True)
                (mutant/'production_region.inc').write_text(extracted.replace(old,new),newline='\n')
                for copied in ['tls_body.inc','layout_body.inc']:shutil.copyfile(folder/copied,mutant/copied)
                obj=mutant/'mutant.obj';exe=mutant/'mutant.exe'
                run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11',*defines,'-I'+str(inc),'-I'+str(mutant),'-I'+str(ROOT/'tests'),'-Wno-deprecated-declarations','-c',ROOT/'tests/region_journal_fixture.c','-o',obj])
                run([bins/'link.exe','/nologo','/out:'+str(exe),obj,'libcmt.lib','oldnames.lib','kernel32.lib'])
                p=subprocess.run([str(exe)],cwd=mutant,env=env,capture_output=True,text=True,timeout=180)
                mutants[name]={'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr,'source_sha256':sha(mutant/'production_region.inc')}
                (mutant/'result.json').write_text(json.dumps(mutants[name],indent=2)+'\n')
                if not p.returncode or not any(marker in p.stderr for marker in ['REGION_LAYOUT_FAIL','REGION_TLS_FAIL','REGION_BYTE_ORACLE_FAIL']):raise ValueError('Semantic mutant escaped: '+name)
        results[variant]={'host':host,'negative':negative,'semantic_mutants_rejected':mutants,'source_sha256':sha(mirror),'extracted_sha256':sha(folder/'production_region.inc')}
    loader=OUT/'region-journal-loader-arm64'
    run([clang,'--target=aarch64-linux-android30','-std=c11','-O2',ROOT/'tests/region_journal_loader.c','-Wl,-z,max-page-size=16384','-ldl','-o',loader])
    arms['shared_loader']={'path':str(loader.relative_to(ROOT)),'sha256':sha(loader),'device_executed':False}
    def rows(run):return [json.loads(line)for line in run['stdout'].splitlines()if line.startswith('{')]
    a,b=(rows(results[v]['host'][0]['runs'][0])for v in ['baseline','candidate'])
    def semantic(row):
        row=json.loads(json.dumps(row))
        for k in ['fast_ns','locked_oracle_ns','ns']:row.pop(k,None)
        for entry in row.get('whole_add_validate_remove',[]):entry.pop('ns')
        return row
    if len(a)!=8 or len(b)!=8:raise ValueError('Proof row coverage changed')
    if [semantic(x)for x in a[:3]]!=[semantic(x)for x in b[:3]]:raise ValueError('Existing byte/physical/race/parallel proof changed')
    if [semantic(x)for x in a[4:]]!=[semantic(x)for x in b[4:]]:raise ValueError('Cross-thread benchmark answers differ')
    if b[3]['candidate_policy_checks']<12 or not a[3]['success']or not b[3]['success']:raise ValueError('Journal coverage missing')
    if pins!={str(p.relative_to(ROOT)):sha(p)for p in paths}:raise ValueError('Production/proof inputs drifted')
    r={'success':True,'production_modified':False,'baseline_sha256':BASE_SHA,'source_pins':pins,'results':results,'ARM':arms,'commands':commands,
       'journal_capacity':16,'atomic_fastpath_and_operations_unchanged':True,'journal_only_under_metadata_mutex':True,
       'game_fps_proven':False,'performance_scope':'Synthetic real cross-thread mutation plus cached-reader cycles; host diagnostic only, ARM pending.'}
    path=OUT/'result.json';path.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({'success':True,'receipt':str(path),'sha256':sha(path),'ARM':arms},indent=2))


if __name__=='__main__':main()
