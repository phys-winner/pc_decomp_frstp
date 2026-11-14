/* Router/QPC differential. ARM links both exact kernel-provider variants;
 * Windows retains their extracted decline conditions and once/lock ordering. */
#include "win64.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <xmmintrin.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define LOCAL __declspec(thread)
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#else
#include <strings.h>
#include <pthread.h>
#define LOCAL _Thread_local
#endif

static void check(int yes,const char *why){if(!yes){fprintf(stderr,"QPC_HOTPATH_FAIL %s\n",why);exit(1);}}
static LOCAL uint32_t error_value;
static LOCAL int guest_errno,clock_failure,real_clock;
static LOCAL uint64_t fake_ns;
static LOCAL unsigned declines,metadata_calls,pointer_calls,clock_calls,kernel_calls;
static LOCAL CPU *active_cpu;
static LOCAL CPU initial_cpu;
static LOCAL int (*active_kernel_state)(void);
static LOCAL unsigned char *allowed_base;
static LOCAL size_t allowed_bytes;
static LOCAL int allowed_write;
static uint64_t semantic_cases,namespace_cases;
static unsigned char arena[96];
static int qpc_clock_gettime(int id,struct timespec *out){
    ++clock_calls;check(id==CLOCK_MONOTONIC,"monotonic clock id");
    if(clock_failure){errno=EIO;return -1;}
    if(!real_clock){out->tv_sec=(time_t)(fake_ns/1000000000);out->tv_nsec=(long)(fake_ns%1000000000);return 0;}
#if defined(_WIN32)
    LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);
    uint64_t whole=(uint64_t)t.QuadPart/(uint64_t)f.QuadPart,tail=(uint64_t)t.QuadPart%(uint64_t)f.QuadPart;
    out->tv_sec=(time_t)whole;out->tv_nsec=(long)(tail*1000000000/(uint64_t)f.QuadPart);return 0;
#else
    return clock_gettime(id,out);
#endif
}
static void environment(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void set_environment(const uint64_t in[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(in[0]),"r"(in[1]):"memory");
#else
    _mm_setcsr((unsigned)in[0]);
#endif
}
static void fault(const char *why){
    check(active_cpu&&!memcmp(active_cpu,&initial_cpu,sizeof(initial_cpu)),"fault before CPU/pop mutation");
    check(clock_calls==1&&kernel_calls==1,"clock and kernel ordering before fault");
    check(active_kernel_state&&active_kernel_state()==1,"real cold kernel initialization before fault");
    fprintf(stderr,"QPC_HOTPATH_REJECT reason=%s CPU_unchanged=1 RSP_unpopped=1 kernel_ready=1 clock_calls=1\n",why);exit(73);
}
static void invalid(const char *why){fault(why);}
void *win64_pointer(uint64_t p,size_t n,int writing){
    ++pointer_calls;uint64_t low=(uintptr_t)allowed_base;
    if(p<low||p-low>allowed_bytes||n>allowed_bytes-(size_t)(p-low)||(writing&&!allowed_write))fault("output pointer");
    return(void*)(uintptr_t)p;
}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t a[]={c->rcx,c->rdx,c->r8,c->r9};return a[i];}uint64_t v;memcpy(&v,win64_pointer(c->rsp+40+(i-4)*8,8,0),8);return v;}
uint32_t win64_get_error(void){return error_value;}
void win64_set_error(uint32_t e){error_value=e;}
void win64_stdio_set_errno(int n){guest_errno=n;}
const char *win64_string_a(uint64_t p){(void)p;check(0,"unexpected kernel string read");return NULL;}
uint64_t win64_alloc(size_t n,int zero){(void)n;(void)zero;check(0,"unexpected kernel allocation");return 0;}
void win64_free(uint64_t p){(void)p;check(0,"unexpected kernel free");}
void win64_region_add(uint64_t p,size_t n,int w){(void)p;(void)n;(void)w;check(0,"unexpected kernel region registration");}
void win64_region_remove(uint64_t p){(void)p;check(0,"unexpected kernel region removal");}
void win64_region_mutation_begin(void){++metadata_calls;}
void win64_region_mutation_end(void){++metadata_calls;}
static void metadata_lock(void){++metadata_calls;}
static void metadata_unlock(void){++metadata_calls;}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;++declines;return 0;}
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;++declines;return 0;}
static int unrelated_notification(CPU*c){(void)c;++declines;return 0;}

#include "qpc_kernel_declines.inc"
static int old_kernel(CPU*c,const char*n){++kernel_calls;return baseline_kernel_call(c,n);}
static int new_kernel(CPU*c,const char*n){++kernel_calls;return candidate_kernel_call(c,n);}
#define clock_gettime qpc_clock_gettime
#include "qpc_routes.inc"
#undef clock_gettime

