"""Execute native/adpf.h against a controlled Android API, plus C/C++ Android compilation.

Windows host harness uses the installed NDK clang and Visual C++ SDK. No device needed.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
import re

ROOT = Path(__file__).resolve().parents[1]
NDK = Path(os.environ.get('ANDROID_NDK', Path.home() / 'AppData/Local/Android/Sdk/ndk/27.2.12479018'))
CLANG = NDK / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
HARNESS = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#define PORT_ADPF_TEST
#define PORT_ADPF_IMPLEMENTATION
#define CLOCK_MONOTONIC 1
#define SYS_gettid 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
typedef int pthread_once_t;
typedef int pthread_key_t;
#define PTHREAD_ONCE_INIT 0
static int opens, reports, closes, updates, lookups, keys;
static int no_api, no_manager, no_session, bad_report, bad_update, bad_key, bad_tls, no_library;
static int64_t rate=16666666, now=1000000000, actual, target;
static void *key_value;
static void (*destroy_key)(void *);
static int pthread_once(pthread_once_t *once,void (*fn)(void)) { if (!*once) { *once=1; fn(); } return 0; }
static int pthread_key_create(pthread_key_t *key,void (*fn)(void *)) { ++keys; destroy_key=fn; *key=1; return bad_key; }
static int pthread_setspecific(pthread_key_t key,const void *p) { (void)key; if(bad_tls && p)return 1; key_value=(void*)p; return 0; }
static long syscall(long n) { assert(n==SYS_gettid); return 99; }
static int fake_clock(int clock,struct timespec *t) { assert(clock==1);t->tv_sec=(time_t)(now/1000000000);t->tv_nsec=(long)(now%1000000000);errno=34;return 0; }
#define clock_gettime fake_clock
struct APerformanceHintManager {int unused;};
struct APerformanceHintSession {int open;};
static struct APerformanceHintManager manager;
static struct APerformanceHintSession session;
static struct APerformanceHintManager *get_manager(void) { return no_manager?NULL:&manager; }
static int64_t get_rate(struct APerformanceHintManager *m) {assert(m==&manager);return rate;}
static struct APerformanceHintSession *create(struct APerformanceHintManager *m,const int32_t *t,size_t n,int64_t ns) {
    assert(m==&manager&&n==1&&t[0]==99&&ns>0);++opens;target=ns;session.open=!no_session;return no_session?NULL:&session;
}
static int update(struct APerformanceHintSession *s,int64_t ns) {assert(s->open);++updates;target=ns;return bad_update;}
static int report(struct APerformanceHintSession *s,int64_t ns) {assert(s->open&&ns>0);++reports;actual=ns;errno=34;return bad_report;}
static void close_session(struct APerformanceHintSession *s) {assert(s->open);++closes;s->open=0;}
static void *dlopen(const char *name,int flags) {(void)flags;assert(!strcmp(name,"libandroid.so"));++lookups;return no_library?NULL:&manager;}
static void *dlsym(void *lib,const char *name) {
    assert(lib==&manager);
    if(!strcmp(name,"APerformanceHint_getManager"))return (void*)get_manager;
    if(!strcmp(name,"APerformanceHint_getPreferredUpdateRateNanos"))return (void*)get_rate;
    if(!strcmp(name,"APerformanceHint_createSession"))return no_api?NULL:(void*)create;
    if(!strcmp(name,"APerformanceHint_updateTargetWorkDuration"))return (void*)update;
    if(!strcmp(name,"APerformanceHint_reportActualWorkDuration"))return (void*)report;
    if(!strcmp(name,"APerformanceHint_closeSession"))return (void*)close_session;
    abort();
}
static void log_line(const char *format,...) {(void)format;}
#define PORT_ADPF_LOG log_line
#include "ADPF_HEADER"
static void begin(double fps) {errno=123;port_adpf_begin(fps);assert(errno==123);}
static void end(void) {errno=123;port_adpf_end();assert(errno==123);}
int main(int argc,char **argv) {
    assert(argc==2);const char *mode=argv[1];
    if(!strcmp(mode,"no-api"))no_api=1;
    if(!strcmp(mode,"no-library"))no_library=1;
    if(!strcmp(mode,"no-manager"))no_manager=1;
    if(!strcmp(mode,"no-hal"))rate=-1;
    if(!strcmp(mode,"no-session"))no_session=1;
    if(!strcmp(mode,"no-key"))bad_key=1;
    if(!strcmp(mode,"no-tls"))bad_tls=1;
    begin(60);end();
    if(!strcmp(mode,"off")){begin(60);now+=100;end();assert(!lookups&&!keys&&!opens&&!reports);return 0;}
    if(strcmp(mode,"on")) {begin(60);now+=100;end();assert(!reports);if(bad_tls)assert(closes==1);return 0;}
    assert(opens==1&&!reports&&target==12500000&&key_value==&session);
    begin(60);now+=7000000;end();assert(reports==1&&actual==7000000);end();assert(reports==1);
    begin(60);port_adpf_block(1,1);port_adpf_block(1,0);now+=1000000000;end();assert(reports==1);
    begin(60);port_adpf_block(2,1);end();assert(reports==1&&closes==1&&!key_value);
    begin(60);assert(opens==1);port_adpf_block(1,1);port_adpf_block(2,0);begin(60);assert(opens==1);
    port_adpf_block(1,0);begin(60);assert(opens==2);begin(120);assert(updates==1&&target==6250000);
    now+=2000000;end();assert(reports==2&&actual==2000000);
    bad_update=1;begin(30);assert(closes==2&&!key_value);begin(60);assert(opens==2);
    /* A clean owner can also fail a report; it closes once and never churns. */
    port_adpf_thread.failed=0;bad_update=0;begin(60);begin(60);now+=100;bad_report=1;end();
    assert(closes==3&&!key_value);begin(60);assert(opens==3);
    port_adpf_thread.failed=0;bad_report=0;begin(60);assert(key_value);destroy_key(key_value);assert(closes==4);
    return 0;
}
'''

def host_environment():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip())
    msvc = sorted((vs / 'VC/Tools/MSVC').iterdir())[-1]
    sdk = Path('C:/Program Files (x86)/Windows Kits/10')
    version = sorted((sdk / 'Lib').iterdir())[-1].name
    env = os.environ.copy()
    env['PATH'] = str(msvc / 'bin/Hostx64/x64') + os.pathsep + env['PATH']
    env['INCLUDE'] = ';'.join(str(p) for p in (msvc/'include',sdk/f'Include/{version}/ucrt',sdk/f'Include/{version}/shared',sdk/f'Include/{version}/um'))
    env['LIB'] = ';'.join(str(p) for p in (msvc/'lib/x64',sdk/f'Lib/{version}/ucrt/x64',sdk/f'Lib/{version}/um/x64'))
    return env

class AdpfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='port-adpf-')
        cls.folder = Path(cls.temp.name)
        cls.env = host_environment()
        source = cls.folder/'adpf.c'
        source.write_text(HARNESS.replace('ADPF_HEADER', (ROOT/'native/adpf.h').as_posix()),encoding='utf-8')
        cls.exe=cls.folder/'adpf.exe'
        obj=cls.folder/'adpf.obj'
        result=subprocess.run([str(CLANG.with_name('clang-cl.exe')), '/nologo','/std:c11','/c',str(source),'/Fo'+str(obj)],cwd=cls.folder,env=cls.env,capture_output=True,text=True)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        linker=Path(cls.env['PATH'].split(os.pathsep)[0])/'link.exe'
        result=subprocess.run([str(linker),'/nologo',str(obj),'/out:'+str(cls.exe)],cwd=cls.folder,env=cls.env,capture_output=True,text=True)
        if result.returncode: raise RuntimeError(result.stdout+result.stderr)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_off_prevents_api_lookup_and_session_creation(self):
        for value in (None,'0','false','invalid'):
            with self.subTest(value=value):
                env=self.env.copy()
                if value is None: env.pop('PORT_OPTION_ADPF',None)
                else: env['PORT_OPTION_ADPF']=value
                subprocess.run([str(self.exe),'off'],env=env,check=True,capture_output=True)

    def test_real_session_reports_lifecycle_target_and_errors(self):
        for value in ('1','true'):
            with self.subTest(value=value):
                subprocess.run([str(self.exe),'on'],env=dict(self.env,PORT_OPTION_ADPF=value),check=True,capture_output=True)

    def test_unsupported_devices_and_resource_failures(self):
        for scenario in ('no-api','no-library','no-manager','no-hal','no-session','no-key','no-tls'):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.exe),scenario],env=dict(self.env,PORT_OPTION_ADPF='1'),check=True,capture_output=True)

    def test_android_c_and_cpp_both_abis(self):
        for target in ('aarch64-linux-android26','armv7a-linux-androideabi26'):
            for language in ('c','c++'):
                with self.subTest(target=target,language=language):
                    source=self.folder/('android.c' if language=='c' else 'android.cpp')
                    source.write_text('#define PORT_ADPF_IMPLEMENTATION\n#include "'+(ROOT/'native/adpf.h').as_posix()+'"\n',encoding='utf-8')
                    result=subprocess.run([str(CLANG),'--target='+target,'-x',language,'-Wall','-Werror','-fsyntax-only',str(source)],text=True,capture_output=True)
                    self.assertEqual(result.returncode,0,result.stderr)

    def test_launcher_default_saved_off_and_environment_handoff(self):
        source=(ROOT/'android/src/port/launcher/Launch.java').read_text(encoding='utf-8')
        start=source.index('    static String option(Context context, Spec.Option option)')
        stop=source.index('    public boolean skipIntro()',start)
        methods=source[start:stop].replace('android.os.Process.is64Bit()', 'Process.is64Bit()')
        loop=re.search(r'        for \(Spec.Option o : spec.options\(distribution.id\)\)\s+set\("PORT_OPTION_"[^;]+;',source).group()
        java=r'''
import java.util.*;
class Context { static String ACTIVITY_SERVICE="activity"; Object getSystemService(String id){return null;} }
class ActivityManager { static class MemoryInfo{long totalMem;} void getMemoryInfo(MemoryInfo i){} }
class Process {static boolean is64Bit(){return true;}}
class Spec {static class Option{String key="adpf",value="1";boolean arm64Only;int defaultMinRamGiB;}
    Option setting=new Option(); List<Option> options(String id){return Arrays.asList(setting);}}
class State {static String CHOICES="choices",saved;static String get(Context c,String file,String key,String fallback){
    if(!key.equals("option.adpf"))throw new AssertionError(key);return saved==null?fallback:saved;}}
public class AdpfHandoff {
    Spec spec=new Spec();Context context=new Context();Map<String,String> env=new HashMap<>();
    static class Distribution {String id="game";} Distribution distribution=new Distribution();
    String option(String key){if(!key.equals("adpf"))throw new AssertionError(key);return option(context,spec.setting);}
    void set(String key,String value){env.put(key,value);}
    void environment(){LOOP}
    METHODS
    public static void main(String[] args){AdpfHandoff launch=new AdpfHandoff();
        for(String saved:new String[]{null,"0","false","1","true"}){State.saved=saved;launch.environment();
            String expected=saved==null?"1":saved;
            if(!expected.equals(launch.env.get("PORT_OPTION_ADPF")))throw new AssertionError(saved);
        }
    }
}
'''.replace('LOOP',loop).replace('METHODS',methods)
        path=self.folder/'AdpfHandoff.java';path.write_text(java,encoding='utf-8')
        subprocess.run(['javac','-encoding','UTF-8',str(path)],check=True,capture_output=True)
        subprocess.run(['java','-cp',str(self.folder),'AdpfHandoff'],check=True,capture_output=True)

if __name__=='__main__': unittest.main()
