/* Exact canonical C versus the single static adapter, including real faults.
 * Original x64 instructions are data only and are never executed. */
#define _GNU_SOURCE
#include "string_resize_aot.h"
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <immintrin.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif
#include "canonical_body.inc"

int64_t g_image_delta;
static unsigned char *image,*arena,*guard_mapping,*guard_end,*cpu_mapping;
static size_t page_size;
static const size_t image_size=0x3000000,arena_size=0x10000;
_Alignas(4096) static unsigned char stack[1024];
static CPU *cpu_pointer;
#define cpu (*cpu_pointer)
static uint64_t digest=UINT64_C(1469598103934665603);
static unsigned cases,aliases,guards,calls,case_number,trace_count;
static uint64_t trace[128];
static uint64_t active_object;
static int assert_path;
static FILE *trace_file;
static void check(int yes,const char *why){if(!yes){fprintf(stderr,"STRING_RESIZE_FAIL %s case=%u\n",why,case_number);exit(1);}}
static uint64_t normalize(uint64_t value){
    if(value>=(uintptr_t)arena&&value-(uintptr_t)arena<arena_size)return UINT64_C(0x1000000000)+value-(uintptr_t)arena;
    if(value>=(uintptr_t)stack&&value-(uintptr_t)stack<sizeof(stack))return UINT64_C(0x2000000000)+value-(uintptr_t)stack;
    if(value>=(uintptr_t)cpu_mapping&&value-(uintptr_t)cpu_mapping<0x10000)return UINT64_C(0x3000000000)+value-(uintptr_t)&cpu;
    if(value>=(uintptr_t)image&&value-(uintptr_t)image<image_size)return UINT64_C(0x4000000000)+value-(uintptr_t)image;
    if(value>=(uintptr_t)guard_mapping&&value-(uintptr_t)guard_mapping<3*page_size)
        return UINT64_C(0x5000001000)+(value-(uintptr_t)guard_end);
    return value;
}
static void hash64(uint64_t v){for(unsigned i=0;i<8;++i){digest^=(unsigned char)(v>>(i*8));digest*=UINT64_C(1099511628211);}}
static void hash_words(const void*p,size_t n){const unsigned char*b=p;for(size_t i=0;i<n;i+=8){uint64_t v=0;memcpy(&v,b+i,n-i<8?n-i:8);hash64(normalize(v));}}
static void trace_words(const void*p,size_t n){
    const unsigned char*b=p;fputc('[',trace_file);
    for(size_t i=0;i<n;i+=8){uint64_t v=0;memcpy(&v,b+i,n-i<8?n-i:8);if(i)fputc(',',trace_file);fprintf(trace_file,"\"%016" PRIx64 "\"",normalize(v));}
    fputc(']',trace_file);
}
static uint32_t random32(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
static void fp_snapshot(uint64_t state[2]){
#ifdef _WIN32
    state[0]=_mm_getcsr();state[1]=0;
#else
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(state[0]),"=r"(state[1]));
#endif
}
static void w16(void*p,uint16_t v){memcpy(p,&v,2);}
static void w32(void*p,uint32_t v){memcpy(p,&v,4);}
static void w64(void*p,uint64_t v){memcpy(p,&v,8);}
void dispatch(CPU*c,uint64_t target){
    check(trace_count<128,"dispatch trace capacity");trace[trace_count++]=target;++calls;
    if(target==UINT64_C(0x140e21b00)){
        w32((void*)(uintptr_t)(active_object+12),(uint32_t)c->rdx);c->rax=UINT64_C(0xa5a55a5a);errno=ERANGE;
    }else if(assert_path){
        if(target==UINT64_C(0x140d8cac0)){
            if(c->rcx)w32((void*)(uintptr_t)c->r9,7);c->rax=7;
        }else if(target==UINT64_C(0x140e20bc0)){
            w64((void*)(uintptr_t)c->rcx,(uintptr_t)(arena+0x800));c->rax=(uintptr_t)(arena+0x800);
        }else if(target==UINT64_C(0x140e20f10))w64((void*)(uintptr_t)c->rcx,0);
        else if(target==UINT64_C(0x140e074b0)||target==UINT64_C(0x140d871b0))c->rax=0;
        else if(target!=UINT64_C(0x140e07320))check(0,"unknown cold assertion call");
    }else check(0,"unexpected canonical dispatch");
    c->rsp+=8;
}
void dispatch_jmp(CPU*c,uint64_t target){dispatch(c,target);}
void frostpunk_unsupported(uint64_t va,const char*instruction){(void)va;(void)instruction;check(0,"unsupported canonical instruction");}

