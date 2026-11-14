/* Complete generated extents; paired external-boundary effects, no original execution. */
#define _GNU_SOURCE
#include "cpu64.h"
#include "aot_cpu.h"
#include "particle_scalar_candidate.h"
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "../android/host/aot_cpu.c"
#include "../android/host/aot_cpu_float.c"
#include "particle_original_data.inc"

#define ARENA_BYTES 65536
#define MAX_BOUNDARIES 24
int64_t g_image_delta;
static unsigned char *image,*arena;
static CPU *active;
static unsigned char initial_memory[ARENA_BYTES],expected_memory[ARENA_BYTES];
typedef struct {uint64_t target,fp[2];int error;unsigned last_error;unsigned char memory[ARENA_BYTES];} boundary;
static boundary boundaries[MAX_BOUNDARIES];
static unsigned variant,boundary_count,expected_boundaries,case_id,entry_id,alias_id,value_id,mode_id,loops;
static unsigned cases,complete_cases,unsupported_cases,fault_cases,alias_cases;
static unsigned memory_mismatches,fp_mismatches,trace_mismatches;
static unsigned benchmark_mode;
static unsigned per_entry[3],per_alias[5];
static uint32_t random_state=UINT32_C(0xf63a856c);
static uint32_t random32(void){random_state=random_state*UINT32_C(1664525)+UINT32_C(1013904223);return random_state;}
static void require(int yes,const char *message){if(!yes){fprintf(stderr,"PARTICLE_FUNCTION_FIXTURE %s case=%u entry=%u alias=%u value=%u mode=%u loops=%u\n",message,case_id,entry_id,alias_id,value_id,mode_id,loops);exit(2);}}
static void fp_read(uint64_t out[2]) {
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void fp_write(const uint64_t in[2]) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(in[0]),"r"(in[1]):"memory");
#else
    _mm_setcsr((unsigned)in[0]);
#endif
}
static void fp_mode(uint64_t out[2],const uint64_t original[2],unsigned n) {
#if defined(__aarch64__)
    unsigned rounding=n&3,arm=((rounding&1)<<1)|((rounding&2)>>1);
    out[0]=(original[0]&~((UINT64_C(3)<<22)|(UINT64_C(1)<<24)|(UINT64_C(1)<<25)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)|UINT64_C(3)))|
        ((uint64_t)arm<<22)|(n&4?UINT64_C(1)<<24:0)|(n&8?UINT64_C(1)<<25:0);
    out[1]=n&16?UINT64_C(0x80000095):0;
#else
    (void)original;out[0]=0x1f80|((n&3)<<13)|(n&4?64:0)|(n&8?32768:0)|(n&16?0x15:0);out[1]=0;
