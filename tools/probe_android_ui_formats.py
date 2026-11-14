"""Isolated exact four-format candidate. Does not modify the production provider or use ADB."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-ui-format-preflight'
PRODUCTION = ROOT / 'android/host/win64_wide_stdio.c'
PRODUCTION_SHA = '377400b21b1b55325e52a11eae1ab3c8eef0ce815c04372d004abd648665caa0'
CANDIDATE_SHA = '891c975a134b1861fad8e665e468d3b84a4e63b27b0d4c8bb9760f456da5a892'
ACCEPTED = ROOT / 'build/android-wide-color-test/vectors.bin'
ACCEPTED_SHA = '18d87f74e96925d4a6a9d1d1a8c98f4976193bc03e86fdca7cf6ab0226fc6aa8'
EXTRA_NEGATIVE = ['short-va-save', 'short-va-ellipsis', 'bad-join-format']


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def replace_one(source, old, new):
    if source.count(old) != 1:
        raise ValueError('Candidate preimage changed: ' + old[:100])
    return source.replace(old, new)


def candidate_source(source, reverse=False):
    changes=[('seven observed UTF16 path/color formats', 'eleven original UTF16 path/color/UI formats'),
             ('"|#color=%02x%02x%02x|"};', '"|#color=%02x%02x%02x|","%s.%s","%s - %s","Saved game %3d","%.*s..."};'),
             ('unsigned candidates=127;', 'unsigned candidates=2047;'),
             ('for(unsigned j=0;j<7;++j)', 'for(unsigned j=0;j<11;++j)'),
             ('size_t argument_bytes=kind==6?24:kind>=4?8:16;', 'size_t argument_bytes=kind==6?24:(kind==4||kind==5||kind==9)?8:16;')]
    helper = '''static piece precision_string(uint64_t address,int32_t precision) {
    if(precision<0)return argument_string(address);
    if(!address){piece result=argument_string(0);if(result.length>(uint32_t)precision)result.length=(uint32_t)precision;return result;}
    piece result={address,NULL,0};
    while(result.length<(uint32_t)precision&&unit(address,result.length))++result.length;
    return result;
}
static size_t decimal_width3(uint64_t bits,uint16_t output[12]) {
    uint32_t value=(uint32_t)bits;int negative=(value&UINT32_C(0x80000000))!=0;
    uint32_t magnitude=negative?UINT32_C(0)-value:value;uint16_t reverse[10];size_t digits=0,used=0;
    do{reverse[digits++]=(uint16_t)('0'+magnitude%10);magnitude/=10;}while(magnitude);
    for(size_t width=digits+(unsigned)negative;width<3;++width)output[used++]=' ';
    if(negative)output[used++]='-';
    while(digits)output[used++]=reverse[--digits];return used;
}
'''
    changes.append(('static int format_path(', helper + 'static int format_path('))
    branch = '''    if(kind>=7){
        static const uint16_t dot[]={'.'},dash[]={' ','-',' '},saved[]={'S','a','v','e','d',' ','g','a','m','e',' '},ellipsis[]={'.','.','.'};
        if(kind==9){parts[0]=(piece){0,saved,11};parts[1]=(piece){0,number,decimal_width3(arguments[0],number)};parts[2]=parts[3]=(piece){0,NULL,0};}
        else if(kind==10){parts[0]=precision_string(arguments[1],(int32_t)(uint32_t)arguments[0]);parts[1]=(piece){0,ellipsis,3};parts[2]=parts[3]=(piece){0,NULL,0};}
        else{parts[0]=argument_string(arguments[0]);parts[1]=kind==7?(piece){0,dot,1}:(piece){0,dash,3};parts[2]=argument_string(arguments[1]);parts[3]=(piece){0,NULL,0};}
    }
    else if(kind==6){'''
    changes.append(('    if(kind==6){', branch))
    for old,new in reversed(changes)if reverse else changes:
        source=replace_one(source,new,old)if reverse else replace_one(source,old,new)
    return source


EXTRA_POSITIVE = r'''
static _Atomic unsigned long guarded_positive_cases;
static void additional_positive_cases(void){
    uint16_t format[32],tail[2]={0xd83d,'B'},output[32];uint64_t va[2],stack[10];
    for(unsigned kind=0;kind<4;++kind)for(unsigned secure=0;secure<2;++secure){
        const char *text=kind==3?"Saved game %3d":"%.*s...";for(size_t i=0;i<=strlen(text);++i)format[i]=(unsigned char)text[i];
        va[0]=kind==3?UINT64_C(0xaabbccddfffffff9):UINT64_C(0xaabbccdd00000000)|(kind==0?2:kind==1?0:1);
        va[1]=kind==1?UINT64_MAX:(uintptr_t)tail;memset(stack,0xc7,sizeof(stack));memset(output,0x5a,sizeof(output));
        uint16_t wanted[20];unsigned length;
        if(kind==3){const uint16_t saved[]={'S','a','v','e','d',' ','g','a','m','e',' ',' ','-','7',0};memcpy(wanted,saved,sizeof(saved));length=14;}
        else{unsigned take=kind==0?2:kind==1?0:1;memcpy(wanted,tail,take*2);wanted[take]='.';wanted[take+1]='.';wanted[take+2]='.';wanted[take+3]=0;length=take+3;}
        CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=secure?4:6;c.rdx=secure?(uintptr_t)output:0;c.r8=secure?length+1:0;c.r9=secure?length:(uintptr_t)format;
        stack[5]=secure?(uintptr_t)format:0;stack[6]=secure?0:(uintptr_t)va;stack[7]=(uintptr_t)va;CPU expected=c;expected.rax=length;
        uint64_t saved_stack[10],saved_va[2];memcpy(saved_stack,stack,sizeof(stack));memcpy(saved_va,va,sizeof(va));
        win64_region_add((uintptr_t)format,(strlen(text)+1)*2,0);win64_region_add((uintptr_t)tail,4,0);win64_region_add((uintptr_t)va,kind==3?8:16,0);win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)output,sizeof(output),1);
        guest_errno=1927;errno=2719;native_fp fp=read_fp();int handled=win64_wide_stdio_call(&c,secure?"__stdio_common_vsnwprintf_s":"__stdio_common_vswprintf");native_fp after=read_fp();
        require(handled&&!memcmp(&c,&expected,sizeof(c)),"UI bounded-input fullCPU result");require(!memcmp(&fp,&after,sizeof(fp))&&guest_errno==1927&&errno==2719,"UI native FP/errno preservation");
        require(!memcmp(stack,saved_stack,sizeof(stack))&&!memcmp(va,saved_va,sizeof(va)),"UI stack and varargs immutable");
        require(!secure||(!memcmp(output,wanted,(length+1)*2)&&output[length+1]==0x5a5a),"UI precision limits/one-slot width3/canary");
        win64_region_remove((uintptr_t)output);win64_region_remove((uintptr_t)stack);win64_region_remove((uintptr_t)va);win64_region_remove((uintptr_t)tail);win64_region_remove((uintptr_t)format);atomic_fetch_add(&calls,1);atomic_fetch_add(&guarded_positive_cases,1);
    }
}
'''


def fixture_source(source):
    source = replace_one(source, 'static void *worker(', EXTRA_POSITIVE + '\nstatic void *worker(')
    source = replace_one(source, 'valid[i%n]', 'valid[(i*17+449)%n]')
    source = replace_one(source, 'win64_wide_stdio_self_test();\n', 'win64_wide_stdio_self_test();additional_positive_cases();\n')
    source = replace_one(source, '    uint64_t va[2]={(uintptr_t)left,(uintptr_t)right},stack[10]={0};', '''    if(!strcmp(name,"short-va-save")||!strcmp(name,"short-va-ellipsis")||!strcmp(name,"bad-join-format")){
        const char *text=!strcmp(name,"short-va-save")?"Saved game %3d":!strcmp(name,"short-va-ellipsis")?"%.*s...":"%s.%s";
        format_bytes=(strlen(text)+1)*2;for(size_t i=0;i<=strlen(text);++i)fmt[i]=(unsigned char)text[i];
    }
    uint64_t va[2]={(uintptr_t)left,(uintptr_t)right},stack[10]={0};''')
    source = replace_one(source, '    if(!strncmp(name,"short-va-color",14)){win64_region_remove', '''    if(!strcmp(name,"short-va-save")||!strcmp(name,"short-va-ellipsis")){win64_region_remove((uintptr_t)va);win64_region_add((uintptr_t)va,!strcmp(name,"short-va-save")?7:15,0);}
    else if(!strcmp(name,"bad-join-format"))fmt[2]=':';
    else if(!strncmp(name,"short-va-color",14)){win64_region_remove''')
    source = replace_one(source, '"bad-color-format"};', '"bad-color-format","short-va-save","short-va-ellipsis","bad-join-format"};')
    source = replace_one(source, r'\"checks\":%lu,', r'\"checks\":%lu,\"guarded_positive_cases\":%lu,')
    source = replace_one(source, 'atomic_load(&checks));free(data);', 'atomic_load(&checks),atomic_load(&guarded_positive_cases));free(data);')
    return source


def original_evidence(cfg, baseline):
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_IMM, X86_OP_REG, X86_OP_MEM, X86_REG_RDX, X86_REG_RIP
    exe=Path(cfg['game_directory'])/'Frostpunk.exe'
    if sha(exe)!=cfg['original_exe_sha256']:
        raise ValueError('Original PE SHA drift')
    pe=pefile.PE(str(exe),fast_load=True);base=pe.OPTIONAL_HEADER.ImageBase
    read=lambda va,size:pe.get_data(va-base,size)
    md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
    def span(va,size):
        raw=read(va,size)
        if len(raw)!=size:raise ValueError('Original span unavailable')
        return {'va':hex(va),'size':size,'bytes':raw.hex(),'sha256':hashlib.sha256(raw).hexdigest()}
    files=subprocess.check_output(['rg','-l','-F','dispatch(c, 0x140E38040ull)',str(ROOT/'generated/frostpunk-x64'),'-g','*.c'],text=True).splitlines()
    formats=re.search(r'static const char \*const formats\[\]=\{(.*?)\};',baseline).group(1)
    supported=re.findall(r'"([^"]*)"',formats);rows=[];seen=set();units={}
    for filename in files:
        path=Path(filename);lines=path.read_text().splitlines();function=''
        for i,line in enumerate(lines):
            if line.startswith('void L_'):function=line.split('(')[0][5:]
            if 'dispatch(c, 0x140E38040ull)'not in line or ': call 'not in line:continue
            call=int(re.search(r'/\* ([0-9A-F]+):',line).group(1),16)
            if call in seen:continue
            seen.add(call);before=lines[max(0,i-65):i];found=None
            for item in reversed(before):
                match=re.search(r'SET64\(c->rdx, GVA\(0x([0-9A-F]+)\)\).*?/\* ([0-9A-F]+):',item)
                if match:found=tuple(int(v,16)for v in match.groups());break
            if found is None:raise ValueError('Dynamic wide format requires separate audit')
            va,lea=found;insn=list(md.disasm(read(call,5),call));load=list(md.disasm(read(lea,7),lea))
            if len(insn)!=1 or insn[0].mnemonic!='call' or insn[0].operands[0].type!=X86_OP_IMM or insn[0].operands[0].imm!=0x140e38040:raise ValueError('Direct call byte gate changed')
            if len(load)!=1 or load[0].mnemonic!='lea' or load[0].operands[0].type!=X86_OP_REG or load[0].operands[0].reg!=X86_REG_RDX or load[0].operands[1].type!=X86_OP_MEM or load[0].operands[1].mem.base!=X86_REG_RIP or lea+7+load[0].operands[1].mem.disp!=va:raise ValueError('Constant-format LEA byte gate changed')
            raw=read(va,256);end=next(j for j in range(0,254,2)if raw[j:j+2]==b'\0\0');fmt=raw[:end].decode('utf-16le')
            units[str(path.relative_to(ROOT))]=sha(path)
            rows.append({'call':hex(call),'function':function,'format':fmt,'supported':fmt in supported,'literal':span(va,end+2),'lea':span(lea,7),'direct_call':span(call,5),'argument_tail':span(lea,call+5-lea)})
    if len(rows)!=19 or len({r['format']for r in rows})!=11:raise ValueError('Bounded direct-call census changed')
    missing={r['format']for r in rows if not r['supported']}
    if missing!={'%s.%s','%s - %s','Saved game %3d','%.*s...'}:raise ValueError('Unexpected missing-format scope')
    return {'original_sha256':cfg['original_exe_sha256'],'original_executed':False,'direct_original_calls':19,'constant_formats':11,'currently_supported_formats':supported,'rows':rows,'canonical_units':units,
            'scope_limit':'Only lifted direct E8 calls to 140E38040; excludes dynamic callers and other wide CRT wrappers. Nearest literal LEAs are byte-proven; no claim that every branch is reached in current gameplay.',
            'wrapper':span(0x140e38040,0xea),'percentage':{'function':'0x141c330d0','format_kind':'narrow','format_literals':[span(0x1421c7be0,10),span(0x1421c7bf0,9)],'format_strings':['%+.*lf%%','%.*lf%%'],'consumer':span(0x141c3328a,0x30),'formatter':'0x140e1eb20','covered_by_current_narrow_parser':True,'math_limit':'Default precision0 is accepted pow10^0. Nonzero signed16 precision is not claimed supported by current bounded pow profile.'},
            'roles':{'%s.%s':{'second_argument':span(0x141e0b9d0,10),'value':'save','classification':'save filename extension'},'Saved game %3d':{'source_literal':span(0x141e01e50,112),'classification':'FrostpunkGame save-game display name; caller forms counter modulo1000'},'%.*s...':{'literals':[span(0x141edef48,9),span(0x141edef88,14),span(0x141edefd8,14)],'classification':'save/load UI slot text elision'},'%s - %s':{'source_literal':span(0x14217bd50,130),'classification':'FrostpunkUIPlacesListControl place name row'}}}


def main():
    current=PRODUCTION.read_text();production_sha=sha(PRODUCTION)
    if production_sha==PRODUCTION_SHA:baseline=current
    elif production_sha==CANDIDATE_SHA:baseline=candidate_source(current,reverse=True)
    else:raise ValueError('Production is neither exact accepted7 nor promoted11 variant')
    # The approved7 source was LF; generated11 is CRLF. Pin both exact byte identities.
    encoded=lambda text:text.replace('\n','\r\n').encode()
    if hashlib.sha256(baseline.encode()).hexdigest()!=PRODUCTION_SHA or sha(ACCEPTED)!=ACCEPTED_SHA:raise ValueError('Frozen baseline/accepted proof drift')
    generated=candidate_source(baseline)
    if hashlib.sha256(encoded(generated)).hexdigest()!=CANDIDATE_SHA or production_sha==CANDIDATE_SHA and generated!=current:raise ValueError('Promoted whole-source candidate drift')
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();cfg=json.loads((ROOT/'port.json').read_text());commands=[]
    inputs=['android/host/win64_wide_stdio.c','android/host/win64_wide_stdio.h','android/host/win64.c','android/host/win64.h','android/host/win64_stdio.h','native/runtime64/cpu64.h','tests/wide_stdio_fixture.c','tests/wide_stdio_oracle.c','tests/ui_wide_formats_oracle.c','tests/environment_windows_host/pthread.h','tools/probe_android_ui_formats.py']
    pins={p:sha(ROOT/p)for p in inputs};evidence=original_evidence(cfg,baseline)
    def run(args,expected=0,marker=None):
        cmd=list(map(str,args));commands.append(cmd);r=subprocess.run(cmd,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if r.returncode!=expected or marker and marker not in r.stdout+r.stderr:raise RuntimeError(subprocess.list2cmdline(cmd)+'\nexit='+str(r.returncode)+'\n'+r.stdout+r.stderr)
        return {'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
    oracle=OUT/'ui-wide-oracle.exe';run([bins/'cl.exe','/nologo','/O2','/std:c11',ROOT/'tests/ui_wide_formats_oracle.c','/Fo'+str(OUT)+'/', '/Fe'+str(oracle)])
    dataset=OUT/'vectors.bin';native=json.loads(run([oracle,ACCEPTED,dataset])['stdout'].strip().splitlines()[-1]);raw=dataset.read_bytes();old=ACCEPTED.read_bytes()
    if native['vectors']!=1604 or native['formats']!=11 or raw[8:len(old)]!=old[8:]:raise ValueError('Accepted vector prefix or exact UI count changed')
    offset=8;invalid=[];valid=[];formats=[]
    for i in range(native['vectors']):
        h=struct.unpack_from('<IIQQIIiiIIII',raw,offset);offset+=56;fmt=raw[offset:offset+h[8]].decode('utf-16le','surrogatepass').rstrip('\0');formats.append(fmt)
        if h[5]:invalid.append((i,h[7]))
        else:valid.append(i)
        offset+=sum(h[8:12])
    if offset!=len(raw)or len(valid)!=native['valid']or len(invalid)!=native['invalid_callbacks']:raise ValueError('Dataset parse failed')
    parallel_formats={formats[valid[(i*17+449)%len(valid)]]for i in range(512)}
    if len(parallel_formats)!=11:raise ValueError('Concurrent schedule misses a format')
    candidate=OUT/'win64_wide_stdio_candidate.c';candidate.write_text(generated)
    fixture=fixture_source((ROOT/'tests/wide_stdio_fixture.c').read_text());fixture_path=OUT/'ui_wide_fixture.c';fixture_path.write_text(fixture)
    route='    if (crt && win64_wide_stdio_call(c,name)) { c->rsp+=8; return 1; }'
    if (ROOT/'android/host/win64.c').read_text().count(route)!=1:raise ValueError('Exact parent route changed')
    host=fixture[:fixture.index('static void *worker(')]
    for header in ('#include <sys/mman.h>\n','#include <sys/wait.h>\n','#include <unistd.h>\n'):host=host.replace(header,'')
    host=replace_one(host,'void win64_stdio_set_errno(int value){guest_errno=value;if(child_errno)*child_errno=value;}','void win64_stdio_set_errno(int value){guest_errno=value;fprintf(stderr,"WIDE_FIXTURE_GUEST_ERRNO=%d\\n",value);if(child_errno)*child_errno=value;}')
    host+='''
static int exact_import(CPU*c,int crt,const char*name){
'''+route+'''
    return 0;
}
int main(int argc,char**argv){
    require(argc==2||argc==4,"native vector path/negative");load_vectors(argv[1]);win64_region_add((uintptr_t)data,data_bytes,0);
    if(argc==4){if(!strcmp(argv[2],"--vector")){unsigned i=(unsigned)strtoul(argv[3],NULL,10);require(i<vector_count&&vectors[i].h.invalid_callback,"native invalid index");run_vector(&vectors[i],1);}else if(!strcmp(argv[2],"--negative"))extra_case(argv[3]);return99;}
    win64_wide_stdio_self_test();additional_positive_cases();unsigned valid=0;for(unsigned i=0;i<vector_count;++i)if(!vectors[i].h.invalid_callback){run_vector(&vectors[i],0);++valid;}
    uint16_t fmt[]={'S','a','v','e','d',' ','g','a','m','e',' ','%','3','d',0};uint64_t va=7,stack[9]={0};CPU c,before;
    win64_region_add((uintptr_t)fmt,sizeof(fmt),0);win64_region_add((uintptr_t)&va,8,0);win64_region_add((uintptr_t)stack,sizeof(stack),1);memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=6;c.rdx=c.r8=0;c.r9=(uintptr_t)fmt;stack[6]=(uintptr_t)&va;before=c;before.rax=14;before.rsp+=8;
    require(exact_import(&c,1,"__stdio_common_vswprintf")&&!memcmp(&c,&before,sizeof(c)),"UI actual parent route one pop");before=c;require(!exact_import(&c,0,"__stdio_common_vswprintf")&&!memcmp(&c,&before,sizeof(c)),"nonCRT namespace unchanged");
    require(atomic_load(&guarded_positive_cases)==8,"all guarded UI cases executed");
    printf("{\\"success\\":true,\\"native_valid_vectors\\":%u,\\"guarded_positive_cases\\":%lu,\\"fullCPU_nativeFP_errno_canaries\\":true,\\"single_parent_pop\\":true}\\n",valid,atomic_load(&guarded_positive_cases));return0;
}
'''
    host=host.replace('return99;','return 99;').replace('return0;','return 0;')
    host_path=OUT/'ui_wide_host.c';host_path.write_text(host);exe=OUT/'ui-wide-host.exe'
    run([bins/'cl.exe','/nologo','/O2','/std:c11','/experimental:c11atomics','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(ROOT/'tests/environment_windows_host'),host_path,candidate,'/Fo'+str(OUT)+'/', '/Fe'+str(exe)])
    host_result=json.loads(run([exe,dataset])['stdout'].strip().splitlines()[-1]);stops=[dict(vector=i,guest_errno=error,**run([exe,dataset,'--vector',i],73,'WIDE_FIXTURE_GUEST_ERRNO='+str(error)))for i,error in invalid]
    import test_android_wide_stdio
    negative=list(test_android_wide_stdio.NEGATIVE)+EXTRA_NEGATIVE
    negatives=[dict(name=name,**run([exe,dataset,'--negative',name],73,'STOP'))for name in negative]
    clang=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe';binary=OUT/'ui-wide-arm64'
    run([clang,'--target=aarch64-linux-android30','-std=c11','-O2','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),fixture_path,candidate,'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    if pins!={p:sha(ROOT/p)for p in inputs}or sha(ACCEPTED)!=ACCEPTED_SHA:raise ValueError('Frozen input drift during proof')
    report={'success':True,'candidate_only':production_sha==PRODUCTION_SHA,'production_modified':False,'sources':pins,'production_sha256':production_sha,'baseline_production_sha256':PRODUCTION_SHA,'promoted_whole_source_verified':production_sha==CANDIDATE_SHA,'candidate':str(candidate.relative_to(ROOT)),'candidate_sha256':sha(candidate),'accepted_dataset':str(ACCEPTED.relative_to(ROOT)),'accepted_dataset_sha256':ACCEPTED_SHA,'original_evidence':evidence,'native_UCRT':native,'host':host_result,'invalid_vector_stops':stops,'negative_stops':negatives,'commands':commands,'binary':str(binary.relative_to(ROOT)),'binary_sha256':sha(binary),'dataset':str(dataset.relative_to(ROOT)),'dataset_sha256':sha(dataset),'expected_ARM':{'native_valid_vectors':len(valid),'native_invalid_callback_stops':len(invalid),'bounds_unsupported_stops':len(negative),'parallel_workers':4,'parallel_calls':2048,'guarded_positive_cases':8,'total_returned_calls':len(valid)+2048+8},'parallel_formats':sorted(parallel_formats),'device_executed':False,'original_game_executed':False,'gameplay_proven':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k]for k in ['success','candidate_only','production_sha256','candidate_sha256','native_UCRT','host','binary','binary_sha256','dataset','dataset_sha256','expected_ARM']},indent=2));print('receipt_sha256='+sha(path))


if __name__=='__main__':main()
