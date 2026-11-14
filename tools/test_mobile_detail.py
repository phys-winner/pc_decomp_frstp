"""Verify live mobile-detail defaults, renderer scope, CPU/stack and FP state on ARM64."""
import argparse,json,shlex,subprocess
from pathlib import Path
import fastbuild as fb
from mobile_detail_overlay import apply

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--serial',required=True);args=p.parse_args()
    out=fb.ROOT/'build/mobile-detail-test';out.mkdir(exist_ok=True)
    sources=[fb.ROOT/'android/host/mobile_detail.c',fb.ROOT/'android/host/mobile_detail.h',fb.ROOT/'tests/mobile_detail_fixture.c',Path(__file__).resolve(),fb.ROOT/'tools/mobile_detail_overlay.py']
    pins={str(s):fb.sha(s) for s in sources}
    unit=fb.ROOT/'build/arm64/generated-overlay/recomp_funcs_0822.c';text=unit.read_text()
    patched=apply(text);assert patched.count('unsigned fp_detail_mode=fp_test_detail_mode(c);')==1
    assert patched.count('fp_test_detail_threshold(')==1 and patched.count('uint8_t fp_detail_gate=rd8(')==1
    binary=out/'mobile-detail-arm64'
    def run(cmd):return subprocess.check_output(list(map(str,cmd)),text=True,stderr=subprocess.STDOUT,timeout=90)
    run([fb.CLANG,*fb.COMPILE_FLAGS,*['-I'+str(fb.ROOT/d) for d in fb.INCLUDES],sources[0],sources[2],'-Wl,-z,max-page-size=16384','-o',binary])
    adb=['adb','-s',args.serial];prop='debug.fp.detail';remote='/data/local/tmp/fp-mobile-detail-'+fb.sha(binary)[:12]
    assert not run([*adb,'shell','pidof com.frostpunk.staticrecomp || true']).strip()
    previous=run([*adb,'shell','getprop '+prop]).strip();results=[]
    try:
        run([*adb,'push',binary,remote]);run([*adb,'shell','chmod','700',remote])
        for value,expected in [('',-1),('0',0),('10',10),('20',20),('30',30),('40',40),('9999999999999999999999',0)]:
            run([*adb,'shell','setprop '+prop+' '+shlex.quote(value)])
            r=json.loads(run([*adb,'shell',remote,expected]));assert r['success'] and r['cases']==2304
            results.append(dict(property=value,**r))
    finally:
        run([*adb,'shell','setprop '+prop+' '+shlex.quote(previous)]);run([*adb,'shell','rm','-f',remote])
    assert pins=={s:fb.sha(Path(s)) for s in pins}
    receipt=dict(success=True,results=results,sources=pins,original_unit_sha256=fb.sha(unit),patched_unit_sha256=__import__('hashlib').sha256(patched.encode()).hexdigest(),binary_sha256=fb.sha(binary))
    (out/'result.json').write_text(json.dumps(receipt,indent=1)+'\n');print(json.dumps(dict(success=True,cases=sum(r['cases'] for r in results),live_mobile_default=True)))
if __name__=='__main__':main()