typedef struct {CPU cpu;unsigned char memory[96];int native_errno,guest_error;uint32_t last_error;uint64_t fp[2];unsigned clock_count,pointer_count,kernel_count,metadata_count;} result;
static result invoke(int candidate,CPU *cpu,const char*dll,const char*name){
    active_cpu=cpu;initial_cpu=*cpu;active_kernel_state=candidate?candidate_kernel_state:baseline_kernel_state;declines=metadata_calls=pointer_calls=clock_calls=kernel_calls=0;errno=E2BIG;guest_errno=31337;error_value=0x51cafe;
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=candidate?candidate_route(cpu,dll,name):baseline_route(cpu,dll,name);
    check(handled==1,"QPC handled");
    result r;memset(&r,0,sizeof(r));r.cpu=*cpu;r.native_errno=errno;r.guest_error=guest_errno;r.last_error=error_value;r.clock_count=clock_calls;r.pointer_count=pointer_calls;r.kernel_count=kernel_calls;r.metadata_count=metadata_calls;
    memcpy(r.memory,arena,sizeof(arena));environment(r.fp);
#if defined(_WIN32)
    check(GetLastError()==0x5a17,"native LastError unchanged");
#endif
    active_cpu=NULL;return r;
}
static void pair(size_t offset,int cpu_alias,unsigned mode){
    CPU live,before;memset(&live,0xa5,sizeof(live));live.rsp=UINT64_C(0x1234567800100020);live.mxcsr=0x1f80|((mode&3)<<13);
    live.rcx=(uintptr_t)(cpu_alias?(unsigned char*)&live+offset:arena+offset);before=live;
    allowed_base=cpu_alias?(unsigned char*)&live:arena;allowed_bytes=cpu_alias?sizeof(live):sizeof(arena);allowed_write=1;
    unsigned char old[96];memset(arena,0x5a,sizeof(arena));memcpy(old,arena,sizeof(old));
    uint64_t initial_fp[2],saved[2];environment(saved);
#if defined(__aarch64__)
    initial_fp[0]=(saved[0]&~(UINT64_C(3)<<22))|((uint64_t)(mode&3)<<22)|((mode&4)?UINT64_C(1)<<24:0)|((mode&8)?UINT64_C(1)<<25:0);initial_fp[1]=0x15;
#else
    initial_fp[0]=UINT32_C(0x1f95)|((mode&3)<<13)|((mode&4)?64:0)|((mode&8)?32768:0);initial_fp[1]=0;
#endif
    set_environment(initial_fp);fake_ns=UINT64_C(0x123456789abc)+semantic_cases;
    result a=invoke(0,&live,"KERNEL32.dll","QueryPerformanceCounter");live=before;memcpy(arena,old,sizeof(old));
    result b=invoke(1,&live,mode&16?"KeRnElBaSe.DlL":"kernel32.dll","QueryPerformanceCounter");
    if(memcmp(&a,&b,sizeof(a)))fprintf(stderr,"QPC_PAIR_DETAIL case=%"PRIu64" offset=%zu alias=%d mode=%u CPU=%d memory=%d native_errno=%d/%d guest_errno=%d/%d FP=%d counters=%u/%u,%u/%u,%u/%u,%u/%u\n",semantic_cases,offset,cpu_alias,mode,memcmp(&a.cpu,&b.cpu,sizeof(CPU)),memcmp(a.memory,b.memory,sizeof(a.memory)),a.native_errno,b.native_errno,a.guest_error,b.guest_error,memcmp(a.fp,b.fp,sizeof(a.fp)),a.clock_count,b.clock_count,a.pointer_count,b.pointer_count,a.kernel_count,b.kernel_count,a.metadata_count,b.metadata_count);
    check(!memcmp(&a,&b,sizeof(a)),"paired CPU/memory/errors/nativeFP");
    CPU expected=before;memcpy(cpu_alias?(unsigned char*)&expected+offset:old+offset,&fake_ns,8);expected.rax=1;expected.rsp+=8;
    check(!memcmp(&b.cpu,&expected,sizeof(expected)),"exact CPU/output aliases/pop");
    if(!cpu_alias)check(!memcmp(b.memory,old,sizeof(old)),"exact eight-byte output/guards");
    check(!memcmp(initial_fp,b.fp,sizeof(initial_fp))&&b.native_errno==E2BIG&&b.guest_error==31337&&b.last_error==0x51cafe,"FP/errors unchanged");
    check(b.clock_count==1&&b.pointer_count==1&&b.kernel_count==1&&!b.metadata_count,"single clock/pointer/kernel without metadata");set_environment(saved);++semantic_cases;
}
static uint64_t nanos(void){int previous=real_clock;real_clock=1;struct timespec t;check(!qpc_clock_gettime(CLOCK_MONOTONIC,&t),"benchmark clock");real_clock=previous;return(uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}
static uint64_t bench(int candidate,unsigned iterations){
    CPU c={0};c.rcx=(uintptr_t)arena;allowed_base=arena;allowed_bytes=sizeof(arena);allowed_write=1;real_clock=1;active_cpu=&c;
    uint64_t begin=nanos();for(unsigned i=0;i<iterations;++i){c.rsp=0x100000;(void)(candidate?candidate_route(&c,"KERNEL32.dll","QueryPerformanceCounter"):baseline_route(&c,"KERNEL32.dll","QueryPerformanceCounter"));}
    uint64_t end=nanos(),last;memcpy(&last,arena,8);real_clock=0;active_cpu=NULL;check(c.rax==1&&c.rsp==0x100008&&begin<=last&&last<=end,"benchmark result and actual monotonic output");return end-begin;
}
int main(int argc,char**argv){
    check(!baseline_kernel_state()&&!candidate_kernel_state(),"kernel initially cold");
    if(argc==4&&!strcmp(argv[1],"--negative")){
        CPU c;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)arena;c.rsp=0x12345000;allowed_base=arena;allowed_bytes=sizeof(arena);allowed_write=1;
        const char*why=argv[3];if(!strcmp(why,"null"))c.rcx=0;else if(!strcmp(why,"short"))allowed_bytes=7;else if(!strcmp(why,"readonly"))allowed_write=0;else if(!strcmp(why,"overflow"))c.rcx=UINT64_MAX-3;else if(!strcmp(why,"clock")){clock_failure=1;c.rcx=0;}else return 2;
        (void)invoke(!strcmp(argv[2],"candidate"),&c,"KERNEL32.dll","QueryPerformanceCounter");return 99;
    }
    pair(0,0,0);check(baseline_kernel_state()==1&&candidate_kernel_state()==1,"cold kernel initialized exactly once/no roots");
    for(unsigned mode=0;mode<32;++mode){for(unsigned i=0;i<65;++i)pair(i,0,mode);for(size_t i=0;i+8<=sizeof(CPU);++i)pair(i,1,mode);}
    const char*dlls[]={"ucrtbase.dll","api-ms-win-crt-runtime-l1-1-0.dll","user32.dll","ntdll.dll","kernel32.dll.fake","xkernel32.dll","dxgi.dll"};
    for(unsigned i=0;i<sizeof(dlls)/sizeof(*dlls);++i)for(unsigned variant=0;variant<2;++variant){CPU c;memset(&c,0x55,sizeof(c));CPU old=c;declines=metadata_calls=kernel_calls=0;int yes=variant?candidate_route(&c,dlls[i],"QueryPerformanceCounter"):baseline_route(&c,dlls[i],"QueryPerformanceCounter");check(!yes&&!memcmp(&c,&old,sizeof(c))&&!kernel_calls&&!metadata_calls,"wrong DLL declines without dereference/pop");++namespace_cases;}
    const char*names[]={"queryperformancecounter","QueryPerformanceCounterX","QueryPerformanceFrequencyX"};
    for(unsigned i=0;i<3;++i){CPU a={0},b={0};check(!baseline_route(&a,"kernel32.dll",names[i])&&!candidate_route(&b,"kernel32.dll",names[i])&&!memcmp(&a,&b,sizeof(a)),"wrong exact name remains slow decline");++namespace_cases;}
    const unsigned iterations=100000;uint64_t old[5],candidate[5];for(unsigned i=0;i<5;++i){if(i&1){candidate[i]=bench(1,iterations);old[i]=bench(0,iterations);}else{old[i]=bench(0,iterations);candidate[i]=bench(1,iterations);}}
    check(baseline_kernel_state()==1&&candidate_kernel_state()==1,"kernel remains exactly once initialized/no roots");
    printf("{\"success\":true,\"paired_cases\":%"PRIu64",\"namespace_cases\":%"PRIu64",\"CPU_alias_cases\":%zu,\"FP_modes\":32,\"kernel_first_init_each\":1,\"fullCPU_memory_nativeFP_errno\":true,\"benchmark\":{\"iterations_each\":%u,\"baseline_ns\":[",semantic_cases,namespace_cases,32*(sizeof(CPU)-7),iterations);
    for(unsigned i=0;i<5;++i)printf("%s%"PRIu64,i?",":"",old[i]);printf("],\"candidate_ns\":[");for(unsigned i=0;i<5;++i)printf("%s%"PRIu64,i?",":"",candidate[i]);puts("]}}");return 0;
}
