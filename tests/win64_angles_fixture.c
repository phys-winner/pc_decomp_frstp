#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
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
static LOCAL int guest_errno;static LOCAL uint64_t handler;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return handler;}
typedef struct{uint32_t y,x,result,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t kind,matherr_calls;} row;
_Static_assert(sizeof(row)==32,"angle row32");
static row *rows;static size_t count;static _Atomic unsigned max_ulp,failures;
static _Atomic unsigned long exact_calls;
static int import_math(CPU*c,const char*dll,const char*name){int crt=!strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll");
#include "angles_route.inc"
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
static int exponent(uint32_t bits){unsigned e=(bits>>23)&255u;if(e)return(int)e-127;int result=-126;uint32_t fraction=bits&0x7fffff;while(!(fraction&0x800000)){fraction<<=1;--result;}return result;}
static unsigned bound(const row*r){
    uint32_t y=r->y&0x7fffffff,x=r->x&0x7fffffff;
    if(r->kind)return y>=0x3f800000||y<0x38800000?0:2;
    if(r->mxcsr&64u){if(y<0x800000)y=0;if(x<0x800000)x=0;}
    if(!y||!x||y>=0x7f800000||x>=0x7f800000)return 0;
    int difference=exponent(y)-exponent(x);if(difference>26||difference < -26&&(r->x>>31)||difference < -13&&!(r->x>>31))return 0;
    return 2;
}
static int one(const row*r,size_t index){
    CPU c,expected;memset(&c,0xa7,sizeof(c));c.mxcsr=r->mxcsr;c.xmm[0].u32[0]=r->y;c.xmm[1].u32[0]=r->x;expected=c;expected.mxcsr=r->expected_mxcsr;if(index&1)expected.rsp+=8;
    guest_errno=1379;errno=2718;uint64_t before[2],after[2];environment(before);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    const char*name=r->kind?"asinf":"atan2f";int handled=index&1?import_math(&c,"api-ms-win-crt-math-l1-1-0.dll",name):win64_math_call(&c,name);
    environment(after);int native_errno=errno;uint32_t actual=c.xmm[0].u32[0];unsigned delta=actual>r->result?actual-r->result:r->result-actual;
    expected.xmm[0].u32[0]=actual;int okay=handled&&delta<=bound(r)&&!memcmp(&c,&expected,sizeof(c))&&guest_errno==r->expected_errno&&native_errno==2718&&!memcmp(before,after,sizeof(before));
#if defined(_WIN32)
    okay=okay&&GetLastError()==0x5a17;
#endif
    if(!okay&&atomic_fetch_add(&failures,1)<16)fprintf(stderr,"ANGLES_FAIL %s y=%08x x=%08x value=%08x expected=%08x ULP=%u/%u CSR=%x/%x errno=%d/%d host_errno=%d CPU=%d nativeFP=%d\n",name,r->y,r->x,actual,r->result,delta,bound(r),c.mxcsr,r->expected_mxcsr,guest_errno,r->expected_errno,native_errno,memcmp(&c,&expected,sizeof(c)),memcmp(before,after,sizeof(before)));
    unsigned previous=atomic_load(&max_ulp);while(delta>previous&&!atomic_compare_exchange_weak(&max_ulp,&previous,delta)){}if(!delta)atomic_fetch_add(&exact_calls,1);return okay;
}
#if !defined(_WIN32)
static void *worker(void*context){size_t seed=(size_t)(uintptr_t)context;uint64_t saved[2],wanted[2];environment(saved);wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((seed&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;set_environment(wanted);
    for(size_t i=0;i<2048;++i)if(!one(&rows[(seed*997+i*37)%count],i)){set_environment(saved);return(void*)1;}set_environment(saved);return NULL;}
#endif
int main(int argc,char**argv){
    if(argc<2)return 2;
    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.rip=0x141ae3276;c.xmm[0].u32[0]=c.xmm[1].u32[0]=0x3f800000;const char*name="atan2f",*reason=argv[2];
        if(!strcmp(reason,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(reason,"tiny_ratio"))c.xmm[0].u32[0]=0x00800000;
        else if(!strcmp(reason,"unmasked_precision"))c.mxcsr&=~4096u;
        else if(!strcmp(reason,"unmasked_invalid")){c.xmm[1].u32[0]=0x7f800001;c.mxcsr&=~128u;}
        else if(!strcmp(reason,"unmasked_denormal")){c.xmm[0].u32[0]=1;c.xmm[1].u32[0]=1;c.mxcsr&=~256u;}
        else if(!strcmp(reason,"unmasked_underflow")){c.xmm[1].u32[0]=0x7f800000;c.mxcsr&=~2048u;}
        else if(!strcmp(reason,"asin_reserved")){name="asinf";c.mxcsr|=0x10000;}
        else if(!strcmp(reason,"asin_unmasked_precision")){name="asinf";c.xmm[0].u32[0]=0;c.mxcsr&=~4096u;}
        else if(!strcmp(reason,"asin_unmasked_invalid")){name="asinf";c.xmm[0].u32[0]=0x7f800000;c.mxcsr&=~128u;}
        else if(!strcmp(reason,"asin_handler")){name="asinf";c.xmm[0].u32[0]=0x40000000;handler=0x1234;}
        else return 2;win64_math_call(&c,name);return 99;
    }
    win64_math_self_test();FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);if(bytes<=0||bytes%sizeof(row))return 4;count=(size_t)bytes/sizeof(row);rows=malloc((size_t)bytes);if(!rows||fread(rows,(size_t)bytes,1,f)!=1)return 5;fclose(f);
    uint64_t saved[2],wanted[2];environment(saved);unsigned atan_cases=0,asin_cases=0;
    for(size_t i=0;i<count;++i){
#if defined(__aarch64__)
        wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=UINT32_C(0x9fd5)|((i&3u)<<13);wanted[1]=0;
#endif
        set_environment(wanted);if(rows[i].kind)++asin_cases;else ++atan_cases;if(!one(&rows[i],i)&&atomic_load(&failures)>=16)break;
    }set_environment(saved);unsigned concurrent=0;
#if !defined(_WIN32)
    if(!atomic_load(&failures)){pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;for(unsigned i=0;i<4;++i){void*result;if(pthread_join(threads[i],&result)||result)atomic_fetch_add(&failures,1);}concurrent=8192;}
#endif
    CPU c;memset(&c,0xa5,sizeof(c));CPU old=c;if(win64_math_call(&c,"unimplemented_angle")||memcmp(&c,&old,sizeof(c)))atomic_fetch_add(&failures,1);
    printf("{\"success\":%s,\"cases\":%zu,\"atan2f_cases\":%u,\"asinf_cases\":%u,\"bit_exact_calls\":%lu,\"max_ULP\":%u,\"failures\":%u,\"concurrent_cases\":%u,\"fullCPU_MXCSR_errno_nativeFP\":true,\"single_import_pop\":true}\n",atomic_load(&failures)?"false":"true",count,atan_cases,asin_cases,atomic_load(&exact_calls),atomic_load(&max_ulp),atomic_load(&failures),concurrent);free(rows);return atomic_load(&failures)?1:0;
}
