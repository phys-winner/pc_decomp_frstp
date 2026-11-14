"""Check the real host/USER32 input checkpoint, concurrency, stop and legacy providers."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from fastbuild import ROOT,CLANG,NDK,CONFIG

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--serial',required=True);args=parser.parse_args()
    out=ROOT/'build/android-input-sequence-test';out.mkdir(exist_ok=True)
    files=['tests/android_input_sequence_fixture.c','android/app/app.c','android/app/host_api.h','android/app/window_probe.h',
           'android/gpu/driver.h','android/host/win64_window.c','android/host/win64_window.h','tools/test_android_input_sequence.py']
    def sha(path):
        with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
    pins={p:sha(ROOT/p) for p in files}
    def run(command):
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=90)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        return r.stdout
    binary=out/'input-sequence-arm64'
    run([CLANG,'--target=aarch64-linux-android30','-std=c11','-O2','-ffunction-sections','-fdata-sections',
         '-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-DFP_ORIGINAL_EXE_SHA256="'+CONFIG['original_exe_sha256']+'"',
         '-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(NDK/'sources/android/native_app_glue'),
         ROOT/files[0],'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-landroid','-llog','-ldl','-lm','-o',binary])
    adb=['adb','-s',args.serial];assert not run([*adb,'shell','pidof com.frostpunk.staticrecomp || true']).strip()
    remote='/data/local/tmp/fp-input-sequence-'+sha(binary)[:12]
    assert run([*adb,'shell','test ! -e '+remote+' && echo absent']).strip()=='absent'
    try:
        run([*adb,'push',binary,remote]);run([*adb,'shell','chmod','700',remote]);result=json.loads(run([*adb,'shell',remote]))
        assert result['success'] and result['publication_cases']==195 and result['parallel_calls']==8000 and result['in_flight_publications']==65
    finally:run([*adb,'shell','rm','-f',remote])
    assert pins=={p:sha(ROOT/p) for p in files}
    (out/'result.json').write_text(json.dumps(dict(success=True,device=result,sources=pins,binary_sha256=sha(binary)),indent=1)+'\n')
    print(json.dumps(result))

if __name__=='__main__':main()
