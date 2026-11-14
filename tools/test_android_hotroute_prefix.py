"""Verify the sole router edit against the sealed predicate on the target device."""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OLD='''    if (crt && (!strcmp(name,"memcpy") || !strcmp(name,"memmove") ||
                !strcmp(name,"strcat_s") || !strcmp(name,"strcpy_s") || !strcmp(name,"strncmp"))) {'''
NEW='''    if (crt && ((name[0]=='m' && (!strcmp(name,"memcpy") || !strcmp(name,"memmove"))) ||
                (name[0]=='s' && (!strcmp(name,"strcat_s") || !strcmp(name,"strcpy_s") || !strcmp(name,"strncmp"))))) {'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--before',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--serial',required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(exist_ok=True,parents=True);core=ROOT/'android/host/win64.c'
    old=(a.before/'android/host/win64.c').read_text();assert old.count(OLD)==1 and old.replace(OLD,NEW)==core.read_text()
    pins={str(p.relative_to(ROOT)):sha(p) for p in [core,Path(__file__).resolve()]}
    def function(text,name):return 'static int '+name+'(int crt,const char*name){return '+text[len('    if ('):-len(') {')]+';}\n'
    code='#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <stdint.h>\n'+function(OLD,'baseline')+function(NEW,'candidate')+r'''
static unsigned checks;static void check(const char*n){for(int crt=0;crt<2;++crt){if(baseline(crt,n)!=candidate(crt,n))abort();++checks;}}
int main(void){const char*names[]={"memcpy","memmove","strcat_s","strcpy_s","strncmp","powf","strcmp","rand","srand","QueryPerformanceCounter","","_time64","_errno"};char n[128];
for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i){check(names[i]);size_t len=strlen(names[i]);for(size_t pos=0;pos<=len;++pos)for(unsigned byte=0;byte<256;++byte){strcpy(n,names[i]);n[pos]=(char)byte;n[len+1]=0;check(n);}}
uint32_t r=193743;for(unsigned i=0;i<200000;++i){unsigned len=i%127;for(unsigned j=0;j<len;++j){r=r*1664525+1013904223;n[j]=(char)(r>>24);}n[len]=0;check(n);}
printf("{\"success\":true,\"predicate_pairs\":%u}\n",checks);}
'''
    source=out/'hotroute-prefix.c';source.write_text(code);cfg=json.loads((ROOT/'port.json').read_text());clang=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary=out/'hotroute-prefix-arm64';r=subprocess.run([str(clang),'--target=aarch64-linux-android30','-O2',str(source),'-o',str(binary)],capture_output=True,text=True,timeout=60);assert r.returncode==0,r.stderr
    remote='/data/local/tmp/frostpunk-hotroute-'+sha(binary)[:12];adb=['adb','-s',a.serial]
    for tail in [['push',str(binary),remote],['shell','chmod','700',remote]]:subprocess.run(adb+tail,capture_output=True,check=True,timeout=30)
    r=subprocess.run(adb+['shell',remote],capture_output=True,text=True,timeout=30);assert r.returncode==0,(r.stdout,r.stderr);row=json.loads(r.stdout);assert row['success'] and row['predicate_pairs']>400000
    assert pins=={str(p.relative_to(ROOT)):sha(p) for p in [core,Path(__file__).resolve()]}
    (out/'qualification.json').write_text(json.dumps(dict(success=True,device_executed=True,serial=a.serial,sources=pins,device=row,binary_sha256=sha(binary)),indent=2)+'\n');print(row)
if __name__=='__main__':main()
