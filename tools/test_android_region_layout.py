"""Isolated private region-node reorder. Production/core/generated stay unchanged."""
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-region-layout-candidate'


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'android/host/win64.c'
    if sha(source) != 'a8381b4ad5d729e24e891cd31fd01827590e39563c1f0b350d02c537bc3498c0':
        raise ValueError('Reviewed region baseline changed')
    text = source.read_text(encoding='utf-8')
    old = 'typedef struct region { uint64_t base,owner; size_t size; uint32_t protection,state; struct region *next,*previous,*owner_next,*left,*right; uint64_t max_end; unsigned height; } region;'
    new = 'typedef struct region { uint64_t base,owner; size_t size; struct region *left,*right; uint64_t max_end; uint32_t protection,state; unsigned height; struct region *owner_next,*next,*previous; } region;'
    if text.count(old) != 1:
        raise ValueError('Private node declaration changed')
    candidate = text.replace(old, new)
    paths = [source, ROOT / 'android/host/win64.h', ROOT / 'android/host/win64_kernel.c',
             ROOT / 'tests/region_layout_fixture.c', ROOT / 'tests/region_layout_loader.c', ROOT / 'tests/region_cache_byte_oracle.c',
             ROOT / 'tests/android_region_tls_fixture.c', ROOT / 'tests/region_pthread_win32.h',
             ROOT / 'tests/region_atomic_host.h', ROOT / 'android/dxvk/bridge.h', ROOT / 'android/steam/bridge.h',
             Path(__file__).resolve()]
    pins = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    cfg = json.loads((ROOT / 'port.json').read_text());ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version']
    clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    bins, env = windows_toolchain();commands=[]

    def run(args, expected=0):
        command=list(map(str,args));commands.append(command)
        p=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if p.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps({'command':command,'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr},indent=2))
            raise RuntimeError(subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr)
        return {'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr}

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
    results={};arms={}
    for variant, body in [('baseline',text),('candidate',candidate)]:
        folder=OUT/variant;folder.mkdir(exist_ok=True);inc=folder/'host-include';inc.mkdir(exist_ok=True)
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
        (folder/'win64.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),body.splitlines(True),fromfile='android/host/win64.c',tofile='android/host/win64.c')))
        defines=['-DREGION_LAYOUT_CANDIDATE=1']if variant=='candidate'else[]
        host=[]
        for bench in [False,True]:
            name='layout-bench'if bench else'layout-proof';obj=folder/(name+'.obj');exe=folder/(name+'.exe');extra=['-DREGION_LAYOUT_RELEASE_BENCH=1']if bench else[]
            run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11',*defines,*extra,'-I'+str(inc),'-I'+str(folder),'-I'+str(ROOT/'tests'),'-Wno-deprecated-declarations','-c',ROOT/'tests/region_layout_fixture.c','-o',obj])
            run([bins/'link.exe','/nologo','/out:'+str(exe),obj,'libcmt.lib','oldnames.lib','kernel32.lib'])
            runs=[run([exe]) for _ in range(3 if bench else 1)];host.append({'release_benchmark':bench,'runs':runs})
            arm=folder/(name+'-arm64')
            run([clang,'--target=aarch64-linux-android30','-std=c11','-O2','-fno-strict-aliasing','-fwrapv','-ffunction-sections','-fdata-sections',*defines,*extra,
                 '-DREGION_LAYOUT_SOURCE="'+mirror.as_posix()+'"',*['-I'+str(p)for p in [ROOT/'native/runtime64',ROOT/'generated/frostpunk-x64',ROOT/'generated/win64',ROOT/'android/host',ROOT/'tests',folder]],
                 ROOT/'tests/region_layout_fixture.c','-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',arm])
            arms[variant+'_'+name]={'path':str(arm.relative_to(ROOT)),'sha256':sha(arm),'arguments':[],'device_executed':False}
            if bench:
                shared=folder/'libregion-layout-bench.so'
                run([clang,'--target=aarch64-linux-android30','-std=c11','-O2','-fno-strict-aliasing','-fwrapv','-fPIC','-shared','-fvisibility=hidden','-ffunction-sections','-fdata-sections',*defines,*extra,'-DREGION_LAYOUT_SHARED=1',
                     '-DREGION_LAYOUT_SOURCE="'+mirror.as_posix()+'"',*['-I'+str(p)for p in [ROOT/'native/runtime64',ROOT/'generated/frostpunk-x64',ROOT/'generated/win64',ROOT/'android/host',ROOT/'tests',folder]],
                     ROOT/'tests/region_layout_fixture.c','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',shared])
                arms[variant+'_shared_release_bench']={'path':str(shared.relative_to(ROOT)),'sha256':sha(shared),'real_dynamic_TLS':True,'device_executed':False}
        neg={arg:run([folder/'layout-proof.exe',arg],73)for arg in ['--unknown-owner','--bad-geometry','--epoch-overflow']}
        results[variant]={'host':host,'negative':neg,'candidate_source_sha256':sha(mirror),'extracted_sha256':sha(folder/'production_region.inc')}
    loader=OUT/'region-layout-loader-arm64'
    run([clang,'--target=aarch64-linux-android30','-std=c11','-O2',ROOT/'tests/region_layout_loader.c','-Wl,-z,max-page-size=16384','-ldl','-o',loader])
    arms['shared_loader']={'path':str(loader.relative_to(ROOT)),'sha256':sha(loader),'arguments':['<absolute shared-library path>'],'device_executed':False}
    def semantic(row):
        v=json.loads(row['stdout'].splitlines()[-1]);v.pop('traversal_prefix_bytes')
        for x in v['whole_add_validate_remove']:x.pop('ns')
        return v
    if semantic(results['baseline']['host'][0]['runs'][0])!=semantic(results['candidate']['host'][0]['runs'][0]):raise ValueError('Semantic aggregate differs')
    if pins!={str(p.relative_to(ROOT)):sha(p)for p in paths}:raise ValueError('Production inputs drifted')
    r={'success':True,'production_modified':False,'only_candidate_change':'private typedef field ordering; size88 and owner+8 unchanged',
       'source_pins':pins,'results':results,'ARM':arms,'commands':commands,'real_Bionic_TLS_in_ARM':True,
       'game_fps_proven':False,'performance_scope':'Synthetic metadata Map-like add/full-validate/remove; host times diagnostic only, actual ARM pending.'}
    path=OUT/'result.json';path.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({'success':True,'receipt':str(path),'sha256':sha(path),'ARM':arms},indent=2))


if __name__=='__main__':main()
