/* Control-flow proof only. Generated dispatch code is unchanged; provider and
 * AOT bodies are deterministic doubles over the real sealed address tables.
 * The optional benchmark excludes tracing and does not measure game startup. */
#define _POSIX_C_SOURCE 200809L
#include "cpu64.h"
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define LOCAL __declspec(thread)
#define _Thread_local __declspec(thread)
#else
#include <pthread.h>
#define LOCAL _Thread_local
#endif
#define FP_NOINLINE __attribute__((noinline))
#define FROSTPUNK_HAVE_STRING_RESIZE 1
#define FROSTPUNK_HAVE_TEXTURE_FORMATS 1
#define FROSTPUNK_HAVE_SUPPLEMENTAL 1
#define FROSTPUNK_HAVE_PAYLOAD 1
#define HLE_BASE UINT64_C(0xe5e30000)
#define HLE_COUNT 557
#define HLE_STRIDE 8
typedef void (*fn_t)(CPU *);
typedef struct {uint64_t va;fn_t fn;} recomp_entry_t;
typedef struct {uint64_t slot;const char *dll,*name;} hle_import_t;
static recomp_entry_t *recomp_dispatch_table;
static uint32_t recomp_dispatch_count;
static uint64_t *tables[8];static uint32_t counts[8];
#include "dispatch_namespaces.inc"
static LOCAL unsigned events[128],event_count,selected_body;
static LOCAL jmp_buf fault;
static LOCAL int fault_code,manual_ready=1,payload_ready=1,dependency_ready=1,audio_mode,import_enabled=1;
static LOCAL CPU working;
static LOCAL uint64_t stack_words[64],stack_lo,stack_hi;
static LOCAL unsigned baseline_depth,candidate_depth;
static LOCAL uint64_t baseline_calls,candidate_calls,baseline_budget,candidate_budget;
static LOCAL int baseline_trace,candidate_trace;
static uint64_t fetch_add(uint64_t *value,uint64_t increment){uint64_t old=*value;*value+=increment;return old;}
#define atomic_fetch_add_explicit(p,v,m) fetch_add((p),(v))
#define memory_order_relaxed 0
static void record(unsigned event){
#if !defined(FP_DISPATCH_BENCHMARK)
    if(event_count==128){fputs("dispatch trace overflow\n",stderr);exit(2);}events[event_count++]=event;
#else
    (void)event;
#endif
}
static _Noreturn void fail(int code,const char *format,...){(void)format;fault_code=code;longjmp(fault,1);}
#define stop(...) fail(73,__VA_ARGS__)
static uint64_t return_pc(const CPU *c){return c->rsp>=stack_lo&&c->rsp<=stack_hi-8?rd64(c->rsp):0;}
static int member(unsigned table,uint64_t target){uint32_t lo=0,hi=counts[table];while(lo<hi){uint32_t m=lo+(hi-lo)/2;if(tables[table][m]<target)lo=m+1;else hi=m;}return lo<counts[table]&&tables[table][lo]==target;}
static void body(CPU *c,unsigned kind){record(20+kind);selected_body=kind;c->rax+=c->rip+kind;c->rsp+=8;}
static void canonical(CPU*c){body(c,1);}static void supplemental(CPU*c){body(c,2);}static void extra(CPU*c){body(c,3);}
static void resize(CPU*c){body(c,4);}static void texture(CPU*c){body(c,5);}static void shader_body(CPU*c){body(c,6);}
static void payload_body(CPU*c){body(c,7);}static void dependency_body(CPU*c){body(c,8);}
static void frostpunk_shader_lookup_1410596a9(CPU*c){shader_body(c);}
static void frostpunk_shader_lookup_1410597e9(CPU*c){shader_body(c);}
static FP_NOINLINE fn_t fp_android_string_resize_lookup(uint64_t t){record(11);return manual_ready&&t==UINT64_C(0x140e222e0)?resize:NULL;}
static FP_NOINLINE fn_t fp_android_texture_formats_lookup(uint64_t t){record(12);return manual_ready&&(t==UINT64_C(0x140d95650)||t==UINT64_C(0x140f499a0))?texture:NULL;}
static FP_NOINLINE fn_t frostpunk_supplemental_lookup(uint64_t t){record(13);return member(1,t)?supplemental:NULL;}
static FP_NOINLINE fn_t fp_android_extra_aot_lookup(uint64_t t){record(14);return manual_ready&&member(2,t)?extra:NULL;}
static FP_NOINLINE int win64_dispatch_import(CPU*c,const char*dll,const char*name){(void)dll;(void)name;record(30);if(!import_enabled)return 0;body(c,9);return 1;}
static FP_NOINLINE int frostpunk_payload_import(CPU*c,uint64_t t){record(1);if(t<UINT64_C(0xe5e60000)||t-UINT64_C(0xe5e60000)>=92*8)return 0;if(t&7)fail(72,"payload alignment");body(c,10);return 1;}
static FP_NOINLINE int frostpunk_dependencies_import(CPU*c,uint64_t t){record(2);if(t<UINT64_C(0xe5e80000)||t-UINT64_C(0xe5e80000)>=185*8)return 0;if(t&7)fail(73,"dependency alignment");body(c,11);return 1;}
static FP_NOINLINE int dinput(CPU*c,uint64_t t){record(4);if(!((t>=UINT64_C(0xe7000000)&&t<UINT64_C(0xe7000058))||(t>=UINT64_C(0xe7000100)&&t<UINT64_C(0xe7000200))))return 0;if(t&7)fail(73,"dinput alignment");body(c,12);return 1;}
static FP_NOINLINE int openal(CPU*c,uint64_t t){record(5);if(t<OPENAL_MIN||t>OPENAL_MAX)return 0;if(!member(5,t))return 0;if(!audio_mode)return 0;if(audio_mode==2)fail(73,"OpenAL after shutdown");if(!member(6,t))fail(73,"unimplemented OpenAL public entry");body(c,13);return 1;}
static FP_NOINLINE int shader(CPU*c,uint64_t t){record(6);if(t<UINT64_C(0xe5ea0000)||t>=UINT64_C(0xe5ea00b0)||(t&7))return 0;body(c,14);return 1;}
static FP_NOINLINE int steam(CPU*c,uint64_t t){record(7);if(t<UINT64_C(0xe6000000)||t>=UINT64_C(0xe6003800))return 0;if(t&7)fail(73,"steam alignment");body(c,15);return 1;}
static FP_NOINLINE int d3d(CPU*c,uint64_t t){record(8);if(t<UINT64_C(0xe5f00000)||t>=UINT64_C(0xe5f1b000))return 0;if(t&7)fail(73,"D3D alignment");body(c,16);return 1;}
static FP_NOINLINE int win64_dispatch_dynamic(CPU*c,uint64_t t){record(3);if(dinput(c,t)||openal(c,t)||shader(c,t)||steam(c,t)||d3d(c,t))return 1;if(t<UINT64_C(0xe5e40000)||t-UINT64_C(0xe5e40000)>=2048*8)return 0;if(t&7)fail(73,"dynamic alignment");return win64_dispatch_import(c,"fixture-export","fixture-name");}
static FP_NOINLINE fn_t frostpunk_payload_lookup(uint64_t t){record(9);if(t<UINT64_C(0x180000000)||t-UINT64_C(0x180000000)>=UINT64_C(0x32000))return NULL;if(!payload_ready)fail(72,"payload NX/source gate");if(!member(3,t))fail(72,"payload missing target");return payload_body;}
static FP_NOINLINE fn_t frostpunk_dependencies_lookup(uint64_t t){record(10);if(t<UINT64_C(0x1a0000000)||t-UINT64_C(0x1a0000000)>=UINT64_C(0x9d000))return NULL;if(!dependency_ready)fail(73,"dependency init gate");if(!member(4,t))fail(71,"dependency missing target");return dependency_body;}
#include "dispatch_baseline.inc"
#include "dispatch_candidate.inc"
typedef struct {CPU cpu;unsigned trace[128],count,body,depth;uint64_t calls;int code,error;} outcome;
static outcome invoke(int candidate,uint64_t target,unsigned seed,unsigned depth,uint64_t budget,int trace){
    memset(&working,(int)(seed*17+3),sizeof(working));working.rsp=(uintptr_t)&stack_words[32];working.mxcsr=0x1fa4;stack_words[32]=UINT64_C(0x140e02420);
    event_count=selected_body=0;fault_code=0;errno=2718;
    if(candidate){candidate_depth=depth;candidate_calls=budget==1?1:0;candidate_budget=budget;candidate_trace=trace;}
    else{baseline_depth=depth;baseline_calls=budget==1?1:0;baseline_budget=budget;baseline_trace=trace;}
    if(!setjmp(fault)){if(candidate)candidate_dispatch(&working,target);else baseline_dispatch(&working,target);}
    outcome o={0};o.cpu=working;o.count=event_count;o.body=selected_body;o.code=fault_code;o.error=errno;memcpy(o.trace,events,event_count*sizeof(*events));
    o.depth=candidate?candidate_depth:baseline_depth;o.calls=candidate?candidate_calls:baseline_calls;return o;
}
static uint64_t case_count,fast_cases,slow_cases,failures,trace_checks;
static int eligible(uint64_t t){return t>=MAIN_BASE&&t<MAIN_END&&(t<OPENAL_MIN||t>OPENAL_MAX);}
static int compare(uint64_t t,unsigned seed,unsigned depth,uint64_t budget,int trace){
    outcome a=invoke(0,t,seed,depth,budget,trace),b=invoke(1,t,seed,depth,budget,trace);++case_count;
    int same=!memcmp(&a.cpu,&b.cpu,sizeof(CPU))&&a.code==b.code&&a.body==b.body&&a.depth==b.depth&&a.calls==b.calls&&a.error==2718&&b.error==2718;
#if !defined(FP_DISPATCH_BENCHMARK)
    unsigned expected[128],n=0;
    if(eligible(t)&&(member(0,t)||member(1,t)||(manual_ready&&member(2,t)))){for(unsigned i=0;i<a.count;++i)if(a.trace[i]>10)expected[n++]=a.trace[i];++fast_cases;}
    else if(eligible(t)&&!a.body&&a.code==71){expected[n++]=11;expected[n++]=12;expected[n++]=13;expected[n++]=14;memcpy(expected+n,a.trace,a.count*sizeof(*expected));n+=a.count;++slow_cases;}
    else{memcpy(expected,a.trace,a.count*sizeof(*expected));n=a.count;++slow_cases;}
    same=same&&n==b.count&&!memcmp(expected,b.trace,n*sizeof(*expected));++trace_checks;
#endif
    if(!same){if(failures++<8)fprintf(stderr,"DISPATCH_FAIL target=%" PRIx64 " audio=%d code=%d/%d body=%u/%u depth=%u/%u trace=%u/%u CPU=%d\n",t,audio_mode,a.code,b.code,a.body,b.body,a.depth,b.depth,a.count,b.count,memcmp(&a.cpu,&b.cpu,sizeof(CPU)));return 0;}return 1;
}
static int load(const char*path){FILE*f=fopen(path,"rb");if(!f)return 0;char magic[8];if(fread(magic,8,1,f)!=1||memcmp(magic,"FPNDISP1",8)||fread(counts,sizeof(counts),1,f)!=1)return 0;
    for(unsigned i=0;i<8;++i){tables[i]=malloc((size_t)counts[i]*8);if(!tables[i]||fread(tables[i],8,counts[i],f)!=counts[i])return 0;for(unsigned j=1;j<counts[i];++j)if(tables[i][j-1]>=tables[i][j])return 0;}if(fgetc(f)!=EOF)return 0;fclose(f);
    recomp_dispatch_count=counts[0];recomp_dispatch_table=malloc((size_t)counts[0]*sizeof(*recomp_dispatch_table));if(!recomp_dispatch_table)return 0;for(unsigned i=0;i<counts[0];++i)recomp_dispatch_table[i]=(recomp_entry_t){tables[0][i],canonical};stack_lo=(uintptr_t)stack_words;stack_hi=stack_lo+sizeof(stack_words);return 1;}
