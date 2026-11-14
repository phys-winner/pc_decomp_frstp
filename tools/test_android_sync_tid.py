"""Execute the real SRW/CV/INIT_ONCE implementation and TID/fork checks on ARM64."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--before',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--serial',required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    sync=ROOT/'android/host/win64_sync.c';old=(a.before/'android/host/win64_sync.c').read_text();new=sync.read_text()
    assert old.replace('return (pid_t)syscall(SYS_gettid);','return gettid();',1)==new
    paths=[sync,ROOT/'android/host/win64.c',ROOT/'tests/android_sync_tid_fixture.c',Path(__file__).resolve()]
    pins={str(p.relative_to(ROOT)):sha(p) for p in paths}
    cfg=json.loads((ROOT/'port.json').read_text());clang=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary=out/'sync-tid-arm64';command=[str(clang),'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',*['-I'+str(ROOT/n) for n in ['native/runtime64','android/host','generated/frostpunk-x64','generated/win64']],str(paths[2]),'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-ldl','-lm','-pthread','-o',str(binary)]
    r=subprocess.run(command,capture_output=True,text=True,timeout=90);assert r.returncode==0,r.stderr
    remote='/data/local/tmp/frostpunk-sync-tid-'+sha(binary)[:12];adb=['adb','-s',a.serial]
    for tail in [['push',str(binary),remote],['shell','chmod','700',remote]]:subprocess.run(adb+tail,capture_output=True,check=True,timeout=30)
    r=subprocess.run(adb+['shell',remote],capture_output=True,text=True,timeout=60);(out/'device.stderr').write_text(r.stderr);assert r.returncode==0,(r.stdout,r.stderr)
    row=json.loads(r.stdout);assert row['success'] and row['sync_self_test'] and row['fork']
    assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths}
    result=dict(success=True,device_executed=True,serial=a.serial,sources=pins,device=row,binary_sha256=sha(binary),command=command)
    (out/'qualification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(row))
if __name__=='__main__':main()
