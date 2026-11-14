"""Isolated one-function virtual-mode reconciliation; no production/device writes."""
import difflib
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import pefile
from leaf_probe import ROOT, windows_toolchain
from test_android_render_scale import function

OUT=ROOT/'build/android-resolution-config-candidate'
EXE_SHA='45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc'


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    cfg=json.loads((ROOT/'port.json').read_text());exe=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(exe)!=EXE_SHA:raise ValueError('Original source changed')
    pe=pefile.PE(exe,fast_load=True)
    base=pe.OPTIONAL_HEADER.ImageBase
    gate=pe.get_data(0x140e5fa80-base,0x80)
    caller_gate=pe.get_data(0x140e66282-base,5)
    if len(gate)!=0x80:raise ValueError('Original mode-match extent missing')
    if len(caller_gate)!=5 or caller_gate[0]!=0xe8:raise ValueError('Original qualified direct call missing')
    header='''#pragma once
#include "cpu64.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int frostpunk_dxvk_get_display_mode(uint32_t*,uint32_t*,uint32_t*);
extern void *win64_pointer(uint64_t,size_t,int);
static const unsigned char resolution_config_gate[]={GATE_BYTES};
static const unsigned char resolution_config_caller_gate[]={CALLER_BYTES};
static _Noreturn void resolution_config_bad(const char *why){
    fprintf(stderr,"STOP Android resolution config: %s\\n",why);fflush(stderr);exit(73);
}
/* shortcut: one explicit Android virtual display policy overrides persisted
 * windowed dimensions; support independent windows when that profile exists. */
static void fp_android_resolution_config(CPU *c){
    int saved_errno=errno;uint32_t width=0,height=0,hz=0;
    if(memcmp((const void*)(uintptr_t)GVA(0x140e5fa80),resolution_config_gate,sizeof(resolution_config_gate)))resolution_config_bad("original bytes");
    if(!frostpunk_dxvk_get_display_mode(&width,&height,&hz)){errno=saved_errno;return;}
    uint64_t caller;memcpy(&caller,win64_pointer(c->rsp,8,0),8);
    /* Only the byte-proven mode-initialization call is qualified. The separate
     * RendererConfig vtable thunk retains its original behavior. */
    if(caller!=UINT64_C(0x140e66287)){errno=saved_errno;return;}
    if(memcmp((const void*)(uintptr_t)GVA(0x140e66282),resolution_config_caller_gate,sizeof(resolution_config_caller_gate)))resolution_config_bad("original caller bytes");
    if(!width||!height||!hz||width>16384||height>16384||hz>1000)resolution_config_bad("invalid virtual display policy");
    uint64_t config=c->rcx;
    if(!config||config>UINT64_MAX-0x54)resolution_config_bad("config geometry");
    const unsigned char *input=win64_pointer(config+0x48,12,0);
    uint64_t modes;int32_t count;memcpy(&modes,input,8);memcpy(&count,input+8,4);
    /* The supported profile has one mode; this bound only permits explicit
     * existing tables and never synthesizes an absent advertised mode. */
    if(count<=0||count>4096||!modes||modes>UINT64_MAX-(uint64_t)count*12)resolution_config_bad("mode vector geometry");
    const unsigned char *entries=win64_pointer(modes,(size_t)count*12,0);
    unsigned match=0;
    for(int32_t i=0;i<count;++i){uint32_t entry[3];memcpy(entry,entries+(size_t)i*12,12);
        if(entry[0]==width&&entry[1]==height&&entry[2]==hz){match=1;break;}}
    if(!match)resolution_config_bad("advertised mode absent from original vector");
    /* Stage every read before committing the exact three scalar fields. The
     * unchanged original function then chooses the first matching index. */
    unsigned char *output=win64_pointer(config+0x34,12,1);
    uint32_t value[3]={width,height,hz};memcpy(output,value,12);errno=saved_errno;
}
'''.replace('GATE_BYTES',','.join('0x%02x'%x for x in gate)).replace('CALLER_BYTES',','.join('0x%02x'%x for x in caller_gate))
    header_path=OUT/'resolution_config_candidate.h';header_path.write_text(header,newline='\n')
    source=ROOT/'generated/frostpunk-x64/recomp_funcs_0807.c';text=source.read_text()
    old=function(text,'140E5FA80')
    prefix='void L_000140E5FA80(CPU *c)\n{\n'
    if old.count(prefix)!=1:raise ValueError('Exact original function entry changed')
    changed=old.replace(prefix,prefix+'    fp_android_resolution_config(c);\n',1)
    if text.count(old)!=1:raise ValueError('Original function duplicate')
    overlay='#include "resolution_config_candidate.h"\n'+text.replace(old,changed,1)
    (OUT/source.name).write_text(overlay,newline='\n')
    (OUT/'source.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True),overlay.splitlines(True),fromfile=str(source.relative_to(ROOT)),tofile=str(source.relative_to(ROOT)))))
    source8=(ROOT/'generated/frostpunk-x64/recomp_funcs_0808.c').read_text()
    leaf=function(source8,'140E75610')
    funcs=old.replace('void L_000140E5FA80(','static void baseline_mode_match(',1)
    funcs+='\n'+changed.replace('void L_000140E5FA80(','static void candidate_mode_match(',1)
    funcs+='\n'+leaf.replace('void L_000140E75610(','static void mode_compare(',1)
    (OUT/'resolution_config_functions.inc').write_text(funcs,newline='\n')
    # Reuse the unchanged qualified UI fixture, adding one exact lifted callee.
    ui=(ROOT/'tests/android_render_scale_fixture.c').read_text()
    ui=ui.replace('#include "cpu64.h"','#include "cpu64.h"\nstatic void mode_compare(CPU*);',1)
    anchor='    switch(target){\n'
    if ui.count(anchor)!=1:raise ValueError('Qualified UI dispatch changed')
    ui=ui.replace(anchor,anchor+'    case UINT64_C(0x140e75610):mode_compare(c);return;\n')
    (OUT/'ui_body.inc').write_text(ui,newline='\n')
    for name in ['render_scale_candidate.h','render_scale_functions.inc','render_scale_dispatch.inc']:
        (OUT/name).write_bytes((ROOT/'build/render-scale-ui-candidate'/name).read_bytes())
    blocks=(ROOT/'build/render-scale-ui-candidate/image-blocks.bin').read_bytes()
    count=struct.unpack_from('<I',blocks)[0]
    blocks=struct.pack('<I',count+2)+blocks[4:]+struct.pack('<II',0xe5fa80,len(gate))+gate+struct.pack('<II',0xe66282,len(caller_gate))+caller_gate
    (OUT/'image-blocks.bin').write_bytes(blocks)
    paths=[Path(__file__).resolve(),ROOT/'tests/resolution_config_fixture.c',source,ROOT/'generated/frostpunk-x64/recomp_funcs_0808.c',
           ROOT/'generated/frostpunk-x64/recomp_meta.json',ROOT/'native/runtime64/cpu64.h',ROOT/'native/runtime64/sse_scalar.h',
           ROOT/'tools/test_android_render_scale.py',ROOT/'tests/android_render_scale_fixture.c',
           ROOT/'build/render-scale-ui-candidate/result.json',ROOT/'build/render-scale-ui-candidate/image-blocks.bin',
           *[ROOT/'build/render-scale-ui-candidate'/name for name in ['render_scale_candidate.h','render_scale_functions.inc','render_scale_dispatch.inc']],
           ROOT/'android/host/android_runtime.c',ROOT/'android/dxvk/bridge.cpp',ROOT/'android/dxvk/wsi_android.cpp',ROOT/'port.json']
    pins={str(p.relative_to(ROOT)):sha(p)for p in paths}
    bins,env=windows_toolchain();ndk=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin';clang=ndk/'clang.exe';commands=[]
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command);p=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        result={'exit':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
        if p.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps({'command':command,**result},indent=2)+'\n');raise RuntimeError(p.stdout+p.stderr)
        return result
    fixture=ROOT/'tests/resolution_config_fixture.c';includes=['-I'+str(ROOT/'native/runtime64'),'-I'+str(OUT)]
    obj=OUT/'resolution-config-host.obj';host=OUT/'resolution-config-host.exe'
    run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',*includes,'-c',fixture,'-o',obj])
    run([bins/'link.exe','/nologo','/out:'+str(host),obj,'libcmt.lib','oldnames.lib','kernel32.lib'])
    positive=run([host,OUT/'image-blocks.bin'])
    negatives={option:run([host,OUT/'image-blocks.bin',option],73)for option in ['--mode-absent','--count-zero','--count-negative','--count-large','--vector-null','--vector-short','--config-readonly','--bad-policy','--config-null','--config-overflow','--gate','--caller-gate']}
    arm=OUT/'resolution-config-arm64'
    run([clang,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',*includes,fixture,'-Wl,-z,max-page-size=16384','-ldl','-lm','-o',arm])
    if pins!={str(p.relative_to(ROOT)):sha(p)for p in paths}:raise ValueError('Inputs drifted during isolated build')
    generated={str(p.relative_to(ROOT)):sha(p)for p in OUT.iterdir()if p.is_file()and p.name not in ['result.json','failure.json']}
    r={'success':True,'production_modified':False,'device_actions':False,'original_exe_sha256':EXE_SHA,
       'one_function':'0x140e5fa80','qualified_caller_return':'0x140e66287','other_callers_original_unchanged':True,
       'original_extent_bytes':len(gate),'original_bytes':gate.hex(),'original_extent_sha256':hashlib.sha256(gate).hexdigest(),
       'qualified_callsite':{'va':'0x140e66282','bytes':caller_gate.hex(),'sha256':hashlib.sha256(caller_gate).hexdigest()},
       'source_pins':pins,'generated':generated,'host':positive,'negative':negatives,'commands':commands,
       'ARM':{'path':str(arm.relative_to(ROOT)),'sha256':sha(arm),'dataset':str((OUT/'image-blocks.bin').relative_to(ROOT)),'dataset_sha256':sha(OUT/'image-blocks.bin'),'device_executed':False},
       'scope':'Explicit virtual policy only, exact existing mode; original body unchanged, CPU oracle original with normalized input. Native UI/RT hardware proof pending. No FPS claim.'}
    path=OUT/'result.json';path.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps({'success':True,'path':str(path),'sha256':sha(path),'ARM':r['ARM']},indent=2))


if __name__=='__main__':main()
