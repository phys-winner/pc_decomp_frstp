/* Reuse the unchanged paired oracle; add a bounded synthetic finite kernel. */
#define main fp_particle_semantic_main
#include "particle_scalar_fixture.c"
#undef main
#include <stdlib.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static float bench_left[256],bench_right[256];
static uint64_t now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER now,frequency;
    if(!QueryPerformanceCounter(&now)||!QueryPerformanceFrequency(&frequency))exit(2);
    return (uint64_t)((long double)now.QuadPart*1000000000.0L/frequency.QuadPart);
#else
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))exit(2);
    return (uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
#endif
}
#define KERNEL(NAME,ADD,MUL) \
static NOINLINE uint64_t NAME(unsigned iterations) { \
    uint64_t digest=UINT64_C(14695981039346656037); \
    for(unsigned repeat=0;repeat<iterations;++repeat)for(unsigned i=0;i<256;++i) { \
        float x=bench_left[i],y=bench_right[i]; \
        for(unsigned j=0;j<8;++j)x=ADD(MUL(x,y),bench_left[(i+1)&255]); \
        uint32_t bits;memcpy(&bits,&x,4);digest=(digest^bits)*UINT64_C(1099511628211); \
    } \
    return digest; \
}
KERNEL(baseline_kernel,frostpunk_addss,frostpunk_mulss)
KERNEL(candidate_kernel,fp_particle_addss,fp_particle_mulss)
#undef KERNEL

int main(int argc,char **argv) {
    if(argc==1)return fp_particle_semantic_main();
    if(argc!=3||strcmp(argv[1],"--benchmark"))return 2;
    char *end;unsigned long parsed=strtoul(argv[2],&end,10);
    if(!*argv[2]||*end||parsed<1||parsed>16384)return 2;
    unsigned iterations=(unsigned)parsed;uint64_t original[2];read_fp(original);
    for(unsigned i=0;i<256;++i) {
        uint32_t a=UINT32_C(0x3e800000)|(random32()&UINT32_C(0x007fffff));
        uint32_t b=UINT32_C(0x3f000000)|(random32()&UINT32_C(0x007fffff));
        memcpy(bench_left+i,&a,4);memcpy(bench_right+i,&b,4);
    }
    uint64_t expected=baseline_kernel(1);if(candidate_kernel(1)!=expected)return 1;
    uint64_t baseline_ns[5],candidate_ns[5],result=0;
    for(unsigned sample=0;sample<5;++sample) {
        uint64_t a,b,begin,end_time;
        if(sample&1) {
            write_fp(original);begin=now_ns();b=candidate_kernel(iterations);end_time=now_ns();candidate_ns[sample]=end_time-begin;
            write_fp(original);begin=now_ns();a=baseline_kernel(iterations);end_time=now_ns();baseline_ns[sample]=end_time-begin;
        } else {
            write_fp(original);begin=now_ns();a=baseline_kernel(iterations);end_time=now_ns();baseline_ns[sample]=end_time-begin;
            write_fp(original);begin=now_ns();b=candidate_kernel(iterations);end_time=now_ns();candidate_ns[sample]=end_time-begin;
        }
        if(a!=b)return 1;result=a;
    }
    write_fp(original);
    printf("{\"success\":true,\"kind\":\"synthetic-finite-kernel\",\"iterations\":%u,\"operations_per_sample\":%" PRIu64 ",\"samples\":[",iterations,(uint64_t)iterations*256*16);
    for(unsigned i=0;i<5;++i)printf("%s{\"order\":\"%s\",\"baseline_ns\":%" PRIu64 ",\"candidate_ns\":%" PRIu64 "}",i?",":"",i&1?"candidate-first":"baseline-first",baseline_ns[i],candidate_ns[i]);
    printf("],\"checksum\":\"%016" PRIx64 "\",\"production_changed\":false,\"gameplay_proven\":false}\n",result);
    return 0;
}
