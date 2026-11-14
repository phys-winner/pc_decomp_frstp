#include "win64_math.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return 0;}
static int pow_import(CPU *c,const char *dll,const char *name) {
    int crt=!strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll");
#include "pow_route.inc"
    return 0;
}
typedef struct {uint64_t x,y,result;uint32_t mxcsr,route;int32_t entries,dimension;uint32_t expected_mxcsr;int32_t expected_errno;} row;
_Static_assert(sizeof(row)==48,"pow row48");
static row *rows;static size_t count;static unsigned long long worst,exact,consumer_count;
static int exact_zero;
static void environment(uint64_t out[2]) {
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void set_environment(uint64_t value[2]) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static uint64_t truncated(uint64_t bits) {
    if(!bits)return 0;int exponent=(int)(bits>>52)-1023;
    return ((bits&UINT64_C(0xfffffffffffff))|UINT64_C(0x10000000000000))>>(52u-(unsigned)exponent);
}
static uint64_t integer_power(uint64_t value,unsigned dimension,uint64_t limit) {
    uint64_t result=1;
    for(unsigned i=0;i<dimension;++i){if(value&&result>limit/value)return limit+1;result*=value;}
    return result;
}
static int consumer(uint64_t result,uint64_t original,int dimension) {
    uint64_t value=truncated(result);
    for(unsigned steps=0;steps<8;++steps){
        uint64_t low=integer_power(value,(unsigned)dimension,original),high=integer_power(value+1,(unsigned)dimension,original);
        if(low<=original&&high>original)return (int)value;
        if(low>original){if(!value)return -1;--value;}else ++value;
    }return -1;
}
static int original_consumer(uint64_t result,int entries,int dimension) {
    uint32_t value=(uint32_t)truncated(result);
    for(unsigned steps=0;steps<8;++steps){
        uint32_t low=1,high=1;
        for(int i=0;i<dimension;++i){low*=value;high*=value+1;}
        int32_t signed_low,signed_high;memcpy(&signed_low,&low,4);memcpy(&signed_high,&high,4);
        if(signed_low<=entries&&signed_high>entries)return(int)value;
        value+=(signed_low>entries)?UINT32_MAX:1u;
    }return -1;
}
static int one(const row *r,int statistics) {
    CPU c,before,expected;memset(&c,0xa5,sizeof(c));c.mxcsr=r->mxcsr;c.xmm[0].u64[0]=r->x;c.xmm[1].u64[0]=r->y;before=c;
    guest_errno=1379;errno=2718;uint64_t old[2],now[2];environment(old);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=r->route?pow_import(&c,"api-ms-win-crt-math-l1-1-0.dll","pow"):win64_math_call(&c,"pow");
    environment(now);int native_errno=errno;uint64_t result=c.xmm[0].u64[0],distance=result>r->result?result-r->result:r->result-result;
    expected=before;expected.xmm[0].u64[0]=result;expected.mxcsr=r->expected_mxcsr;if(r->route)expected.rsp+=8;
    int okay=handled&&distance<=(exact_zero?0u:4u)&&guest_errno==r->expected_errno&&native_errno==2718&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(old,now,sizeof(old));
#if defined(_WIN32)
    okay=okay&&GetLastError()==0x5a17;
#endif
    /* Original IMUL is signed32 modulo arithmetic. Claim mathematical-root
     * parity only when both candidate start/next-step products cannot wrap. */
    if(r->dimension<=32 && r->entries>0 && r->entries<=16777215 &&
       integer_power(truncated(result)+2,(unsigned)r->dimension,INT32_MAX)<=INT32_MAX &&
       integer_power(truncated(r->result)+2,(unsigned)r->dimension,INT32_MAX)<=INT32_MAX){
        int got=consumer(result,(uint32_t)r->entries,r->dimension),reference=consumer(r->result,(uint32_t)r->entries,r->dimension);
        uint64_t lo=0,hi=(uint64_t)(uint32_t)r->entries+1;
        while(lo+1<hi){uint64_t mid=lo+(hi-lo)/2;if(integer_power(mid,(unsigned)r->dimension,(uint32_t)r->entries)<=(uint32_t)r->entries)lo=mid;else hi=mid;}
        okay=okay&&got==reference&&got==(int)lo&&original_consumer(result,r->entries,r->dimension)==got&&
             original_consumer(r->result,r->entries,r->dimension)==reference;if(statistics)++consumer_count;
    }
    if(statistics){if(!distance)++exact;if(distance>worst)worst=distance;}
    if(!okay)fprintf(stderr,"POW_FAIL x=%016llx y=%016llx value=%016llx expected=%016llx ULP=%llu CSR=%x/%x guest_errno=%d/%d host_errno=%d CPU=%d nativeFP=%d entries=%d dimension=%d\n",
        (unsigned long long)r->x,(unsigned long long)r->y,(unsigned long long)result,(unsigned long long)r->result,(unsigned long long)distance,
        c.mxcsr,r->expected_mxcsr,guest_errno,r->expected_errno,native_errno,memcmp(&c,&expected,sizeof(c)),memcmp(old,now,sizeof(old)),r->entries,r->dimension);
    return okay;
}
#if !defined(_WIN32)
static void *worker(void *opaque) {
    size_t seed=(size_t)(uintptr_t)opaque;uint64_t saved[2],wanted[2];environment(saved);wanted[0]=saved[0];wanted[1]=0x15;
    wanted[0]=(wanted[0]&~(UINT64_C(3)<<22))|((seed&3u)<<22)|(UINT64_C(1)<<24);set_environment(wanted);
    for(size_t i=0;i<2048;++i)if(!one(&rows[(seed*997+i*37)%count],0)){set_environment(saved);return(void*)1;}
    set_environment(saved);return NULL;
}
#endif
int main(int argc,char **argv) {
    if(argc<2)return 2;
    if(argc==3&&!strcmp(argv[1],"--exact-zero")){exact_zero=1;--argc;++argv;}
    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.rip=UINT64_C(0x141c5679f);
        c.xmm[0].u64[0]=UINT64_C(0x4000000000000000);c.xmm[1].u64[0]=UINT64_C(0x3fe0000000000000);
        const char *name=argv[2];
        if(!strcmp(name,"precision"))c.mxcsr&=~4096u;
        else if(!strcmp(name,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(name,"negative"))c.xmm[0].u64[0]|=UINT64_C(0x8000000000000000);
        else if(!strcmp(name,"negative_zero"))c.xmm[0].u64[0]=UINT64_C(0x8000000000000000);
        else if(!strcmp(name,"fraction"))c.xmm[0].u64[0]=UINT64_C(0x3ff8000000000000);
        else if(!strcmp(name,"subnormal"))c.xmm[0].u64[0]=1;
        else if(!strcmp(name,"NaN"))c.xmm[0].u64[0]=UINT64_C(0x7ff8000000000001);
        else if(!strcmp(name,"infinity"))c.xmm[1].u64[0]=UINT64_C(0x7ff0000000000000);
        else if(!strcmp(name,"negative_power"))c.xmm[1].u64[0]=UINT64_C(0xbfe0000000000000);
        else if(!strcmp(name,"large_base"))c.xmm[0].u64[0]=UINT64_C(0x41f0000000000000);
        else if(!strcmp(name,"large_power"))c.xmm[1].u64[0]=UINT64_C(0x4000000000000000);
        else if(!strcmp(name,"wide_power"))++c.xmm[1].u64[0];
        else return 2;win64_math_call(&c,"pow");return 99;
    }
    win64_math_self_test();FILE *input=fopen(argv[1],"rb");if(!input)return 3;fseek(input,0,SEEK_END);long bytes=ftell(input);rewind(input);
    if(bytes<=0||bytes%sizeof(row))return 4;count=(size_t)bytes/sizeof(row);rows=malloc((size_t)bytes);if(!rows||fread(rows,(size_t)bytes,1,input)!=1)return 5;fclose(input);
    unsigned failures=0;
    uint64_t saved_environment[2],wanted_environment[2];environment(saved_environment);
    for(size_t i=0;i<count;++i){
#if defined(__aarch64__)
        wanted_environment[0]=(saved_environment[0]&~(UINT64_C(3)<<22))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted_environment[1]=0x15;
#else
        wanted_environment[0]=UINT32_C(0x1f95)|((i&3u)<<13)|64u|32768u;wanted_environment[1]=0;
#endif
        set_environment(wanted_environment);
        if(!one(&rows[i],1)&&++failures>=12)break;
    }set_environment(saved_environment);
    unsigned concurrent=0;
#if !defined(_WIN32)
    if(!failures){pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;
        for(unsigned i=0;i<4;++i){void *result;if(pthread_join(threads[i],&result)||result)++failures;}concurrent=8192;}
#endif
    printf("{\"success\":%s,\"cases\":%zu,\"bit_exact\":%llu,\"max_ULP\":%llu,\"consumer_cases\":%llu,\"consumer_scope\":\"original signed32 loop with nonoverflow products\",\"concurrent_cases\":%u,\"failures\":%u,\"double_XMM0_XMM1\":true,\"full_CPU_single_pop\":true,\"host_FP_errno_preserved\":true,\"UCRT_MXCSR_exact\":true,\"game_boot_proven\":false}\n",
        failures?"false":"true",count,exact,worst,consumer_count,concurrent,failures);free(rows);return failures?1:0;
}
