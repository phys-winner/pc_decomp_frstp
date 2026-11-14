/* UCRT data oracle on Windows; the ARM CLI uses the real guest region registry. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include "win64.h"
#include "win64_utility.h"
#include <errno.h>
#include <stdio.h>
static _Thread_local int crt_errno;
static _Thread_local uint32_t guest_error;
typedef struct {uint64_t address;size_t bytes;int write;} fixture_region;
static _Thread_local fixture_region regions[128];
void win64_region_add(uint64_t address,size_t bytes,int write){for(unsigned i=0;i<128;++i)if(!regions[i].bytes){regions[i]=(fixture_region){address,bytes,write};return;}abort();}
void win64_region_remove(uint64_t address){for(unsigned i=0;i<128;++i)if(regions[i].address==address)regions[i].bytes=0;}
void *win64_pointer(uint64_t address,size_t bytes,int write){for(unsigned i=0;i<128;++i){fixture_region *r=&regions[i];if(r->bytes&&address>=r->address&&bytes<=r->bytes&&address-r->address<=r->bytes-bytes&&(!write||r->write))return(void*)(uintptr_t)address;}fputs("STOP qsort fixture pointer bounds\n",stderr);_Exit(73);}
uint64_t win64_argument(CPU *c,unsigned i){(void)c;(void)i;abort();}
const char *win64_string_a(uint64_t address){(void)address;abort();}
void win64_stdio_set_errno(int value){crt_errno=value;}
void win64_set_error(uint32_t value){guest_error=value;}
uint32_t win64_get_error(void){return guest_error;}
#else
#include "../android/host/win64.c"
#include "win64_utility.h"
static _Thread_local unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset+bytes>sizeof(fixture_teb))abort();memcpy(&value,fixture_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(fixture_teb))abort();memcpy(fixture_teb+offset,&value,bytes);}
#endif
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include "qsort_comparators.h"
typedef struct {uint32_t kind,count,pattern,seed;} specification;
typedef struct {uint32_t word[4];} sort_key;
typedef struct {specification spec;sort_key *keys;} test_case;
typedef struct {unsigned char *guard,*base,*objects,*original;size_t bytes,width;} array_data;
typedef struct context {CPU parent;array_data *array;specification spec;uint64_t calls;int effects,reenter,corrupt;struct context *previous;} context;
static _Thread_local context *active;
static _Thread_local uint64_t native_tls=UINT64_C(0x123456789abcdef0);
static test_case *cases;static uint32_t case_count;
static _Thread_local uint64_t comparisons;
static void need(int ok,const char *why){if(!ok){fprintf(stderr,"QSORT_FIXTURE_FAIL %s\n",why);exit(2);}}
static uint32_t random32(uint32_t *value){*value^=*value<<13;*value^=*value>>17;*value^=*value<<5;return *value;}
static uint32_t read32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t read64(const void *p){uint64_t v;memcpy(&v,p,8);return v;}
static void write32(void *p,uint32_t value){memcpy(p,&value,4);}
static size_t width_for(unsigned kind){return kind<2?4:kind==2?8:48;}
static array_data make_array(specification spec){
    array_data a={0};a.width=width_for(spec.kind);a.bytes=a.width*spec.count;
    a.guard=malloc(a.bytes+32);a.objects=calloc(spec.count?spec.count:1,520);a.original=malloc(a.bytes?a.bytes:1);
    need(a.guard&&a.objects&&a.original,"array allocation");memset(a.guard,0xa5,a.bytes+32);a.base=a.guard+16;
    uint32_t state=spec.seed;
    for(unsigned i=0;i<spec.count;++i){uint32_t v=random32(&state);
        switch(spec.pattern%7){case 1:v=i;break;case 2:v=spec.count-i;break;case 3:v&=7;break;case 4:v=0x80000000;break;case 5:v=i%17;break;case 6:if(i%3==0)v=UINT32_MAX;else if(i%3==1)v=0x80000000;break;}
        unsigned char *record=a.base+i*a.width,*object=a.objects+i*520;
        if(spec.kind<2)write32(record,v);
        else{uint64_t pointer=(uintptr_t)object;memcpy(record,&pointer,8);write32(object,v);
            if(spec.kind==3){float depth=(float)((int)(v%257)-128);memcpy(object+512,&depth,4);
                for(unsigned j=8;j<48;++j)record[j]=(unsigned char)(i*31+j*7);
                write32(record+8,i);write32(record+16,v%31);write32(record+24,(v>>8)%31);write32(record+32,(v>>16)%31);}
        }
    }
    memcpy(a.original,a.base,a.bytes);return a;
}
static sort_key key_for(const void *record,unsigned kind){sort_key key={{0}};
    if(kind<2)key.word[0]=read32(record);
    else if(kind==2)key.word[0]=read32((void *)(uintptr_t)read64(record));
    else {key.word[0]=read32((unsigned char *)(uintptr_t)read64(record)+512);key.word[1]=read32((const char *)record+16);key.word[2]=read32((const char *)record+24);key.word[3]=read32((const char *)record+32);}
    return key;
}
static int model_compare(const void *a,const void *b,unsigned kind){
    sort_key x=key_for(a,kind),y=key_for(b,kind);
    if(kind<3){if(kind==1){int32_t p=(int32_t)x.word[0],q=(int32_t)y.word[0];return(p>q)-(p<q);}return(x.word[0]>y.word[0])-(x.word[0]<y.word[0]);}
    float p,q;memcpy(&p,&x.word[0],4);memcpy(&q,&y.word[0],4);int32_t difference=(int32_t)(q-p);
    if(difference)return(difference>0)-(difference<0);
    for(unsigned i=1;i<4;++i){difference=(int32_t)(y.word[i]-x.word[i]);if(difference)return difference;}return 0;
}
static int one(const test_case *item,unsigned serial,int reenter,int effects);
void dispatch(CPU *cpu,uint64_t target){
    context *ctx=active;need(ctx!=NULL,"context present");uint64_t frame=(ctx->parent.rsp-48)&~UINT64_C(15);
    CPU expected=ctx->parent;expected.rcx=cpu->rcx;expected.rdx=cpu->rdx;expected.rsp=frame-8;
    need(!memcmp(cpu,&expected,sizeof(*cpu)),"full CPU callback entry copy");
    for(unsigned i=0;i<2;++i){uint64_t pointer=i?cpu->rdx:cpu->rcx;need(pointer>=(uintptr_t)ctx->array->base&&pointer-(uintptr_t)ctx->array->base<ctx->array->bytes&&(pointer-(uintptr_t)ctx->array->base)%ctx->array->width==0,"comparator record bounds/alignment");}
    need((cpu->rsp&15)==8&&read64(win64_pointer(cpu->rsp,8,0))==UINT64_C(0xe5ef00d8),"sentinel/alignment");
    memset(win64_pointer(cpu->rsp+8,32,1),0x4d,32);++ctx->calls;++comparisons;
    if(ctx->reenter&&ctx->calls==1){int ge=crt_errno,he=errno;uint32_t le=win64_get_error();uint64_t tls=native_tls;need(one(&cases[17],17,0,0),"reentrant qsort");native_tls=tls;crt_errno=ge;win64_set_error(le);errno=he;}
    if(ctx->spec.kind>=2){win64_pointer(read64((void *)(uintptr_t)cpu->rcx),ctx->spec.kind==3?516:4,0);win64_pointer(read64((void *)(uintptr_t)cpu->rdx),ctx->spec.kind==3?516:4,0);}
    int wanted=model_compare((void *)(uintptr_t)cpu->rcx,(void *)(uintptr_t)cpu->rdx,ctx->spec.kind);
    if(target==UINT64_C(0x141c561a0)&&ctx->spec.kind==2)L_000141C561A0(cpu);
    else if(target==UINT64_C(0x1410310d0)&&ctx->spec.kind==3)L_0001410310D0(cpu);
    else if(target==UINT64_C(0xe5e70018))cpu->rax=(uint32_t)wanted,cpu->rsp+=8;
    else{fputs("STOP qsort fixture unsupported procedure\n",stderr);_Exit(73);}
    need((int32_t)(uint32_t)cpu->rax==wanted,"exact lifted comparator versus C model");
    uint64_t rsp=cpu->rsp;memset(cpu,0x6d,sizeof(*cpu));cpu->rax=UINT64_C(0xfedcba9800000000)|(uint32_t)wanted;cpu->rsp=rsp;
    if(ctx->effects){crt_errno=2345;win64_set_error(1234);errno=3141;}
    if(ctx->corrupt==1)cpu->rsp+=8;else if(ctx->corrupt==2)memset(win64_pointer(frame-8,8,1),0,8);
    else if(ctx->corrupt==3)win64_region_remove((uintptr_t)ctx->array->guard);
    else if(ctx->corrupt==4)win64_region_remove((uintptr_t)ctx->array->objects);
}
static int one(const test_case *item,unsigned serial,int reenter,int effects){
    array_data array=make_array(item->spec);_Alignas(16) unsigned char stack[768];memset(stack,0x6b,sizeof(stack));
    win64_region_add((uintptr_t)array.guard,array.bytes+32,1);win64_region_add((uintptr_t)array.objects,(item->spec.count?item->spec.count:1)*520,1);win64_region_add((uintptr_t)stack,sizeof(stack),1);
    CPU cpu;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=(uintptr_t)array.base;cpu.rdx=item->spec.count;cpu.r8=array.width;cpu.r9=item->spec.kind==2?UINT64_C(0x141c561a0):item->spec.kind==3?UINT64_C(0x1410310d0):UINT64_C(0xe5e70018);cpu.rsp=(uintptr_t)(stack+600);cpu.mxcsr=0x1f80|((serial&3)<<13)|(serial&4?0x8040:0);CPU before=cpu;
    context ctx={0};ctx.parent=cpu;ctx.array=&array;ctx.spec=item->spec;ctx.effects=effects;ctx.reenter=reenter;ctx.previous=active;active=&ctx;
    native_tls=UINT64_C(0x123456789abcdef0)^serial;uint64_t tls=native_tls;crt_errno=1927;win64_set_error(0x5a17);errno=2718;
#ifdef _WIN32
    unsigned control=_mm_getcsr();
#else
    uint64_t control,status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
#endif
    need(win64_utility_call(&cpu,"qsort"),"qsort handled");need(!memcmp(&cpu,&before,sizeof(cpu)),"parent full CPU preserved/no pop");
    need(crt_errno==(effects&&ctx.calls?2345:1927)&&win64_get_error()==(effects&&ctx.calls?1234:0x5a17)&&errno==(effects&&ctx.calls?3141:2718),"guest/native errno LastError propagated");
    need(native_tls==tls,"native TLS identity");
#ifdef _WIN32
    need(_mm_getcsr()==control,"native MXCSR preserved");
#else
    uint64_t after_control,after_status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(after_control),"=r"(after_status));need(after_control==control&&after_status==status,"native FPCR/FPSR preserved");
#endif
    unsigned logarithm=0;for(unsigned n=item->spec.count;n;n>>=1)++logarithm;
    need(ctx.calls<=4*(uint64_t)item->spec.count*(logarithm+1),"O(nlogn) comparator bound");need(item->spec.count>=2||!ctx.calls,"0/1 no callbacks");
    unsigned char *seen=calloc(item->spec.count?item->spec.count:1,1);need(seen!=NULL,"permutation allocation");
    for(unsigned i=0;i<item->spec.count;++i){const unsigned char *record=array.base+i*array.width;sort_key key=key_for(record,item->spec.kind);need(!memcmp(&key,&item->keys[i],sizeof(key)),"native UCRT sorted keys");
        if(item->spec.kind>=2){uint64_t object=read64(record);need(object>=(uintptr_t)array.objects&&(object-(uintptr_t)array.objects)%520==0,"pointer payload preserved");uint64_t id=(object-(uintptr_t)array.objects)/520;need(id<item->spec.count&&!seen[id],"record permutation");seen[id]=1;need(!memcmp(record,array.original+id*array.width,array.width),"all record bytes travel together");}
    }
    free(seen);for(unsigned i=0;i<16;++i)need(array.guard[i]==0xa5&&array.base[array.bytes+i]==0xa5,"array canaries");
    uintptr_t frame=(before.rsp-48)&~(uintptr_t)15;for(unsigned i=0;i<sizeof(stack);++i){uintptr_t p=(uintptr_t)stack+i;if(ctx.calls&&p>=frame-8&&p<frame+32)continue;need(stack[i]==0x6b,"stack shadow canaries");}
    active=ctx.previous;win64_region_remove((uintptr_t)array.guard);win64_region_remove((uintptr_t)array.objects);win64_region_remove((uintptr_t)stack);free(array.guard);free(array.objects);free(array.original);return 1;
}
#ifdef _WIN32
typedef void (__cdecl *sort_fn)(void*,size_t,size_t,int(__cdecl*)(const void*,const void*));
static unsigned oracle_kind,invalid_calls;
static int __cdecl oracle_compare(const void *a,const void *b){return model_compare(a,b,oracle_kind);}
static void __cdecl invalid_parameter(const wchar_t*a,const wchar_t*b,const wchar_t*c,unsigned d,uintptr_t e){(void)a;(void)b;(void)c;(void)d;(void)e;++invalid_calls;}
static int oracle(const char *path){
    wchar_t system[MAX_PATH];UINT n=GetSystemDirectoryW(system,MAX_PATH);need(n&&n+14<MAX_PATH,"System32 path");wcscat_s(system,MAX_PATH,L"\\ucrtbase.dll");
    HMODULE module=LoadLibraryExW(system,NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);need(module!=NULL,"native UCRT load");wchar_t actual[MAX_PATH];need(GetModuleFileNameW(module,actual,MAX_PATH)&&!_wcsicmp(system,actual),"UCRT source verified");
    sort_fn sort=(sort_fn)GetProcAddress(module,"qsort");need(sort!=NULL,"native qsort");
    typedef void (__cdecl *invalid_fn)(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t);
    invalid_fn (__cdecl *set_invalid)(invalid_fn)=(invalid_fn(__cdecl*)(invalid_fn))GetProcAddress(module,"_set_thread_local_invalid_parameter_handler");need(set_invalid!=NULL,"native invalid handler");invalid_fn old=set_invalid(invalid_parameter);
    sort(NULL,0,4,oracle_compare);need(!invalid_calls,"NULL base count0 valid");
    sort(NULL,1,4,oracle_compare);need(invalid_calls==1,"NULL base count1 invalid");
    sort(NULL,0,0,oracle_compare);need(invalid_calls==2,"zero width invalid even count0");
    sort(NULL,0,4,NULL);need(invalid_calls==3,"NULL compare invalid even count0");set_invalid(old);
    static const unsigned sizes[]={0,1,2,3,7,16,31,64,127,257,512,1024};unsigned total=4*7*12;
    FILE *out=fopen(path,"wb");need(out!=NULL,"oracle output");need(fwrite("FPQS0001",1,8,out)==8&&fwrite(&total,4,1,out)==1,"oracle header");
    for(unsigned kind=0;kind<4;++kind)for(unsigned pattern=0;pattern<7;++pattern)for(unsigned k=0;k<12;++k){specification spec={kind,sizes[k],pattern,UINT32_C(0xcafebabe)^(kind*12345+pattern*997+k)};array_data a=make_array(spec);oracle_kind=kind;sort(a.base,spec.count,a.width,oracle_compare);need(fwrite(&spec,sizeof(spec),1,out)==1,"oracle spec");for(unsigned i=0;i<spec.count;++i){sort_key key=key_for(a.base+i*a.width,kind);need(fwrite(&key,sizeof(key),1,out)==1,"oracle keys");}free(a.guard);free(a.objects);free(a.original);}
    need(!fclose(out),"oracle close");printf("{\"native_oracle\":true,\"System32_UCRT_verified\":true,\"cases\":%u,\"invalid_boundary_cases\":4}\n",total);return 0;
}
static DWORD WINAPI worker(void *parameter){
#else
static void *worker(void *parameter){
#endif
    unsigned serial=(unsigned)(uintptr_t)parameter;for(unsigned i=0;i<48;++i)need(one(&cases[(serial*37+i*17)%case_count],i,0,i%2),"parallel sort");return 0;
}
static int negative(const char *name){
    specification spec={2,8,3,0x12345678};array_data a=make_array(spec);_Alignas(16) unsigned char stack[768];memset(stack,0x6b,sizeof(stack));CPU c={0};c.rcx=(uintptr_t)a.base;c.rdx=spec.count;c.r8=a.width;c.r9=UINT64_C(0x141c561a0);c.rsp=(uintptr_t)(stack+600);
    win64_region_add((uintptr_t)a.guard,a.bytes+32,1);win64_region_add((uintptr_t)a.objects,spec.count*520,1);win64_region_add((uintptr_t)stack,sizeof(stack),1);
    context ctx={0};ctx.array=&a;ctx.spec=spec;active=&ctx;
    if(!strcmp(name,"--zero-width"))c.r8=0;else if(!strcmp(name,"--null-base"))c.rcx=0;else if(!strcmp(name,"--null-base-one"))c.rcx=0,c.rdx=1;else if(!strcmp(name,"--zero-width-empty"))c.rcx=0,c.rdx=0,c.r8=0;else if(!strcmp(name,"--null-compare-empty"))c.rcx=0,c.rdx=0,c.r9=0;else if(!strcmp(name,"--null-compare"))c.r9=0;else if(!strcmp(name,"--span-overflow")){c.rdx=UINT64_MAX;c.r8=8;}else if(!strcmp(name,"--address-overflow"))c.rcx=UINT64_MAX-4;else if(!strcmp(name,"--array-bounds"))c.rdx=1024;else if(!strcmp(name,"--array-readonly")){win64_region_remove((uintptr_t)a.guard);win64_region_add((uintptr_t)a.guard,a.bytes+32,0);}else if(!strcmp(name,"--stack-underflow"))c.rsp=32;else if(!strcmp(name,"--frame-bounds")){win64_region_remove((uintptr_t)stack);win64_region_add(c.rsp,48,1);}else if(!strcmp(name,"--frame-readonly")){win64_region_remove((uintptr_t)stack);win64_region_add((uintptr_t)stack,sizeof(stack),0);}else if(!strcmp(name,"--bad-compare"))c.r9=0x12345678;else if(!strcmp(name,"--callback-pop"))ctx.corrupt=1;else if(!strcmp(name,"--callback-sentinel"))ctx.corrupt=2;else if(!strcmp(name,"--callback-free-array"))ctx.corrupt=3;else if(!strcmp(name,"--callback-free-pointee"))ctx.corrupt=4;else if(!strcmp(name,"--frame-overlap")){c.rcx=((c.rsp-48)&~UINT64_C(15))-8;c.rdx=4;c.r8=8;}else return 3;
    ctx.parent=c;win64_utility_call(&c,"qsort");return 99;
}
int main(int argc,char **argv){
#ifdef _WIN32
    if(argc==3&&!strcmp(argv[1],"--oracle"))return oracle(argv[2]);
#endif
    need(argc==2,"dataset or negative");if(argv[1][0]=='-')return negative(argv[1]);
    FILE *in=fopen(argv[1],"rb");need(in!=NULL,"input dataset");char magic[8];need(fread(magic,1,8,in)==8&&!memcmp(magic,"FPQS0001",8)&&fread(&case_count,4,1,in)==1&&case_count==336,"dataset header");cases=calloc(case_count,sizeof(*cases));need(cases!=NULL,"cases");
    for(unsigned i=0;i<case_count;++i){need(fread(&cases[i].spec,sizeof(specification),1,in)==1&&cases[i].spec.kind<4&&cases[i].spec.count<=1024,"case spec");cases[i].keys=calloc(cases[i].spec.count?cases[i].spec.count:1,sizeof(sort_key));need(cases[i].keys&&fread(cases[i].keys,sizeof(sort_key),cases[i].spec.count,in)==cases[i].spec.count,"expected keys");}need(fgetc(in)==EOF&&!fclose(in),"dataset exact end");
    CPU zero;memset(&zero,0xa5,sizeof(zero));zero.rcx=0;zero.rdx=0;zero.r8=4;zero.r9=0x12345678;zero.rsp=0;CPU before=zero;errno=2718;need(win64_utility_call(&zero,"qsort")&&!memcmp(&zero,&before,sizeof(zero))&&errno==2718,"zero count NULL base opaque compare no frame");
    uint32_t single=0x12345678;win64_region_add((uintptr_t)&single,sizeof(single),1);zero.rcx=(uintptr_t)&single;zero.rdx=1;before=zero;
    need(win64_utility_call(&zero,"qsort")&&!memcmp(&zero,&before,sizeof(zero))&&single==0x12345678,"one count opaque compare no frame");win64_region_remove((uintptr_t)&single);
#ifdef _WIN32
    unsigned initial_control=_mm_getcsr();_mm_setcsr(0x1f80|0x4000|0x8040|0x25);
#else
    uint64_t initial_control,initial_status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(initial_control),"=r"(initial_status));uint64_t seeded_control=(UINT64_C(1)<<24)|(UINT64_C(2)<<22),seeded_status=0x9f;__asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(seeded_control),"r"(seeded_status):"memory");
#endif
    for(unsigned i=0;i<case_count;++i)need(one(&cases[i],i,0,i%3==0),"oracle case");
    for(unsigned i=0;i<16;++i)need(one(&cases[47+i*13],i,1,1),"nested context");
#ifdef _WIN32
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)i,0,NULL);need(threads[i]!=NULL,"thread");}need(WaitForMultipleObjects(4,threads,TRUE,60000)==WAIT_OBJECT_0,"join");for(unsigned i=0;i<4;++i)need(CloseHandle(threads[i]),"thread close");
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)need(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i),"thread");for(unsigned i=0;i<4;++i)need(!pthread_join(threads[i],NULL),"join");
#endif
#ifdef _WIN32
    _mm_setcsr(initial_control);
#else
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(initial_control),"r"(initial_status):"memory");
#endif
    printf("{\"success\":true,\"native_oracle_cases\":336,\"native_comparator_models\":4,\"main_thread_comparisons\":%llu,\"parallel_calls\":192,\"reentrant_cases\":16,\"actual_AOT_Vorbis_pointer_comparator\":true,\"actual_AOT_48B_render_comparator\":true,\"full_CPU_preserved\":true,\"native_TLS_FP_errno\":true,\"heap_stack_canaries\":true,\"worst_case_comparison_bound\":\"4n(log2(n)+1)\",\"original_code_executed\":false}\n",(unsigned long long)comparisons);
    for(unsigned i=0;i<case_count;++i)free(cases[i].keys);free(cases);return 0;
}
