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
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return 0;}
typedef struct {uint64_t x,result,rdx;uint32_t mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t word;} row;
_Static_assert(sizeof(row)==40,"ldexp row40");
static row *rows;static size_t count;
static int import_math(CPU *c,const char *dll,const char *name){
    int crt=!strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll");
#include "ldexp_route.inc"
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
static int one(const row *r,size_t index){
    CPU c,expected;memset(&c,0xa7,sizeof(c));c.mxcsr=r->mxcsr;c.xmm[0].u64[0]=r->x;c.rdx=r->rdx;expected=c;
    expected.xmm[0].u64[0]=r->result;expected.mxcsr=r->expected_mxcsr;if(index&1)expected.rsp+=8;
    guest_errno=1379;errno=2718;uint64_t before[2],after[2];environment(before);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=index&1?import_math(&c,"api-ms-win-crt-math-l1-1-0.dll","ldexp"):win64_math_call(&c,"ldexp");
    environment(after);int native_errno=errno;
    int okay=handled&&!memcmp(&c,&expected,sizeof(c))&&guest_errno==r->expected_errno&&native_errno==2718&&!memcmp(before,after,sizeof(before));
#if defined(_WIN32)
    okay=okay&&GetLastError()==0x5a17;
#endif
    if(!okay)fprintf(stderr,"LDEXP_FAIL word=%08x x=%016llx rdx=%016llx value=%016llx expected=%016llx CSR=%x/%x errno=%d/%d host_errno=%d CPU=%d hostFP=%d\n",
        r->word,(unsigned long long)r->x,(unsigned long long)r->rdx,(unsigned long long)c.xmm[0].u64[0],(unsigned long long)r->result,
        c.mxcsr,r->expected_mxcsr,guest_errno,r->expected_errno,native_errno,memcmp(&c,&expected,sizeof(c)),memcmp(before,after,sizeof(before)));
    return okay;
}
#if !defined(_WIN32)
static void *worker(void *context){
    size_t seed=(size_t)(uintptr_t)context;uint64_t saved[2],wanted[2];environment(saved);wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((seed&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;set_environment(wanted);
    for(size_t i=0;i<2048;++i)if(!one(&rows[(seed*997+i*37)%count],i)){set_environment(saved);return(void*)1;}
    set_environment(saved);return NULL;
}
#endif
int main(int argc,char **argv){
    if(argc<2)return 2;
    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.rip=UINT64_C(0x141c562ab);c.rdx=0xfffffcec;c.xmm[0].u64[0]=UINT64_C(0x4008000000000000);
        const char *reason=argv[2];if(!strcmp(reason,"scale_low"))c.rdx=0xfffffceb;
        else if(!strcmp(reason,"scale_high"))c.rdx=236;
        else if(!strcmp(reason,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(reason,"fraction"))c.xmm[0].u64[0]=UINT64_C(0x4004000000000000);
        else if(!strcmp(reason,"subnormal"))c.xmm[0].u64[0]=1;
        else if(!strcmp(reason,"NaN"))c.xmm[0].u64[0]=UINT64_C(0x7ff8000000000001);
        else if(!strcmp(reason,"infinity"))c.xmm[0].u64[0]=UINT64_C(0x7ff0000000000000);
        else if(!strcmp(reason,"large"))c.xmm[0].u64[0]=UINT64_C(0x4140000000000000);
        else return 2;win64_math_call(&c,"ldexp");return 99;
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
        set_environment(wanted);if(!one(&rows[i],i)&&++failures>=12)break;
    }set_environment(saved);unsigned concurrent=0;
#if !defined(_WIN32)
    if(!failures){pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;
        for(unsigned i=0;i<4;++i){void *result;if(pthread_join(threads[i],&result)||result)++failures;}concurrent=8192;}
#endif
    printf("{\"success\":%s,\"cases\":%zu,\"bit_exact\":%zu,\"failures\":%u,\"concurrent_cases\":%u,\"double_XMM0_int_lowRDX\":true,\"RDX_upper32_ignored\":true,\"full_CPU_import_pop\":true,\"exact_guest_MXCSR_errno\":true,\"native_FP_untouched\":true,\"general_ldexp_parity\":false}\n",
        failures?"false":"true",count,failures?0:count,failures,concurrent);free(rows);return failures?1:0;
}