#endif
}
static unsigned last_error(void) {
#ifdef _WIN32
    return GetLastError();
#else
    return 0;
#endif
}
static void set_last_error(unsigned value) {
#ifdef _WIN32
    SetLastError(value);
#else
    (void)value;
#endif
}
static void mismatch(const char *kind,unsigned *count) {
    if((*count)++<4)fprintf(stderr,"PARTICLE_FUNCTION_DIFF %s case=%u entry=%u alias=%u value=%u mode=%u loops=%u boundary=%u\n",kind,case_id,entry_id,alias_id,value_id,mode_id,loops,boundary_count);
}
static void record_boundary(uint64_t target) {
    if(benchmark_mode)return;
    uint64_t fp[2];fp_read(fp);int error=errno;unsigned native_error=last_error();
    require(boundary_count<MAX_BOUNDARIES,"bounded dispatch trace");
    boundary *b=&boundaries[boundary_count];
    if(!variant){b->target=target;memcpy(b->fp,fp,sizeof(fp));b->error=error;b->last_error=native_error;memcpy(b->memory,arena,ARENA_BYTES);}
    else {
        if(boundary_count>=expected_boundaries||b->target!=target)mismatch("dispatch order",&trace_mismatches);
        if(memcmp(b->memory,arena,ARENA_BYTES))mismatch("dispatch full CPU/live memory",&memory_mismatches);
        if(memcmp(b->fp,fp,sizeof(fp))||b->error!=error||b->last_error!=native_error)mismatch("dispatch native FP/errors",&fp_mismatches);
    }
    ++boundary_count;errno=error;set_last_error(native_error);
}
void dispatch(CPU *c,uint64_t target) {
    require(c==active,"same live CPU address");record_boundary(target);
    if(target==UINT64_C(0x141ae1130)) {
        /* Captured input state is the relational oracle; this deterministic
         * callee double stresses return lanes and preserved native side effects. */
        static const uint32_t returns[4][4]={{0x3f000000,0xbf000000,0x3f800000,0},
            {0,0x3f800000,0x80000000,0xbf800000},{0x7fc00123,0x7f800001,0xff800000,1},
            {0x3f3504f3,0xbf3504f3,0x3f000000,0x3f800000}};
        memcpy(c->xmm[0].u32,returns[value_id&3],16);c->rsp+=8;
        uint64_t fp[2];fp_read(fp);fp[1]|=UINT64_C(0x11);
#if !defined(__aarch64__)
        fp[0]|=UINT64_C(0x21);fp[1]=0;
#endif
        fp_write(fp);errno=EDOM;set_last_error(1618);return;
    }
    require(target==UINT64_C(0x140f642c5)||target==UINT64_C(0x140f64b00)||target==UINT64_C(0x140f6754f)||target==UINT64_C(0x140f6755d),"known tail boundary");
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
static jmp_buf stop_jump;
static uint64_t stopped_va;
void frostpunk_unsupported(uint64_t va,const char *instruction){(void)instruction;record_boundary(va|UINT64_C(0xf000000000000000));stopped_va=va;longjmp(stop_jump,1);}
static uint64_t fixture_gs64(uint64_t off){require(off==0x58,"TLS slot address");record_boundary(UINT64_C(0xffffffff00000058));return (uintptr_t)(arena+0x7000);}
#define rd_gs64 fixture_gs64
#include "particle_baseline.inc"
#include "particle_candidate.inc"
#undef rd_gs64

static void (*baseline_functions[])(CPU*)={baseline_F63A85,baseline_F6460C,baseline_F66DC6};
static void (*candidate_functions[])(CPU*)={candidate_F63A85,candidate_F6460C,candidate_F66DC6};
#ifndef _WIN32
static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_waiting;
static uint64_t fault_address;
static int fault_signal;
static void fault_handler(int signal,siginfo_t *info,void *context){(void)context;if(!fault_waiting)_Exit(90+signal);fault_address=(uintptr_t)info->si_addr;fault_signal=signal;fault_waiting=0;siglongjmp(fault_jump,1);}
#endif
static int execute(void(*fn)(CPU*),uint64_t *fault,unsigned *code) {
    stopped_va=0;
    if(setjmp(stop_jump)){*fault=stopped_va;*code=73;return 2;}
#ifdef _WIN32
    __try{fn(active);return 0;}
    __except((*fault=(uint64_t)GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1],*code=GetExceptionCode()),EXCEPTION_EXECUTE_HANDLER){return 1;}
#else
    fault_address=0;fault_signal=0;fault_waiting=1;
    if(!sigsetjmp(fault_jump,1)){fn(active);fault_waiting=0;return 0;}
    *fault=fault_address;*code=(unsigned)fault_signal;return 1;
#endif
}
static void map_data(void) {
#ifdef _WIN32
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    image=VirtualAlloc(NULL,FIXTURE_IMAGE_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    unsigned char *mapping=VirtualAlloc(NULL,ARENA_BYTES+2*page,MEM_RESERVE,PAGE_NOACCESS);
    require(mapping!=NULL,"reserve guarded arena");arena=VirtualAlloc(mapping+page,ARENA_BYTES,MEM_COMMIT,PAGE_READWRITE);
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    image=mmap(NULL,FIXTURE_IMAGE_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    unsigned char *mapping=mmap(NULL,ARENA_BYTES+2*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    require(mapping!=MAP_FAILED,"reserve guarded arena");arena=mapping+page;require(!mprotect(arena,ARENA_BYTES,PROT_READ|PROT_WRITE),"commit arena");
    struct sigaction action={0};action.sa_sigaction=fault_handler;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
    require(!sigaction(SIGSEGV,&action,NULL)&&!sigaction(SIGBUS,&action,NULL)&&!sigaction(SIGFPE,&action,NULL),"fault handlers");
#endif
    require(image&&arena,"fixture memory allocation");
#ifndef _WIN32
    require(image!=MAP_FAILED,"image mmap");
#endif
    g_image_delta=(intptr_t)image-INT64_C(0x140000000);
    memcpy(image,fixture_pe_header,sizeof(fixture_pe_header));
    for(size_t i=0;i<sizeof(fp_aot_gates)/sizeof(*fp_aot_gates);++i)memcpy((void*)(uintptr_t)GVA(fp_aot_gates[i].va),fp_aot_gates[i].bytes,fp_aot_gates[i].size);
    for(size_t i=0;i<sizeof(fixture_constants)/sizeof(*fixture_constants);++i)memcpy((void*)(uintptr_t)GVA(fixture_constants[i].va),fixture_constants[i].bytes,16);
    require(fp_aot_cpu_init(image,FIXTURE_IMAGE_BYTES),"actual packed helper original-byte gates");
    active=(CPU*)(arena+0xe000);
}
static const uint32_t values[]={0x3e800000,0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
    0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00123,0x7f800001,0xff800123};
static void w32(unsigned char *p,uint32_t v){memcpy(p,&v,4);}
static void w64(unsigned char *p,uint64_t v){memcpy(p,&v,8);}
static void prepare_case(void) {
    for(unsigned i=0;i<ARENA_BYTES;++i)arena[i]=(unsigned char)random32();
    unsigned char *config=arena+0x1000,*records=arena+0x4000,*stack=arena+0x400;
    for(unsigned at=0;at<0x500;at+=4)w32(config+at,(value_id==0)?UINT32_C(0x3f000000):values[(value_id+(at/4)%3)%16]);
    for(unsigned at=0x240;at<0x260;at+=4)w32(config+at,at<0x250?0:UINT32_C(0x3f800000));
    w32(config+0x3e0,4);w32(config+0x3e4,4);
    for(unsigned at=0x220;at<=0x222;++at)config[at]=(unsigned char)((case_id>>(at-0x220))&1);
    config[0x3ee]=(case_id>>3)&1;config[0x3ef]=(case_id>>4)&1;
    for(unsigned i=0;i<32;++i)for(unsigned at=0;at<56;at+=4)w32(records+i*56+at,value_id?values[(value_id+i+at/4)%16]:UINT32_C(0x3f000000));
    for(unsigned i=0;i<32;++i){records[i*56+12]=(unsigned char)i;records[i*56+13]=(unsigned char)(i*7);}
    memset(active,0x5a,sizeof(*active));for(unsigned r=0;r<16;++r)for(unsigned lane=0;lane<4;++lane)active->xmm[r].u32[lane]=value_id?values[(value_id+r+lane)%16]:UINT32_C(0x3f000000);
    active->cf=case_id&1;active->zf=(case_id>>1)&1;active->sf=(case_id>>2)&1;active->of=(case_id>>3)&1;active->pf=(case_id>>4)&1;active->af=(case_id>>5)&1;
    active->mxcsr=0x1f80|((mode_id&3)<<13)|(mode_id&4?0x40:0)|(mode_id&8?0x8000:0)|(mode_id&16?0x3f:0);
    active->rsp=(uintptr_t)stack;active->rcx=128;active->r9=1;
    active->rdi=(uintptr_t)(arena+0x8000);
    if(alias_id==1)active->rdi=(uintptr_t)(records+4);
    if(alias_id==2)active->rdi=(uintptr_t)(config+0x240);
    if(alias_id==3)active->rdi=(uintptr_t)(stack+0x100);
    if(alias_id==4)active->rdi=(uintptr_t)&active->xmm[0];
    for(unsigned at=0x20;at<0x200;at+=4)w32(stack+at,value_id?values[(value_id+at/4)%16]:UINT32_C(0x3f000000));
    if(entry_id==0){active->r11=(uintptr_t)(stack+0x178);active->rbp=(uintptr_t)config;active->r14=(uintptr_t)records;active->rdx=0;w32(stack+0x28,0);w32(stack+0x40,loops);}
    else if(entry_id==1){active->r11=(uintptr_t)(stack+0x198);active->rbp=(uintptr_t)(stack+0x100);active->r14=(uintptr_t)config;active->r15=(uintptr_t)records;active->r13=0;w32(stack+0x54,loops);}
    else {active->r11=(uintptr_t)(stack+0x158);active->rsi=(uintptr_t)config;active->r14=(uintptr_t)records;active->r13=0;w32(stack+0x48,loops);}
    w64(arena+0x7000,(uintptr_t)(arena+0x7100));w32(arena+0x711c,0);w32((void*)(uintptr_t)GVA(0x14400aa98),0);w32((void*)(uintptr_t)GVA(0x1440118a0),0);
    active->rip=fixture_targets[entry_id];
}
static void paired(const uint64_t original[2]) {
    ++case_id;prepare_case();memcpy(initial_memory,arena,ARENA_BYTES);
    uint64_t mode[2],old_fp[2],new_fp[2],old_fault=0,new_fault=0;unsigned old_code=0,new_code=0;
    fp_mode(mode,original,mode_id);variant=0;boundary_count=0;fp_write(mode);errno=2718;set_last_error(3141);
    int old_status=execute(baseline_functions[entry_id],&old_fault,&old_code);fp_read(old_fp);int old_errno=errno;unsigned old_error=last_error();
    if(!old_status){require((entry_id?R32(active->r13):R32(active->rdx))==loops,"complete baseline loop count");require(boundary_count==(entry_id<2?loops+1:2),"complete baseline boundary count");}
    expected_boundaries=boundary_count;memcpy(expected_memory,arena,ARENA_BYTES);memcpy(arena,initial_memory,ARENA_BYTES);
    variant=1;boundary_count=0;fp_write(mode);errno=2718;set_last_error(3141);
    int new_status=execute(candidate_functions[entry_id],&new_fault,&new_code);fp_read(new_fp);int new_errno=errno;unsigned new_error=last_error();
    if(!new_status){require((entry_id?R32(active->r13):R32(active->rdx))==loops,"complete candidate loop count");require(boundary_count==(entry_id<2?loops+1:2),"complete candidate boundary count");}
    fp_write(original);
    if(old_status!=new_status||old_fault!=new_fault||old_code!=new_code||boundary_count!=expected_boundaries)mismatch("terminal status/dispatch",&trace_mismatches);
    if(memcmp(expected_memory,arena,ARENA_BYTES))mismatch("terminal full CPU/live memory",&memory_mismatches);
    if(memcmp(old_fp,new_fp,sizeof(old_fp))||old_errno!=new_errno||old_error!=new_error)mismatch("terminal native FP/errors",&fp_mismatches);
    ++cases;++per_entry[entry_id];++per_alias[alias_id];alias_cases+=alias_id!=0;
    if(!old_status)++complete_cases;else if(old_status==2)++unsupported_cases;else ++fault_cases;
}
static uint64_t now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER now,frequency;require(QueryPerformanceCounter(&now)&&QueryPerformanceFrequency(&frequency),"benchmark clock");
    return (uint64_t)((long double)now.QuadPart*1000000000.0L/frequency.QuadPart);
#else
    struct timespec now;require(!clock_gettime(CLOCK_MONOTONIC,&now),"benchmark clock");
    return (uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
#endif
}
static int benchmark(unsigned iterations,const uint64_t original[2]) {
    benchmark_mode=1;alias_id=value_id=mode_id=0;loops=8;
    uint64_t timing[3][5][2]={0},mode[2];fp_mode(mode,original,0);
    for(entry_id=0;entry_id<3;++entry_id) {
        prepare_case();memcpy(initial_memory,arena,ARENA_BYTES);
        for(unsigned sample=0;sample<5;++sample)for(unsigned repeat=0;repeat<iterations;++repeat) {
            for(unsigned phase=0;phase<2;++phase) {
                unsigned chosen=phase^((sample+repeat)&1);
                memcpy(arena,initial_memory,ARENA_BYTES);fp_write(mode);errno=2718;set_last_error(3141);
                uint64_t begin=now_ns();
                (chosen?candidate_functions[entry_id]:baseline_functions[entry_id])(active);
                uint64_t end=now_ns();require(end>=begin,"monotonic benchmark clock");timing[entry_id][sample][chosen]+=end-begin;
                require((entry_id?R32(active->r13):R32(active->rdx))==8,"benchmark eight particles completed");
                if(!phase)memcpy(expected_memory,arena,ARENA_BYTES);
                else require(!memcmp(expected_memory,arena,ARENA_BYTES),"benchmark full CPU/live output mismatch");
            }
        }
    }
    fp_write(original);
    printf("{\"success\":true,\"kind\":\"three-complete-extent-timing\",\"invocations_per_sample\":%u,\"particles_per_invocation\":8,\"prepare_and_snapshot_time_excluded\":true,\"external_callee_test_double\":true,\"entries\":[",iterations);
    for(unsigned e=0;e<3;++e) {
        printf("%s{\"va\":\"0x%" PRIx64 "\",\"samples\":[",e?",":"",fixture_targets[e]);
        for(unsigned s=0;s<5;++s)printf("%s{\"baseline_ns\":%" PRIu64 ",\"candidate_ns\":%" PRIu64 "}",s?",":"",timing[e][s][0],timing[e][s][1]);
        printf("]}");
    }
    printf("],\"production_changed\":false,\"gameplay_proven\":false}\n");return 0;
}
int main(int argc,char **argv) {
    unsigned iterations=0;
    if(argc!=1) {
        if(argc!=3||strcmp(argv[1],"--benchmark"))return 2;
        char *end;unsigned long n=strtoul(argv[2],&end,10);
        if(!*argv[2]||*end||n<1||n>4096)return 2;iterations=(unsigned)n;
    }
    map_data();uint64_t original[2];fp_read(original);
    if(iterations)return benchmark(iterations,original);
    for(entry_id=0;entry_id<3;++entry_id)for(value_id=0;value_id<16;++value_id)for(mode_id=0;mode_id<32;++mode_id)
        for(alias_id=0;alias_id<5;++alias_id)for(unsigned n=0;n<3;++n){loops=n==0?1:n==1?2:8;if(alias_id==2&&loops>2||alias_id==3&&loops>1)continue;paired(original);}
    fp_write(original);
    unsigned failed=memory_mismatches||fp_mismatches||trace_mismatches;
    printf("{\"success\":%s,\"cases\":%u,\"complete_extent_cases\":%u,\"unsupported_boundary_cases\":%u,\"native_fault_cases\":%u,\"alias_cases\":%u,\"native_FP_modes\":32,\"per_entry\":[%u,%u,%u],\"per_alias\":[%u,%u,%u,%u,%u],\"memory_CPU_mismatches\":%u,\"FP_error_mismatches\":%u,\"trace_status_mismatches\":%u,\"external_callee_test_double\":true,\"actual_packed_helpers\":true,\"production_changed\":false,\"original_bytes_executed\":false}\n",
        failed?"false":"true",cases,complete_cases,unsupported_cases,fault_cases,alias_cases,per_entry[0],per_entry[1],per_entry[2],per_alias[0],per_alias[1],per_alias[2],per_alias[3],per_alias[4],memory_mismatches,fp_mismatches,trace_mismatches);
    return failed?1:0;
}
