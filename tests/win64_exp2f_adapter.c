/* Exact normal-result integer-exponent UCRT/port exp2f comparison. No original game instruction is executed. */
#include "win64_math.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <xmmintrin.h>
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
static LOCAL uint64_t handler;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return handler;}
#include "exp2f_route.inc"
typedef struct {uint64_t bits;uint32_t operation,mxcsr;int32_t initial_errno;uint32_t handler;uint64_t expected;uint32_t expected_mxcsr;int32_t expected_errno;uint32_t tolerance,reserved;} test_case;
_Static_assert(sizeof(test_case)==48,"exp2f case48");
static test_case *data;static size_t count;static uint64_t exact,worst;
static void env_get(uint64_t out[2]){
#if defined(__aarch64__)
 __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
 out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void env_set(const uint64_t in[2]){
#if defined(__aarch64__)
 __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(in[0]),"r"(in[1]):"memory");
#else
 _mm_setcsr((unsigned)in[0]);
#endif
}
static uint32_t ordered(uint32_t b){return b&UINT32_C(0x80000000)?~b:b|UINT32_C(0x80000000);}
static int one(const test_case*t,int verbose){
 CPU c,before,expected;memset(&c,0xa5,sizeof(c));c.xmm[0].u32[0]=(uint32_t)t->bits;c.mxcsr=t->mxcsr;before=c;
 guest_errno=t->initial_errno;handler=t->handler;int saved_errno=errno;errno=1379;uint64_t old[2],now[2];env_get(old);
 int handled=t->reserved?exp2f_import(&c,"api-ms-win-crt-math-l1-1-0.dll","exp2f"):win64_math_call(&c,"exp2f");env_get(now);int native_errno=errno;errno=saved_errno;
 uint32_t bits=c.xmm[0].u32[0],a=ordered(bits),b=ordered((uint32_t)t->expected);uint64_t distance=a>b?(uint64_t)a-b:(uint64_t)b-a;
 expected=before;expected.xmm[0].u32[0]=bits;expected.mxcsr=t->expected_mxcsr;if(t->reserved)expected.rsp+=8;
 int okay=handled&&distance<=t->tolerance&&guest_errno==t->expected_errno&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(old,now,sizeof(old))&&native_errno==1379;
 if(verbose){if(!distance)++exact;if(distance>worst)worst=distance;}
 if(!okay&&verbose)fprintf(stderr,"EXP2F_FAIL input=%08x mode=%x result=%08x expected=%08x ULP=%llu/%u csr=%x/%x errno=%d/%d CPU=%d hostenv=%d hosterrno=%d\n",(unsigned)t->bits,t->mxcsr,bits,(unsigned)t->expected,(unsigned long long)distance,t->tolerance,c.mxcsr,t->expected_mxcsr,guest_errno,t->expected_errno,memcmp(&c,&expected,sizeof(c)),memcmp(old,now,sizeof(old)),native_errno);
 return okay;
}
#if !defined(_WIN32)
static void *worker(void*arg){size_t offset=(size_t)(uintptr_t)arg;uint64_t old[2],wanted[2];env_get(old);wanted[0]=old[0];wanted[1]=old[1];
 wanted[0]=(old[0]&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|((offset&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x08000015;
 env_set(wanted);for(size_t i=0;i<2048;++i)if(!one(&data[(offset*997+i*37)%count],0)){env_set(old);return(void*)1;}env_set(old);return NULL;
}
#endif
int main(int argc,char**argv){
 if(argc<2)return 2;win64_math_self_test();
 if(!strcmp(argv[1],"--negative")){
  if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.xmm[0].u32[0]=0x40000000;
  if(!strcmp(argv[2],"fractional"))c.xmm[0].u32[0]=0x3f000000;
  else if(!strcmp(argv[2],"negative_fractional"))c.xmm[0].u32[0]=0xbf000000;
  else if(!strcmp(argv[2],"subnormal"))c.xmm[0].u32[0]=1;
  else if(!strcmp(argv[2],"NaN"))c.xmm[0].u32[0]=0x7fc12345;
  else if(!strcmp(argv[2],"sNaN"))c.xmm[0].u32[0]=0x7f800001;
  else if(!strcmp(argv[2],"infinity"))c.xmm[0].u32[0]=0x7f800000;
  else if(!strcmp(argv[2],"low_range"))c.xmm[0].u32[0]=0xc2fe0000;
  else if(!strcmp(argv[2],"high_range"))c.xmm[0].u32[0]=0x43000000;
  else return 2;win64_math_call(&c,"exp2f");return 99;
 }
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);if(bytes<=0||bytes%sizeof(test_case))return 4;count=(size_t)bytes/sizeof(test_case);data=malloc((size_t)bytes);if(!data||fread(data,(size_t)bytes,1,f)!=1)return 5;fclose(f);
 unsigned failures=0;for(size_t i=0;i<count;++i)if(!one(&data[i],1)&&++failures>=16)break;
 unsigned concurrent=0;
#if !defined(_WIN32)
 pthread_t threads[4];if(!failures){for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;for(unsigned i=0;i<4;++i){void*r;if(pthread_join(threads[i],&r)||r)++failures;}concurrent=8192;}
#endif
 CPU c,b;memset(&c,0xa5,sizeof(c));b=c;if(win64_math_call(&c,"unknown_exp2f")||memcmp(&c,&b,sizeof(c)))return 7;
 printf("{\"success\":%s,\"cases\":%zu,\"bit_exact\":%llu,\"max_ULP\":%llu,\"failures\":%u,\"concurrent_cases\":%u,\"full_CPU_RSP_canary\":true,\"host_FP_errno_preserved\":true,\"original_game_executed\":false}\n",failures?"false":"true",count,(unsigned long long)exact,(unsigned long long)worst,failures,concurrent);free(data);return failures?1:0;
}
