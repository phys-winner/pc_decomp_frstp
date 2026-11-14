"""Isolated two-site current TID reuse; no production or device writes."""
from pathlib import Path
import difflib,hashlib,json,subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/android-critical-tid-candidate'
BASE='bfb4c0cffb1c1b026f98a90ea806b9408820eb9a893ab8535c23d55abd1c23c6'
CORE='72726db44adea6107ac44b6536dc15873847626434fe22ab018c74db686672be'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(ok,s):
    if not ok:raise ValueError(s)
def main():
    OUT.mkdir(exist_ok=True,parents=True)
    source=ROOT/'android/host/win64_threads.c';core=ROOT/'android/host/win64.c'
    need(sha(source)==BASE and sha(core)==CORE,'Frozen production changed')
    baseline=source.read_text();candidate=baseline
    hunks=[('cs->owner=(pid_t)syscall(SYS_gettid);','cs->owner=current?current->tid:(pid_t)syscall(SYS_gettid);'),
           ('cs->owner!=(pid_t)syscall(SYS_gettid)','cs->owner!=(current?current->tid:(pid_t)syscall(SYS_gettid))')]
    for old,new in hunks:need(candidate.count(old)==1,'Source anchor changed');candidate=candidate.replace(old,new,1)
    back=candidate
    for old,new in reversed(hunks):back=back.replace(new,old,1)
    need(back==baseline,'Changes exceed the two owner TID reads')
    cfg=json.loads((ROOT/'port.json').read_text());ndk=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    clang=ndk/'clang.exe';bins,env=windows_toolchain();commands=[]
    paths=[source,core,ROOT/'android/host/win64_kernel.c',ROOT/'android/host/win64_sync.c',
           ROOT/'tests/thread_name_fixture.c',ROOT/'tests/critical_tid_host_fixture.c',ROOT/'tests/critical_tid_full_fixture.c',
           ROOT/'tests/critical_tid_loader.c',ROOT/'tests/region_pthread_win32.h',ROOT/'tests/region_atomic_host.h',Path(__file__)]
    for folder in ('android/host','native/runtime64','generated/win64','generated/frostpunk-x64'):paths.extend((ROOT/folder).glob('*.h'))
    pins={p.relative_to(ROOT).as_posix():sha(p)for p in paths}
    def run(args,expected=0,timeout=120):
        command=list(map(str,args));commands.append(command);r=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=timeout)
        value=dict(command=command,exit=r.returncode,stdout=r.stdout,stderr=r.stderr)
        if r.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps(value,indent=2)+'\n');raise RuntimeError(r.stdout[-4000:]+r.stderr[-7000:])
        return value
    for label,text in [('baseline',baseline),('candidate',candidate)]:
        folder=OUT/label;folder.mkdir(exist_ok=True)
        (folder/'win64_threads.c').write_text(text,newline='\n');(folder/'win64.c').write_bytes(core.read_bytes())
        body=text[text.index('static critical_state *critical_lookup('):text.index('/* Android profile shortcut:')]
        for name in ('critical_lookup','critical_fields','critical_initialize','critical_enter','critical_leave'):body=body.replace(name,label+'_'+name)
        (OUT/('critical-'+label+'.inc')).write_text(body,newline='\n')
        full=(ROOT/'tests/thread_name_fixture.c').read_text()
        full=full.replace('#include "../android/host/win64.c"','#include "'+(folder/'win64.c').as_posix()+'"',1)
        full=full.replace('#include "../android/host/win64_threads.c"','#include "'+(folder/'win64_threads.c').as_posix()+'"',1)
        (folder/'thread-provider-fixture.inc').write_text(full,newline='\n')
    (OUT/'candidate.patch').write_text(''.join(difflib.unified_diff(baseline.splitlines(True),candidate.splitlines(True),fromfile='android/host/win64_threads.c',tofile='isolated candidate')),newline='\n')
    host=OUT/'critical-tid-host.exe';obj=OUT/'critical-tid-host.obj'
    run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'tests'),'-I'+str(OUT),'-c',ROOT/'tests/critical_tid_host_fixture.c','-o',obj])
    run([bins/'link.exe','/nologo','/out:'+str(host),obj,'libcmt.lib','oldnames.lib','kernel32.lib'])
    host_result=run([host]);host_json=json.loads(host_result['stdout']);need(host_json['success']and host_json['paired_cases']==1280,'Host paired count differs')
    negatives=[]
    for label in ('baseline','candidate'):
        for mode in ('unknown','unowned','foreign'):
            r=run([host,'--'+label+'-'+mode],73);need('STOP Win64 thread'in r['stderr'],'Wrong typed negative');negatives.append(r)
    includes=['-I'+str(ROOT/p)for p in ('generated/win64','generated/frostpunk-x64','native/runtime64','android/host')]
    adapter=OUT/'adapter.c';adapter.write_text('#include <stdint.h>\nint64_t g_image_delta;\n',newline='\n')
    artifacts={}
    for label in ('baseline','candidate'):
        folder=OUT/label;binary=folder/'critical-tid-arm64'
        common=[clang,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-ffunction-sections','-fdata-sections',*includes,'-I'+str(folder)]
        sources=[ROOT/'tests/critical_tid_full_fixture.c',ROOT/'android/host/win64_kernel.c',ROOT/'android/host/win64_sync.c',adapter]
        libs=['-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm']
        run([*common,*sources,*libs,'-o',binary]);artifacts[label+'_proof']=dict(path=binary.relative_to(ROOT).as_posix(),sha256=sha(binary),arguments=[])
        dso=folder/'libcritical-tid-bench.so'
        run([*common,'-fPIC','-shared','-fvisibility=hidden','-DCRITICAL_TID_SHARED=1',*sources,*libs,'-Wl,--no-undefined','-o',dso])
        reloc=run([ndk/'llvm-readelf.exe','-r',dso]);need('R_AARCH64_TLSDESC'in reloc['stdout'],'No actual dynamic TLS in DSO')
        (folder/'tls-relocations.txt').write_text(reloc['stdout'],newline='\n');artifacts[label+'_shared_benchmark']=dict(path=dso.relative_to(ROOT).as_posix(),sha256=sha(dso),dynamic_TLS=True)
    loader=OUT/'critical-tid-loader-arm64';run([clang,'--target=aarch64-linux-android30','-O2',ROOT/'tests/critical_tid_loader.c','-ldl','-Wl,-z,max-page-size=16384','-o',loader])
    artifacts['shared_loader']=dict(path=loader.relative_to(ROOT).as_posix(),sha256=sha(loader),arguments=['<absolute DSO path>'])
    need(pins=={name:sha(ROOT/name)for name in pins},'Production/dependencies changed during proof')
    report=dict(success=True,production_modified=False,device_actions=False,baseline_sha256=BASE,candidate_sha256=sha(OUT/'candidate/win64_threads.c'),
        only_changes=hunks,exact_baseline_reconstruction=True,pins=pins,host=host_json,host_run=host_result,host_negative=negatives,
        ARM=artifacts,ARM_execution_pending=True,ARM_expected_new_row=dict(success=True,whole_recursive_pairs=224,adopt_finish_fallback=True,native_start_detach=True,fullCPU_FP_errno_LastError=True,original_thread_selftest_and_name_regression=True),
        ARM_legacy_negatives=['dead-tid','unknown-tid','invalid-utf8','too-long','count','raise-flags','type','info-flags','invalid-record','short-six'],
        ARM_new_negatives=['tid-unknown','tid-unowned','tid-foreign','tid-busy-delete'],commands=commands,
        scope='Only existing current->tid is reused at two critical owner reads; NULL current retains original SYS_gettid. Full providers used for ARM; no lazy adoption and no generic API change.',game_fps_proven=False)
    report['generated_pins']={p.relative_to(ROOT).as_posix():sha(p)for p in OUT.rglob('*')if p.is_file()and p.suffix in('.c','.inc','.patch','.txt')}
    (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(success=True,receipt_sha256=sha(OUT/'result.json'),host=host_json,ARM=artifacts),indent=2))
if __name__=='__main__':main()
