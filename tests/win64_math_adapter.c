#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
static int guest_errno;
static uint64_t guest_matherr;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return guest_matherr;}
typedef struct {uint64_t bits;uint32_t is_double,mxcsr;int32_t initial_errno;uint32_t handler;uint64_t expected_bits;uint32_t expected_mxcsr;int32_t expected_errno;} math_case;
_Static_assert(sizeof(math_case)==40,"math case byte layout");
static void host_environment(uint64_t*out){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#elif defined(_WIN32)
    out[0]=_mm_getcsr();out[1]=0;
#else
#error Untested host fixture
#endif
}
static int run_case(const math_case*t,unsigned index){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.mxcsr=t->mxcsr;
    if(t->is_double)c.xmm[0].u64[0]=t->bits;else c.xmm[0].u32[0]=(uint32_t)t->bits;
    before=c;guest_errno=t->initial_errno;guest_matherr=t->handler?UINT64_C(0x140123450):0;
    uint64_t old_env[2],new_env[2];host_environment(old_env);
    if(!win64_math_call(&c,t->is_double?"sqrt":"sqrtf"))abort();host_environment(new_env);
    uint64_t result=t->is_double?c.xmm[0].u64[0]:c.xmm[0].u32[0];uint32_t mxcsr=c.mxcsr;
    c.xmm[0]=before.xmm[0];c.mxcsr=before.mxcsr;
    if(result!=t->expected_bits||mxcsr!=t->expected_mxcsr||guest_errno!=t->expected_errno||memcmp(&c,&before,sizeof(c))||memcmp(old_env,new_env,sizeof(old_env))){
        fprintf(stderr,"MATH_CASE_FAIL %u %s input=%llx mode=%x result=%llx expected=%llx mxcsr=%x expected=%x errno=%d expected=%d env=%llx/%llx -> %llx/%llx\n",index,t->is_double?"sqrt":"sqrtf",(unsigned long long)t->bits,t->mxcsr,(unsigned long long)result,(unsigned long long)t->expected_bits,mxcsr,t->expected_mxcsr,guest_errno,t->expected_errno,(unsigned long long)old_env[0],(unsigned long long)old_env[1],(unsigned long long)new_env[0],(unsigned long long)new_env[1]);return 0;
    }return 1;
}
static uint64_t integer_root(uint64_t bits,int wide){
    unsigned fraction=wide?52:23,bias=wide?1023:127;uint64_t implicit=UINT64_C(1)<<fraction;
    int exponent=(int)(bits>>fraction)-(int)bias;uint64_t significand=(bits&(implicit-1))|implicit;
    if(exponent%2){significand<<=1;--exponent;}
    uint64_t root=0,remainder=0;
    for(int i=(int)fraction;i>=0;--i){unsigned position=(unsigned)i*2,pair=0;
        for(unsigned j=0;j<2;++j){unsigned bit=position+j;if(bit>=fraction&&bit-fraction<64)pair|=(unsigned)((significand>>(bit-fraction))&1)<<j;}
        remainder=(remainder<<2)|pair;uint64_t trial=(root<<2)|1;root<<=1;if(remainder>=trial){remainder-=trial;root|=1;}
    }
    if(remainder>root)++root;if(root==(implicit<<1)){root>>=1;exponent+=2;}
    return((uint64_t)(exponent/2+(int)bias)<<fraction)|(root&(implicit-1));
}
static uint64_t raw_root(uint64_t bits,int wide){
#if defined(__aarch64__)
    if(wide){double x,y;memcpy(&x,&bits,8);__asm__ volatile("fsqrt %d0,%d1":"=w"(y):"w"(x));memcpy(&bits,&y,8);}
    else{uint32_t low=(uint32_t)bits;float x,y;memcpy(&x,&low,4);__asm__ volatile("fsqrt %s0,%s1":"=w"(y):"w"(x));memcpy(&low,&y,4);bits=low;}
#else
    if(wide){double x,y;memcpy(&x,&bits,8);__m128d v=_mm_set_sd(x);y=_mm_cvtsd_f64(_mm_sqrt_sd(v,v));memcpy(&bits,&y,8);}
    else{uint32_t low=(uint32_t)bits;float x,y;memcpy(&x,&low,4);y=_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));memcpy(&low,&y,4);bits=low;}
