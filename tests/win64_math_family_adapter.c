#include "win64_math.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <xmmintrin.h>
#endif
static int guest_errno;static uint64_t guest_matherr;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return guest_matherr;}
static const char*names[]={"log2l","log2","log2f","floor","floorf","ceil","ceilf","round","roundf"};
typedef struct {uint64_t bits;uint32_t operation,mxcsr;int32_t initial_errno;uint32_t handler;uint64_t expected_bits;uint32_t expected_mxcsr;int32_t expected_errno;uint32_t max_ulp;} test_case;
_Static_assert(sizeof(test_case)==48,"test case protocol");
static void host_env(uint64_t*out){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static uint64_t order(uint64_t bits,int single){uint64_t sign=single?UINT64_C(0x80000000):UINT64_C(0x8000000000000000),mask=single?UINT64_C(0xffffffff):UINT64_MAX;return bits&sign?(~bits&mask):(bits|sign);}
int main(int argc,char**argv){
    if(argc<2)return 2;guest_matherr=0;win64_math_self_test();
    if(!strcmp(argv[1],"--negative")){
        CPU c={0};c.mxcsr=0x1f80;
        if(argc!=3)return 2;
        if(!strcmp(argv[2],"log2_matherr_domain")){guest_matherr=1;c.xmm[0].u64[0]=UINT64_C(0xbff0000000000000);win64_math_call(&c,"log2");}
        else if(!strcmp(argv[2],"log2_matherr_pole")){guest_matherr=1;c.xmm[0].u64[0]=0;win64_math_call(&c,"log2");}
        else if(!strcmp(argv[2],"log2l_unmasked_invalid")){c.mxcsr&=~128u;c.xmm[0].u64[0]=UINT64_C(0xbff0000000000000);win64_math_call(&c,"log2l");}
        else if(!strcmp(argv[2],"log2f_unmasked_invalid")){c.mxcsr&=~128u;c.xmm[0].u32[0]=0x7f812345;win64_math_call(&c,"log2f");}
        else if(!strcmp(argv[2],"log2l_unmasked_pole")){c.mxcsr&=~512u;c.xmm[0].u64[0]=0;win64_math_call(&c,"log2l");}
        else if(!strcmp(argv[2],"log2l_unmasked_precision")){c.mxcsr&=~4096u;c.xmm[0].u64[0]=UINT64_C(0x3ff8000000000000);win64_math_call(&c,"log2l");}else return 2;
        return 99;
    }
    FILE*input=fopen(argv[1],"rb");if(!input)return 3;test_case t;unsigned count=0,failures=0,exact=0;uint64_t worst[9]={0};
    while(fread(&t,sizeof(t),1,input)==1){
        if(t.operation>=9)return 4;int single=t.operation==2||t.operation==4||t.operation==6||t.operation==8;
        CPU c,before;memset(&c,0xa5,sizeof(c));c.mxcsr=t.mxcsr;if(single)c.xmm[0].u32[0]=(uint32_t)t.bits;else c.xmm[0].u64[0]=t.bits;before=c;
        guest_errno=t.initial_errno;guest_matherr=t.handler;uint64_t old[2],now[2];host_env(old);
        if(!win64_math_call(&c,names[t.operation]))return 5;host_env(now);
        uint64_t result=single?c.xmm[0].u32[0]:c.xmm[0].u64[0],a=order(result,single),b=order(t.expected_bits,single),distance=a>b?a-b:b-a;
        uint32_t mxcsr=c.mxcsr;c.xmm[0]=before.xmm[0];c.mxcsr=before.mxcsr;
        if(!distance)++exact;if(distance>worst[t.operation])worst[t.operation]=distance;
        if(distance>t.max_ulp||mxcsr!=t.expected_mxcsr||guest_errno!=t.expected_errno||memcmp(&c,&before,sizeof(c))||memcmp(old,now,sizeof(old))){
            fprintf(stderr,"MATH_FAMILY_FAIL %u %s input=%llx mode=%x result=%llx expected=%llx ULP=%llu limit=%u mxcsr=%x expected=%x errno=%d expected=%d env=%llx/%llx -> %llx/%llx\n",count,names[t.operation],(unsigned long long)t.bits,t.mxcsr,(unsigned long long)result,(unsigned long long)t.expected_bits,(unsigned long long)distance,t.max_ulp,mxcsr,t.expected_mxcsr,guest_errno,t.expected_errno,(unsigned long long)old[0],(unsigned long long)old[1],(unsigned long long)now[0],(unsigned long long)now[1]);++failures;
        }
        ++count;if(failures>=12)break;
    }
    fclose(input);printf("{\"cases\":%u,\"failures\":%u,\"success\":%s,\"bit_exact_cases\":%u,\"max_ULP_by_operation\":{",count,failures,failures?"false":"true",exact);
    for(unsigned i=0;i<9;++i)printf("%s\"%s\":%llu",i?",":"",names[i],(unsigned long long)worst[i]);puts("},\"game_boot_proven\":false}");return failures?1:0;
}
