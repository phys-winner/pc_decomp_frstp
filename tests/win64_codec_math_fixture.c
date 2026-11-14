#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
void win64_stdio_set_errno(int v){guest_errno=v;}
uint64_t win64_math_error_handler(void){return 0;}
static const char *names[]={"log","cos","sin"};
typedef struct {uint64_t x,result;uint32_t kind,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t float_result,consumer;} row;
_Static_assert(sizeof(row)==40,"codec row40");
static row *rows;static size_t count;
static uint64_t worst[3][4],exact[3];static unsigned consumers,narrowed;
static int import_math(CPU *c,const char *dll,const char *name){
    int crt=!strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll");
#include "codec_route.inc"
    return 0;
}
static void environment(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void set_environment(const uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static void guest_environment(uint32_t mxcsr){
#if defined(__aarch64__)
    unsigned r=(mxcsr>>13)&3u;uint64_t mode=((uint64_t)(((r&1u)<<1)|((r&2u)>>1)))<<22;
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(mode):"memory");
#else
    _mm_setcsr(0x1f80u|(mxcsr&0x6000u));
#endif
}
static uint32_t narrow(uint64_t bits,uint32_t csr){
    uint64_t saved[2];environment(saved);guest_environment(csr);double x;float y;uint32_t result;memcpy(&x,&bits,8);
#if defined(__aarch64__)
    __asm__ volatile("fcvt %s[out],%d[in]":[out]"=w"(y):[in]"w"(x):"memory");
#else
    y=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(x)));