#ifndef _WIN32
static sigjmp_buf fault_jump;
static volatile sig_atomic_t waiting_for_fault;
static void *fault_address;
static void fault_handler(int signal,siginfo_t *info,void *context){
    (void)context;if(!waiting_for_fault)_Exit(90+signal);
    fault_address=info->si_addr;waiting_for_fault=0;siglongjmp(fault_jump,1);
}
#endif
static int execute(void(*function)(CPU*),uint64_t *address){
#ifdef _WIN32
    __try{function(&cpu);return 0;}
    __except((*address=(uint64_t)GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1]),EXCEPTION_EXECUTE_HANDLER){return 1;}
#else
    fault_address=NULL;waiting_for_fault=1;
    if(!sigsetjmp(fault_jump,1)){function(&cpu);waiting_for_fault=0;return 0;}
    *address=(uintptr_t)fault_address;return 1;
#endif
}

static void map_fixture(void){
#ifdef _WIN32
    SYSTEM_INFO system;GetSystemInfo(&system);page_size=system.dwPageSize;
    image=VirtualAlloc(NULL,image_size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    arena=VirtualAlloc(NULL,arena_size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    guard_mapping=VirtualAlloc(NULL,3*page_size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    cpu_mapping=VirtualAlloc(NULL,0x10000,MEM_RESERVE,PAGE_NOACCESS);
    check(VirtualAlloc(cpu_mapping,page_size,MEM_COMMIT,PAGE_READWRITE)!=NULL,"CPU committed page");
    DWORD old;check(VirtualProtect(guard_mapping,page_size,PAGE_NOACCESS,&old),"leading guard");
    check(VirtualProtect(guard_mapping+2*page_size,page_size,PAGE_NOACCESS,&old),"trailing guard");
#else
    page_size=(size_t)sysconf(_SC_PAGESIZE);
    image=mmap(NULL,image_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    arena=mmap(NULL,arena_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    guard_mapping=mmap(NULL,3*page_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    cpu_mapping=mmap(NULL,0x10000,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    check(cpu_mapping!=MAP_FAILED&&!mprotect(cpu_mapping,page_size,PROT_READ|PROT_WRITE),"CPU committed page");
    check(image!=MAP_FAILED&&arena!=MAP_FAILED&&guard_mapping!=MAP_FAILED,"mmap");
    check(!mprotect(guard_mapping,page_size,PROT_NONE)&&!mprotect(guard_mapping+2*page_size,page_size,PROT_NONE),"guard mprotect");
    struct sigaction action={0};action.sa_sigaction=fault_handler;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
    check(!sigaction(SIGSEGV,&action,NULL)&&!sigaction(SIGBUS,&action,NULL),"fault handlers");
#endif
    check(image&&arena&&guard_mapping,"allocation");guard_end=guard_mapping+2*page_size;
    cpu_pointer=(CPU*)(cpu_mapping+page_size-1024);
    check(((uintptr_t)stack&4095u)==0,"portable virtual stack low bits");
    g_image_delta=(intptr_t)image-INT64_C(0x140000000);
}
static unsigned byte_gates(void){
    check(fp_android_string_resize_lookup(UINT64_C(0x140e222e0))==NULL,"lookup before init");
    w16(image,0x5a4d);w32(image+0x3c,0x80);memcpy(image+0x80,"PE\0\0",4);
    w16(image+0x84,0x8664);w16(image+0x86,1);w16(image+0x94,0xf0);w16(image+0x98,0x20b);
    w64(image+0xb0,UINT64_C(0x140000000));w32(image+0xd0,(uint32_t)image_size);
    w32(image+0x190,(uint32_t)image_size);w32(image+0x1ac,0x20000000);
    FILE *f=fopen("original-bytes.bin","rb");check(f!=NULL,"original byte data fixture");
    size_t n=fread(image+0xe222e0,1,298,f);check(n==298&&!ferror(f)&&fgetc(f)==EOF,"byte data size");fclose(f);
    check(fp_android_string_resize_init(image,image_size),"valid original PE gate");
    for(size_t i=0;i<298;++i){image[0xe222e0+i]^=1;
        check(!fp_android_string_resize_init(image,image_size)&&!fp_android_string_resize_lookup(UINT64_C(0x140e222e0)),"each opcode mutation rejects");
        image[0xe222e0+i]^=1;
    }
    check(!fp_android_string_resize_init(NULL,image_size)&&!fp_android_string_resize_init(image,63),"truncated/null PE rejects");
    w32(image+0x1ac,0);check(!fp_android_string_resize_init(image,image_size),"non-code rejects");w32(image+0x1ac,0x20000000);
    w64(image+0xb0,0);check(!fp_android_string_resize_init(image,image_size),"wrong image base rejects");w64(image+0xb0,UINT64_C(0x140000000));
    w32(image+0xd0,0xe22409);check(!fp_android_string_resize_init(image,image_size),"last code byte outside image rejects");w32(image+0xd0,(uint32_t)image_size);
    check(fp_android_string_resize_init(image,image_size),"final init");
    check(!fp_android_string_resize_lookup(UINT64_C(0x140e222e1))&&!fp_android_string_resize_lookup(UINT64_C(0x140e223e0)),"exact entry only");
    return 298;
}

static void paired(unsigned old,unsigned wanted,unsigned capacity,unsigned kind,unsigned seed){
    uint32_t rng=seed|1;CPU initial;
    for(size_t i=0;i<sizeof(initial);i+=4){uint32_t v=random32(&rng);memcpy((unsigned char*)&initial+i,&v,4);}
    initial.cf&=1;initial.zf&=1;initial.sf&=1;initial.of&=1;initial.pf&=1;initial.af&=1;
    unsigned char arena_before[0x10000],arena_expected[0x10000],stack_before[1024],stack_expected[1024],tail_before[64],tail_expected[64];
    memset(arena,0xa5,arena_size);memset(stack,0x5a,sizeof(stack));memset(guard_end-64,0xa5,64);
    uint64_t object=(uintptr_t)(arena+0x1000),data=(uintptr_t)(arena+0x2000);
    if(kind==1){data=object;++aliases;}
    else if(kind==2){data=(uintptr_t)&cpu.xmm[15];++aliases;}
    else if(kind==3){object=(uintptr_t)&cpu.xmm[14];data=(uintptr_t)&cpu.xmm[15];++aliases;}
    else if(kind==4){data=(uintptr_t)(stack+64);++aliases;}
    else if(kind==5){data=(uintptr_t)(guard_end-17);++guards;}
    else if(kind==6){data=(uintptr_t)guard_end;++guards;}
    else if(kind==7){data=(uintptr_t)(guard_end-1);++guards;}
    else if(kind==9){data=(uintptr_t)&cpu.rcx+1;++aliases;++guards;}
    else if(kind==10){data=(uintptr_t)&cpu.rax+5;++aliases;}
    initial.rcx=object;initial.rdx=((uint64_t)random32(&rng)<<32)|wanted;initial.rsp=(uintptr_t)(stack+768);
    initial.mxcsr=UINT32_C(0x1fa1)|((seed&3u)<<13);
    cpu=initial;w64((void*)(uintptr_t)object,data);w32((void*)(uintptr_t)(object+8),old);w32((void*)(uintptr_t)(object+12),capacity);
    initial=cpu;active_object=object;
    w32(image+0x296fae0,kind==8);assert_path=kind==8;
    memcpy(arena_before,arena,arena_size);memcpy(stack_before,stack,sizeof(stack));memcpy(tail_before,guard_end-64,64);
    trace_count=0;errno=E2BIG;uint64_t fault1=0,fault2=0,fp_before[2],fp_first[2],fp_second[2];
    fp_snapshot(fp_before);
    int bad1=execute(baseline_resize,&fault1),error1=errno;CPU expected=cpu;
    fp_snapshot(fp_first);
    uint64_t trace_expected[128];unsigned trace_n=trace_count;memcpy(trace_expected,trace,sizeof(trace));
    memcpy(arena_expected,arena,arena_size);memcpy(stack_expected,stack,sizeof(stack));memcpy(tail_expected,guard_end-64,64);
    memcpy(arena,arena_before,arena_size);memcpy(stack,stack_before,sizeof(stack));memcpy(guard_end-64,tail_before,64);cpu=initial;
    trace_count=0;errno=E2BIG;
    int bad2=execute(fp_android_string_resize_lookup(UINT64_C(0x140e222e0)),&fault2),error2=errno;
    fp_snapshot(fp_second);
    check(bad1==bad2&&fault1==fault2,"fault boundary");
    check(!memcmp(&cpu,&expected,sizeof(cpu)),"whole CPU including flags/XMM/rip");
    check(!memcmp(arena,arena_expected,arena_size)&&!memcmp(stack,stack_expected,sizeof(stack))&&!memcmp(guard_end-64,tail_expected,64),"whole memory / partial writes");
    check(error1==error2,"native errno");
    check(!memcmp(fp_before,fp_first,sizeof(fp_before))&&!memcmp(fp_before,fp_second,sizeof(fp_before)),"native FP controls/status");
    check(trace_n==trace_count&&!memcmp(trace,trace_expected,trace_n*8),"cold calls and order");
    if(trace_file){
        fprintf(trace_file,"{\"case\":%u,\"kind\":%u,\"old\":%u,\"wanted\":%u,\"capacity\":%u,\"fault\":%d,\"fault_address\":\"%016" PRIx64 "\",\"errno\":%d,\"cpu\":",case_number,kind,old,wanted,capacity,bad2,normalize(fault2),error2);
        trace_words(&cpu,sizeof(cpu));fputs(",\"object\":",trace_file);trace_words(arena+0x1000,32);
        fputs(",\"data\":",trace_file);trace_words(arena+0x2000,64);fputs(",\"tail\":",trace_file);trace_words(guard_end-64,64);
        fputs(",\"calls\":",trace_file);trace_words(trace,trace_count*8);fputs("}\n",trace_file);
    }
    hash64(case_number);hash64(bad2);hash64(normalize(fault2));hash_words(&cpu,sizeof(cpu));hash_words(arena+0x1000,32);
    hash_words(arena+0x2000,64);hash_words(guard_end-64,64);hash64(error2);hash_words(trace,trace_count*8);
    ++cases;++case_number;
}

static uint64_t now_ns(void){
#ifdef _WIN32
    LARGE_INTEGER ticks,frequency;QueryPerformanceCounter(&ticks);QueryPerformanceFrequency(&frequency);
    return (uint64_t)(ticks.QuadPart/frequency.QuadPart)*UINT64_C(1000000000)+
        (uint64_t)(ticks.QuadPart%frequency.QuadPart)*UINT64_C(1000000000)/(uint64_t)frequency.QuadPart;
#else
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*UINT64_C(1000000000)+now.tv_nsec;
#endif
}
static uint64_t benchmark(void(*function)(CPU*),unsigned iterations){
    uint64_t object=(uintptr_t)(arena+0x1000);active_object=object;assert_path=0;w32(image+0x296fae0,0);
    w64((void*)(uintptr_t)object,(uintptr_t)(arena+0x2000));w32((void*)(uintptr_t)(object+12),24576);
    uint64_t start=now_ns();
    for(unsigned i=0;i<iterations;++i){w32((void*)(uintptr_t)(object+8),0);memset(&cpu,0,sizeof(cpu));
        cpu.rcx=object;cpu.rdx=24576;cpu.rsp=(uintptr_t)(stack+768);function(&cpu);}
    uint64_t elapsed=now_ns()-start;
    check(arena[0x2000]==0x3f&&arena[0x7fff]==0x3f&&arena[0x8000]==0,"benchmark output");
    return elapsed;
}

int main(int argc,char **argv){
    if(argc==3&&!strcmp(argv[1],"--trace")){trace_file=fopen(argv[2],"wb");check(trace_file!=NULL,"open fixture trace");}
    else check(argc==1,"arguments");
    map_fixture();unsigned mutations=byte_gates();
    if(trace_file)fprintf(trace_file,"{\"kind\":\"trace_header\",\"sizeof_CPU\":%zu,\"page_size\":%zu,\"E2BIG\":%d,\"ERANGE\":%d}\n",sizeof(cpu),page_size,E2BIG,ERANGE);
    const unsigned values[]={0,1,2,3,7,15,16,17,31,63,127,255,511,1023,2047,4095,4096,4097,8191};
    for(unsigned r=0;r<16;++r)for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i)
        for(unsigned j=0;j<sizeof(values)/sizeof(*values);++j)
            paired(values[i],values[j],r&1?values[j]:16384,0,UINT32_C(0x12345678)+r*12345+i*23+j);
    for(unsigned kind=1;kind<=4;++kind)for(unsigned r=0;r<64;++r)
        paired(0,(r%15)+1,32,kind,UINT32_C(0x87654321)+r);
    for(unsigned r=0;r<64;++r){paired(0,40,64,5,r);paired(0,17,64,5,r+1);paired(0,1,64,6,r);paired(0,2,64,7,r);}
    for(unsigned r=0;r<64;++r)paired(0,UINT32_MAX,32,8,r);
    for(unsigned r=0;r<64;++r){paired(0,40,64,9,r);paired(0,15,64,10,r);}
    if(trace_file){check(!ferror(trace_file)&&!fclose(trace_file),"fixture trace write");trace_file=NULL;}
    uint64_t baseline_ns=benchmark(baseline_resize,2000);
    uint64_t candidate_ns=benchmark(fp_android_string_resize_lookup(UINT64_C(0x140e222e0)),2000);
    fprintf(stderr,"{\"kind\":\"isolated_fill_benchmark\",\"iterations\":2000,\"bytes_per_call\":24576,\"baseline_ns\":%" PRIu64 ",\"candidate_ns\":%" PRIu64 ",\"game_fps_measured\":false}\n",baseline_ns,candidate_ns);
    printf("{\"success\":true,\"full_CPU_cases\":%u,\"alias_cases\":%u,\"guard_cases\":%u,\"opcode_mutations\":%u,\"cold_dispatch_calls\":%u,\"digest\":\"%016" PRIx64 "\",\"original_bytes_executed\":false}\n",
           cases,aliases,guards,mutations,calls,digest);
    return 0;
}
