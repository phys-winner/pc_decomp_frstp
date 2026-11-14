"""Run existing byte, race and history oracles against the current Android source."""
from pathlib import Path
import argparse,hashlib,json,subprocess
ROOT=Path(__file__).resolve().parents[1]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--serial');args=parser.parse_args()
    out=ROOT/'build/android-region-history-test';out.mkdir(exist_ok=True)
    source=ROOT/'android/host/win64.c';assert 'static void region_cache_sync(' in source.read_text()
    paths=[source,Path(__file__).resolve(),*[ROOT/'tests'/name for name in
        ['region_journal_fixture.c','region_layout_fixture.c','android_region_tls_fixture.c','region_cache_byte_oracle.c']]]
    pins={str(p.relative_to(ROOT)):sha(p) for p in paths}
    layout=paths[3].read_text();anchor='int main(int argc,char**argv){';assert layout.count(anchor)==1
    (out/'layout_body.inc').write_text(layout.replace(anchor,'int layout_fixture_main(int argc,char**argv){'))
    tls=paths[4].read_text();(out/'tls_body.inc').write_text(tls[tls.index('#include <time.h>'):])
    cfg=json.loads((ROOT/'port.json').read_text())
    clang=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary=out/'region-history-arm64'
    command=[str(clang),'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv',
        '-ffunction-sections','-fdata-sections','-DREGION_LAYOUT_CANDIDATE=1','-DREGION_JOURNAL_CANDIDATE=1',
        '-DREGION_LAYOUT_SOURCE="'+source.as_posix()+'"',*['-I'+str(ROOT/p) for p in
        ['native/runtime64','generated/frostpunk-x64','generated/win64','android/host','tests']],'-I'+str(out),
        str(paths[2]),'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',str(binary)]
    p=subprocess.run(command,capture_output=True,text=True,timeout=90);assert p.returncode==0,p.stderr
    result=dict(success=True,sources=pins,command=command,binary_sha256=sha(binary),device_executed=False)
    if args.serial:
        adb=['adb','-s',args.serial];remote='/data/local/tmp/frostpunk-history-'+sha(binary)[:12]
        for tail in [['push',str(binary),remote],['shell','chmod','700',remote]]:
            subprocess.run(adb+tail,check=True,capture_output=True,timeout=30)
        p=subprocess.run(adb+['shell',remote],capture_output=True,text=True,timeout=150)
        (out/'device.stdout').write_text(p.stdout);(out/'device.stderr').write_text(p.stderr)
        assert p.returncode==0,(p.stdout[-2000:],p.stderr[-2000:])
        rows=[json.loads(line) for line in p.stdout.splitlines() if line.startswith('{')]
        assert len(rows)==8 and rows[0]['legacy_queries']==5702400 and rows[3]['candidate_policy_checks']>=13
        result.update(device_executed=True,serial=args.serial,device=rows)
    assert pins=={str(p.relative_to(ROOT)):sha(p) for p in paths}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
