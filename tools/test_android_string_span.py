"""Production string-span scanner: byte oracle, guards, races and a rejected mutant."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OLD='''const char *win64_string_a(uint64_t p) {
    for (size_t n=0;n<32768;++n) {
        if (!accessible(p+n,1,0)) invalid("unterminated guest string at 0x%" PRIx64,p);
        if (!*(const char *)(uintptr_t)(p+n)) return (const char *)(uintptr_t)p;
    }
    invalid("guest string exceeds 32767 bytes");
}'''
NEW='''const char *win64_string_a(uint64_t p) {
    size_t n=0;
    while (n<32768) {
        uint64_t address=p+n;
        if (!accessible(address,1,0)) invalid("unterminated guest string at 0x%" PRIx64,p);
        if (!*(const char *)(uintptr_t)address) return (const char *)(uintptr_t)p;
        size_t span=1;
        if (region_cache_count) {
            region_snapshot hint=region_cache[0];
            if (address>=hint.base && address-hint.base<hint.size) {
                span=hint.size-(size_t)(address-hint.base);
                if (span>32768-n) span=32768-n;
                /* Cached geometry is only a hint; prove the complete read anew. */
                if (span>1 && !accessible(address,span,0)) {
                    span=1;
                    if (!accessible(address,1,0)) invalid("unterminated guest string at 0x%" PRIx64,p);
                }
            }
        }
        if (span>1 && memchr((const void *)(uintptr_t)(address+1),0,span-1)) return (const char *)(uintptr_t)p;
        n+=span;
    }
    invalid("guest string exceeds 32767 bytes");
}'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--before',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--serial',required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);core=ROOT/'android/host/win64.c';fixture=ROOT/'tests/android_string_span_fixture.c'
    old=(a.before/'android/host/win64.c').read_text();new=core.read_text();assert old.count(OLD)==1 and old.replace(OLD,NEW)==new
    files=[core,fixture,Path(__file__).resolve()];pins={str(p.relative_to(ROOT)):sha(p) for p in files}
    marker='if (span>1 && !accessible(address,span,0)) {';assert new.count(marker)==1
    mutant=out/'bulk-gate-omitted.c';mutant.write_text(new.replace(marker,'if (0) {'))
    cfg=json.loads((ROOT/'port.json').read_text());clang=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    results={};adb=['adb','-s',a.serial]
    for variant in ['release','races','mutant']:
        binary=out/('string-span-'+variant+'-arm64');definitions=[] if variant=='release' else ['-DWIN64_REGION_CACHE_TEST=1']
        if variant=='mutant':definitions+=['-DSTRING_SPAN_SOURCE="'+mutant.as_posix()+'"']
        command=[str(clang),'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-ffunction-sections','-fdata-sections',*definitions,*['-I'+str(ROOT/n) for n in ['native/runtime64','generated/frostpunk-x64','generated/win64','android/host']],str(fixture),'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-ldl','-lm','-o',str(binary)]
        r=subprocess.run(command,capture_output=True,text=True,timeout=90);assert r.returncode==0,r.stderr
        remote='/data/local/tmp/frostpunk-string-span-'+sha(binary)[:12]
        for tail in [['push',str(binary),remote],['shell','chmod','700',remote]]:subprocess.run(adb+tail,capture_output=True,check=True,timeout=30)
        r=subprocess.run(adb+['shell',remote],capture_output=True,text=True,timeout=150)
        (out/(variant+'.stdout')).write_text(r.stdout);(out/(variant+'.stderr')).write_text(r.stderr)
        if variant=='mutant':
            assert r.returncode!=0 and 'SPAN_RACE variant=1 expected_exit=73 actual_exit=99' in r.stderr,(r.returncode,r.stdout,r.stderr)
            row=dict(rejected=True,reason='Skipping the authoritative bulk gate fails the forced mutation oracle',returncode=r.returncode)
        else:
            assert r.returncode==0,(r.stdout,r.stderr);row=json.loads(r.stdout);assert row['success'] and row['cases']==(16649 if variant=='release' else 16651)
        results[variant]=dict(result=row,binary_sha256=sha(binary),command=command)
    assert pins=={str(p.relative_to(ROOT)):sha(p) for p in files}
    result=dict(success=True,device_executed=True,serial=a.serial,sources=pins,device=results,baseline_core_sha256=sha(a.before/'android/host/win64.c'))
    (out/'qualification.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v['result'] for k,v in results.items()},indent=2))
if __name__=='__main__':main()
