"""Verify the exact Windows-qualified ShaderFamily body on host; build ARM proofs.

Only scratch fixtures are generated. No ADB, original game execution or vendor edits.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

import pefile
from leaf_probe import ROOT, windows_toolchain

OUT=ROOT/'build/android-shader-lookup-test'


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    source=ROOT/'android/host/shader_lookup_aot.c'
    qualified=ROOT/'native/windows/shader_lookup.c'
    template=ROOT/'tests/windows_shader_lookup_fixture.c'
    baseline=ROOT/'tests/windows_shader_lookup_baseline.inc'
    gates=ROOT/'tests/windows_shader_lookup_gates.h'
    proof_path=ROOT/'research/windows-shader-lookup.json'
    proof=json.loads(proof_path.read_text())
    if not proof['success'] or any(proof['source_sha256'].get(str(p))!=sha(p)
                                  for p in (qualified,template,baseline,gates)):
        raise ValueError('Qualified Windows source proof changed')
    config=json.loads((ROOT/'port.json').read_text())
    original=Path(config['game_directory'])/'Frostpunk.exe'
    if sha(original)!=config['original_exe_sha256']:raise ValueError('Original PE changed')
    # Require the entire scheduled body, including final CPU materialization,
    # to equal the previously qualified implementation, rather than a new algorithm.
    expected=qualified.read_text().split('static const unsigned char gate_0[]')[0]
    expected=expected.replace('#include "shader_lookup.h"','#include "shader_lookup_aot.h"')
    if source.read_text().strip()!=expected.strip():raise ValueError('Android body differs from qualified Windows body')
    pe=pefile.PE(str(original),fast_load=True);opcode=[]
    for n,(start,end) in enumerate(((0x1410596a9,0x141059750),(0x1410597e9,0x141059890))):
        raw=pe.get_data(start-pe.OPTIONAL_HEADER.ImageBase,end-start)
        for text,name in ((qualified.read_text(),'gate_%d'%n),(gates.read_text(),'expected_gate_%d'%n)):
            match=re.search(r'static const unsigned char '+name+r'\[\] = \{([^}]+)\};',text,re.S)
            if not match or bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',match[1]))!=raw:
                raise ValueError('Original ShaderFamily byte gate differs')
        opcode.append({'va':hex(start),'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()})
    pins=[source,qualified,template,baseline,gates,proof_path,ROOT/'android/host/shader_lookup_aot.h',
          Path(__file__).resolve(),*sorted((ROOT/'native/runtime64').glob('*.h'))]
    before={str(p.relative_to(ROOT)):sha(p) for p in pins}
    text=template.read_text().replace('#include "shader_lookup.h"','#include "shader_lookup_aot.h"')
    start=text.index('#include "windows_shader_lookup_gates.h"')
    end=text.index('\nint main(void)',start)
    text=text[:start]+text[end:]
    text=text.replace('    byte_gates();\n','')
    anchor='static uint32_t last_error,guest_errno;static uint64_t cases,guard_cases;'
    if text.count(anchor)!=1:raise ValueError('Fixture TLS anchor changed')
    text=text.replace(anchor,'''static _Thread_local uint32_t last_error,guest_errno;
static uint64_t cases,guard_cases;
#ifndef _WIN32
/* Android has no Win32 LastError API; retain a distinct per-thread cookie. */
static _Thread_local uint32_t native_thread_cookie;
#define SetLastError(value) (native_thread_cookie=(value))
#define GetLastError() (native_thread_cookie)
#endif
static uint64_t prefix_compares,prefix_zeros;
static int inject_errno=1;
int fixture_strncmp(const char *a,const char *b,size_t n) {
    int result=strncmp(a,b,n);++prefix_compares;if(inject_errno)errno=E2BIG;return result;
}
void *fixture_memchr(const void *p,int c,size_t n) {
    void *result=memchr(p,c,n);++prefix_zeros;if(inject_errno)errno=EILSEQ;return result;
}''')
    text=text.replace('static void benchmark(void)\n{','static void benchmark(void)\n{\n    inject_errno=0;')
    text=text.replace('    fprintf(stderr,"phase nonfinal pairs\\n");','    inject_errno=1;\n    fprintf(stderr,"phase nonfinal pairs\\n");')
    text=text.replace('    printf("{\\"success\\":true,',
                      '    CHECK(!FP_SHADER_ERRNO_INJECT || (prefix_compares && prefix_zeros));\n    printf("{\\"success\\":true,')
    fixture=OUT/'fixture.c';fixture.write_text(text,newline='\n')
    llvm=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    clang=llvm/'clang.exe';bins,env=windows_toolchain();commands=[]
    def run(args):
        command=list(map(str,args));commands.append(command)
        p=subprocess.run(command,env=env,cwd=OUT,capture_output=True,text=True,timeout=90)
        if p.returncode:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+p.stdout+p.stderr)
        return p.stdout
    includes=['-I'+str(ROOT/p) for p in ('native/runtime64','android/host','tests')]
    common=['-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',*includes]
    def objects(target,variant):
        injected=variant=='errno';result=[]
        for item in (fixture,source):
            obj=OUT/(target+'-'+variant+'-'+item.stem+'.o')
            defines=['-DFP_SHADER_ERRNO_INJECT='+str(int(injected))]
            if item==source and injected:defines+=['-Dstrncmp=fixture_strncmp','-Dmemchr=fixture_memchr']
            run([clang,'--target='+('x86_64-pc-windows-msvc' if target=='host' else 'aarch64-linux-android30'),
                 *common,'-D_CRT_SECURE_NO_WARNINGS','-Wno-deprecated-declarations',*defines,'-c',item,'-o',obj])
            result.append(obj)
        return result
    host={};arms={}
    for variant in ('native','errno'):
        exe=OUT/('shader-'+variant+'-host.exe')
        run([bins/'link.exe','/nologo','/OUT:'+str(exe),*objects('host',variant),'libcmt.lib','oldnames.lib','kernel32.lib'])
        output=run([exe]);(OUT/('host-'+variant+'.jsonl')).write_text(output)
        rows=[json.loads(line) for line in output.splitlines()]
        if not rows[-1].get('success') or rows[-1]['differential_cases']!=268646 or rows[-1]['guard_page_readonly_cases']!=560:
            raise ValueError('Incomplete shader differential fixture')
        host[variant]={'results':rows,'binary_sha256':sha(exe)}
        arm=OUT/('shader-lookup-'+variant+'-arm64')
        run([clang,'--target=aarch64-linux-android30',*objects('arm',variant),'-lm','-Wl,-z,max-page-size=16384','-o',arm])
        arms[variant]={'path':str(arm.relative_to(ROOT)),'sha256':sha(arm),'args':[],
                       'native_comparison_errno_clobbered_in_semantic_tests':variant=='errno'}
    if before!={str(p.relative_to(ROOT)):sha(p) for p in pins}:raise ValueError('Shader inputs changed during tests')
    report={'success':True,'qualified_body_exact':True,'original_opcode_gates':opcode,'source_sha256':before,
            'fixture_sha256':sha(fixture),'compiler_sha256':sha(clang),'host':host,'arm':arms,
            'ARM_executed':False,'device_actions':False,'original_game_executed':False,'commands':commands}
    receipt=OUT/'result.json';receipt.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'host_cases_each':268646,'host_guards_each':560,'arm':arms,
                      'receipt':str(receipt.relative_to(ROOT)),'receipt_sha256':sha(receipt)},indent=2))


if __name__=='__main__':main()