#endif
    memcpy(&result,&y,4);set_environment(saved);return result;
}
static uint32_t log_consumer(const row *r,uint64_t result){
    CPU c={0};c.mxcsr=r->mxcsr;c.xmm[0].u64[0]=UINT64_C(0x4000000000000000);win64_math_call(&c,"log");
    double numerator,denominator,sum,half=0.5;memcpy(&numerator,&result,8);memcpy(&denominator,&c.xmm[0].u64[0],8);
    uint64_t saved[2];environment(saved);guest_environment(r->mxcsr);
#if defined(__aarch64__)
    __asm__ volatile("fdiv %d[out],%d[a],%d[b]\n\tfadd %d[out],%d[out],%d[h]"
        :[out]"=&w"(sum):[a]"w"(numerator),[b]"w"(denominator),[h]"w"(half):"memory");
#else
    sum=_mm_cvtsd_f64(_mm_add_sd(_mm_div_sd(_mm_set_sd(numerator),_mm_set_sd(denominator)),_mm_set_sd(half)));
#endif
    memcpy(&c.xmm[0].u64[0],&sum,8);win64_math_call(&c,"floor");set_environment(saved);
    uint64_t bits=c.xmm[0].u64[0];if(!(bits&UINT64_C(0x7fffffffffffffff)))return 0;
    unsigned exponent=(unsigned)(bits>>52)-1023u;
    return(uint32_t)(((bits&UINT64_C(0xfffffffffffff))|UINT64_C(0x10000000000000))>>(52u-exponent));
}
static uint64_t ordered(uint64_t x){return x>>63?~x:x^UINT64_C(0x8000000000000000);}
static int one(const row *r,size_t index,int statistics){
    if(r->kind>=3)return 0;
    CPU c,expected;memset(&c,0xa7,sizeof(c));c.mxcsr=r->mxcsr;c.xmm[0].u64[0]=r->x;expected=c;
    guest_errno=1379;errno=2718;uint64_t before[2],after[2];environment(before);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=(index&1)?import_math(&c,"api-ms-win-crt-math-l1-1-0.dll",names[r->kind]):win64_math_call(&c,names[r->kind]);
    environment(after);int native_errno=errno;uint64_t result=c.xmm[0].u64[0];
    uint64_t a=ordered(result),b=ordered(r->result),distance=a>b?a-b:b-a;
    expected.xmm[0].u64[0]=result;expected.mxcsr=r->expected_mxcsr;if(index&1)expected.rsp+=8;
    int okay=handled&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(before,after,sizeof(before))&&native_errno==2718&&guest_errno==r->expected_errno;
#if defined(_WIN32)
    okay=okay&&GetLastError()==0x5a17;
#endif
    uint32_t got_float=narrow(result,r->mxcsr);okay=okay&&got_float==r->float_result;
    unsigned mode=(r->mxcsr>>13)&3u;okay=okay&&distance<=4;
    if(!(r->x&UINT64_C(0x7fffffffffffffff))||(!r->kind&&r->x==UINT64_C(0x3ff0000000000000)))okay=okay&&result==r->result;
    uint32_t consumer_result=0;if(!r->kind){consumer_result=log_consumer(r,result);okay=okay&&consumer_result==r->consumer;}
    if(statistics){if(!distance)++exact[r->kind];if(distance>worst[r->kind][mode])worst[r->kind][mode]=distance;++narrowed;if(!r->kind)++consumers;}
    if(!okay)fprintf(stderr,"CODEC_MATH_FAIL name=%s x=%016llx value=%016llx expected=%016llx ULP=%llu CSR=%x/%x float=%08x/%08x consumer=%u/%u errno=%d/%d host_errno=%d CPU=%d hostFP=%d\n",
        names[r->kind],(unsigned long long)r->x,(unsigned long long)result,(unsigned long long)r->result,(unsigned long long)distance,c.mxcsr,r->expected_mxcsr,
        got_float,r->float_result,consumer_result,r->consumer,guest_errno,r->expected_errno,native_errno,memcmp(&c,&expected,sizeof(c)),memcmp(before,after,sizeof(before)));
    set_environment(before);return okay;
}
#if !defined(_WIN32)
static void *worker(void *context){
    size_t seed=(size_t)(uintptr_t)context;uint64_t saved[2],wanted[2];environment(saved);wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((seed&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;set_environment(wanted);
    for(size_t i=0;i<2048;++i)if(!one(&rows[(seed*997+i*37)%count],i,0)){set_environment(saved);return(void*)1;}
    set_environment(saved);return NULL;
}
#endif
int main(int argc,char **argv){
    if(argc<2)return 2;
    if(!strcmp(argv[1],"--negative")){
        if(argc!=4)return 2;unsigned kind=!strcmp(argv[2],"log")?0:!strcmp(argv[2],"cos")?1:!strcmp(argv[2],"sin")?2:3;if(kind>2)return 2;
        CPU c={0};c.mxcsr=0x1f80;c.rip=UINT64_C(0x141c5da9c);c.xmm[0].u64[0]=kind?UINT64_C(0x3ff0000000000000):UINT64_C(0x4000000000000000);
        const char *reason=argv[3];if(!strcmp(reason,"precision"))c.mxcsr&=~4096u;
        else if(!strcmp(reason,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(reason,"negative"))c.xmm[0].u64[0]|=UINT64_C(0x8000000000000000);
        else if(!strcmp(reason,"NaN"))c.xmm[0].u64[0]=UINT64_C(0x7ff8000000000001);
        else if(!strcmp(reason,"infinity"))c.xmm[0].u64[0]=UINT64_C(0x7ff0000000000000);
        else if(!strcmp(reason,"fraction"))c.xmm[0].u64[0]=kind?UINT64_C(0x3ff0000000000001):UINT64_C(0x3ff8000000000000);
        else if(!strcmp(reason,"large"))c.xmm[0].u64[0]=kind?UINT64_C(0x4014000000000000):UINT64_C(0x41f0000000000000);
        else if(!strcmp(reason,"tiny"))c.xmm[0].u64[0]=kind?UINT64_C(0x3f10000000000000):0;
        else if(!strcmp(reason,"round_down"))c.mxcsr|=0x2000;
        else if(!strcmp(reason,"round_up"))c.mxcsr|=0x4000;
        else if(!strcmp(reason,"round_zero"))c.mxcsr|=0x6000;
        else return 2;win64_math_call(&c,names[kind]);return 99;
    }
    win64_math_self_test();FILE *input=fopen(argv[1],"rb");if(!input)return 3;fseek(input,0,SEEK_END);long bytes=ftell(input);rewind(input);
    if(bytes<=0||bytes%sizeof(row))return 4;count=(size_t)bytes/sizeof(row);rows=malloc((size_t)bytes);if(!rows||fread(rows,(size_t)bytes,1,input)!=1)return 5;fclose(input);
    unsigned failures=0;uint64_t saved[2],wanted[2];environment(saved);
    for(size_t i=0;i<count;++i){
#if defined(__aarch64__)
        wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=UINT32_C(0x9fd5)|((i&3u)<<13);wanted[1]=0;
#endif
        set_environment(wanted);if(!one(&rows[i],i,1)&&++failures>=16)break;
    }set_environment(saved);unsigned concurrent=0;
#if !defined(_WIN32)
    if(!failures){pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;
        for(unsigned i=0;i<4;++i){void *result;if(pthread_join(threads[i],&result)||result)++failures;}concurrent=8192;}
#endif
    printf("{\"success\":%s,\"cases\":%zu,\"failures\":%u,\"log2N_consumers\":%u,\"exact_float_consumers\":%u,\"concurrent_cases\":%u,\"bit_exact\":[%llu,%llu,%llu],\"max_ULP_by_kind_mode\":[[%llu,%llu,%llu,%llu],[%llu,%llu,%llu,%llu],[%llu,%llu,%llu,%llu]],\"full_CPU_import_pop\":true,\"exact_guest_MXCSR\":true,\"host_FP_errno_preserved\":true,\"general_math_parity\":false}\n",
        failures?"false":"true",count,failures,consumers,narrowed,concurrent,(unsigned long long)exact[0],(unsigned long long)exact[1],(unsigned long long)exact[2],
        (unsigned long long)worst[0][0],(unsigned long long)worst[0][1],(unsigned long long)worst[0][2],(unsigned long long)worst[0][3],
        (unsigned long long)worst[1][0],(unsigned long long)worst[1][1],(unsigned long long)worst[1][2],(unsigned long long)worst[1][3],
        (unsigned long long)worst[2][0],(unsigned long long)worst[2][1],(unsigned long long)worst[2][2],(unsigned long long)worst[2][3]);
    free(rows);return failures?1:0;
}