#endif
    return bits;
}
static uint64_t time_ns(void){
#if defined(_WIN32)
    LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return(uint64_t)((double)t.QuadPart*(1e9/(double)f.QuadPart));
#else
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
#endif
}
static void benchmark(unsigned iterations,int wide){
    const uint64_t base=wide?UINT64_C(0x3ff0000000000000):UINT64_C(0x3f800000),mask=wide?UINT64_C(0xfffffffffffff):UINT64_C(0x7fffff);
    CPU c={0};c.mxcsr=0x1f80;uint64_t sums[3]={0},elapsed[3];guest_matherr=0;
    for(unsigned mode=0;mode<3;++mode){uint64_t start=time_ns();
        for(unsigned i=0;i<iterations;++i){uint64_t bits=base|((UINT64_C(2654435761)*i)&mask),result;
            if(mode==0){if(wide)c.xmm[0].u64[0]=bits;else c.xmm[0].u32[0]=(uint32_t)bits;win64_math_call(&c,wide?"sqrt":"sqrtf");result=wide?c.xmm[0].u64[0]:c.xmm[0].u32[0];}
            else result=mode==1?raw_root(bits,wide):integer_root(bits,wide);sums[mode]+=result;
        }elapsed[mode]=time_ns()-start;
    }
    if(sums[0]!=sums[1]||sums[0]!=sums[2]){fputs("MATH_BENCHMARK_CHECKSUM_FAIL\n",stderr);abort();}
    printf("{\"precision\":\"%s\",\"iterations\":%u,\"runtime_ns\":%llu,\"raw_native_ns\":%llu,\"integer_reference_ns\":%llu,\"checksum\":\"%llx\"}",wide?"binary64":"binary32",iterations,(unsigned long long)elapsed[0],(unsigned long long)elapsed[1],(unsigned long long)elapsed[2],(unsigned long long)sums[0]);
}
#if defined(MATH_FIXTURE_MAIN)
int main(int argc,char**argv){
    if(argc<2)return 2;guest_matherr=0;win64_math_self_test();
    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;
        if(!strcmp(argv[2],"matherr_float")){guest_matherr=1;c.xmm[0].u32[0]=0xbf800000;win64_math_call(&c,"sqrtf");}
        else if(!strcmp(argv[2],"matherr_double")){guest_matherr=1;c.xmm[0].u64[0]=UINT64_C(0xbff0000000000000);win64_math_call(&c,"sqrt");}
        else if(!strcmp(argv[2],"unmasked_invalid")){c.mxcsr&=~128u;c.xmm[0].u32[0]=0xbf800000;win64_math_call(&c,"sqrtf");}
        else if(!strcmp(argv[2],"unmasked_precision")){c.mxcsr&=~4096u;c.xmm[0].u32[0]=0x40000000;win64_math_call(&c,"sqrtf");}
        else if(!strcmp(argv[2],"unmasked_denormal")){c.mxcsr&=~256u;c.xmm[0].u64[0]=1;win64_math_call(&c,"sqrt");}else return 2;
        return 99;
    }
    FILE*input=fopen(argv[1],"rb");if(!input)return 3;math_case t;unsigned count=0,failures=0;
    while(fread(&t,sizeof(t),1,input)==1){if(!run_case(&t,count))++failures;++count;if(failures>=12)break;}
    fclose(input);guest_matherr=0;printf("{\"cases\":%u,\"failures\":%u,\"success\":%s,\"game_boot_proven\":false,\"benchmarks\":[",count,failures,failures?"false":"true");
    if(!failures){benchmark(100000,0);putchar(',');benchmark(1000000,0);putchar(',');benchmark(100000,1);putchar(',');benchmark(1000000,1);}puts("]}");return failures?1:0;
}
#endif