static uint64_t now_ns(void){
#if defined(_WIN32)
    LARGE_INTEGER n,f;QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return(uint64_t)((n.QuadPart/f.QuadPart)*UINT64_C(1000000000)+(n.QuadPart%f.QuadPart)*UINT64_C(1000000000)/f.QuadPart);
#else
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))exit(3);return(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
#endif
}
static void bench(unsigned iterations){
    uint64_t times[2][7];CPU c={0};c.rsp=(uintptr_t)&stack_words[32];c.mxcsr=0x1f80;baseline_budget=candidate_budget=0;baseline_trace=candidate_trace=0;
    for(unsigned round=0;round<7;++round)for(unsigned j=0;j<2;++j){unsigned candidate=(j+round)&1;uint64_t start=now_ns();for(unsigned i=0;i<iterations;++i){c.rsp=(uintptr_t)&stack_words[32];if(candidate)candidate_dispatch(&c,UINT64_C(0x140d871c0));else baseline_dispatch(&c,UINT64_C(0x140d871c0));}times[candidate][round]=now_ns()-start;}
    uint64_t expected=(uint64_t)iterations*14*(UINT64_C(0x140d871c0)+1);if(c.rax!=expected){fputs("dispatch benchmark checksum\n",stderr);exit(3);}
    printf("{\"success\":true,\"kind\":\"dispatch_namespace_model_microbenchmark\",\"iterations_per_round\":%u,\"rounds\":7,\"baseline_ns\":[",iterations);for(unsigned i=0;i<7;++i)printf("%s%" PRIu64,i?",":"",times[0][i]);printf("],\"candidate_ns\":[");for(unsigned i=0;i<7;++i)printf("%s%" PRIu64,i?",":"",times[1][i]);printf("],\"checksum\":%" PRIu64 ",\"checksum_verified\":true,\"tracing_compiled_out\":true,\"real_provider_API_bodies\":false,\"startup_or_FPS_claim\":false}\n",c.rax);
}
int main(int argc,char**argv){
    if(argc<2||!load(argv[1]))return 2;
#if defined(FP_DISPATCH_BENCHMARK)
    unsigned iterations=argc==3?(unsigned)strtoul(argv[2],NULL,10):1000000;if(!iterations||iterations>100000000)return 2;bench(iterations);return 0;
#else
    for(unsigned i=0;i<counts[0];++i)compare(tables[0][i],i,0,0,0);
    for(unsigned k=1;k<=4;++k)for(unsigned i=0;i<counts[k];++i)compare(tables[k][i],i+k,0,0,0);
    for(audio_mode=0;audio_mode<3;++audio_mode)for(unsigned i=0;i<counts[5];++i)compare(tables[5][i],i,0,0,0);audio_mode=0;
    for(unsigned ready=0;ready<2;++ready){manual_ready=(int)ready;for(unsigned i=0;i<counts[7];++i)for(unsigned j=0;j<64;++j)compare(tables[7][i],j,0,0,0);}manual_ready=1;
    uint64_t edges[]={MAIN_BASE-1,MAIN_BASE,MAIN_END-1,MAIN_END,OPENAL_MIN-1,OPENAL_MIN,OPENAL_MAX,OPENAL_MAX+1,0,UINT64_MAX,UINT64_C(0xe5ed0000),UINT64_C(0xe5ef0000)};
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);++i)compare(edges[i],i,0,0,0);
    const uint64_t spaces[][2]={{HLE_BASE,HLE_COUNT*8},{0xe5e40000,2048*8},{0xe5e60000,92*8},{0xe5e80000,185*8},{0xe5ea0000,22*8},{0xe5f00000,54*256*8},{0xe6000000,7*256*8},{0xe7000000,11*8},{0xe7000100,32*8}};
    for(unsigned i=0;i<sizeof(spaces)/sizeof(*spaces);++i){uint64_t b=spaces[i][0],n=spaces[i][1];for(unsigned j=0;j<n/8;++j)compare(b+j*8,j,0,0,0);compare(b-1,i,0,0,0);compare(b+1,i,0,0,0);compare(b+n,i,0,0,0);}
    payload_ready=dependency_ready=0;compare(tables[3][0],1,0,0,0);compare(tables[4][0],2,0,0,0);payload_ready=dependency_ready=1;
    compare(UINT64_C(0x180001001),1,0,0,0);compare(UINT64_C(0x1a0001001),1,0,0,0);
    import_enabled=0;compare(HLE_BASE,1,0,0,0);import_enabled=1;
    compare(tables[0][0],1,4096,0,0);compare(tables[0][0],1,0,1,0);compare(tables[0][0],1,0,0,1);
    printf("{\"success\":%s,\"cases\":%" PRIu64 ",\"full_CPU_cases\":%" PRIu64 ",\"trace_checks\":%" PRIu64 ",\"fast_hits\":%" PRIu64 ",\"slow_or_exception_cases\":%" PRIu64 ",\"failures\":%" PRIu64 ",\"all_main_entries\":%u,\"OpenAL_public_entries\":%u,\"OpenAL_typed_entries\":%u,\"original_bytes_executed\":false,\"real_provider_API_bodies\":false,\"startup_or_FPS_claim\":false}\n",failures?"false":"true",case_count,case_count,trace_checks,fast_cases,slow_cases,failures,counts[0],counts[5],counts[6]);return failures?1:0;
#endif
}
