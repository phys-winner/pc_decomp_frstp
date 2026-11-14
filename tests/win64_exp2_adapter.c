#include "win64_math.h"
#include <errno.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <xmmintrin.h>
#endif
#if defined(_MSC_VER)
#define LOCAL __declspec(thread)
#else
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
static LOCAL uint64_t handler;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return handler;}
typedef struct {uint64_t bits;uint32_t operation,mxcsr;int32_t initial_errno;uint32_t handler;uint64_t expected;uint32_t expected_mxcsr;int32_t expected_errno;uint32_t tolerance;} test_case;
_Static_assert(sizeof(test_case)==48,"case48");
static test_case *data;static size_t count;static unsigned long long worst;static unsigned exact;
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
static int one(const test_case*t,int verbose){
 CPU c,before,expected;memset(&c,0xa5,sizeof(c));c.xmm[0].u64[0]=t->bits;c.mxcsr=t->mxcsr;before=c;
 guest_errno=t->initial_errno;handler=t->handler;int native_errno=errno;errno=1379;uint64_t old[2],now[2];env_get(old);
 if(!win64_math_call(&c,"exp2"))return 0;env_get(now);int saved=errno;errno=native_errno;
 uint64_t value=c.xmm[0].u64[0],distance=value>t->expected?value-t->expected:t->expected-value;
 expected=before;expected.xmm[0].u64[0]=value;expected.mxcsr=t->expected_mxcsr;
 int ok=distance<=t->tolerance&&guest_errno==t->expected_errno&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(old,now,sizeof(old))&&saved==1379;
 if(verbose){if(!distance)++exact;if(distance>worst)worst=distance;}
 if(!ok&&verbose)fprintf(stderr,"EXP2_FAIL input=%llx mode=%x result=%llx expected=%llx ulp=%llu/%u csr=%x/%x errno=%d/%d CPU=%d hostenv=%d hosterrno=%d\n",(unsigned long long)t->bits,t->mxcsr,(unsigned long long)value,(unsigned long long)t->expected,(unsigned long long)distance,t->tolerance,c.mxcsr,t->expected_mxcsr,guest_errno,t->expected_errno,memcmp(&c,&expected,sizeof(c)),memcmp(old,now,sizeof(old)),saved);
 return ok;
}
static void *worker(void*arg){size_t offset=(size_t)(uintptr_t)arg;uint64_t old[2],wanted[2];env_get(old);wanted[0]=old[0];wanted[1]=old[1];
#if defined(__aarch64__)
 wanted[0]=(old[0]&~(UINT64_C(3)<<22))|((offset&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
 wanted[0]=0x9f95u|((offset&3u)<<13);
#endif
 env_set(wanted);for(size_t i=0;i<2048;++i)if(!one(&data[(offset*997+i*37)%count],0)){env_set(old);return(void*)1;}env_set(old);return NULL;
}
int main(int argc,char**argv){
 if(argc<2)return 2;win64_math_self_test();
 if(!strcmp(argv[1],"--negative")){
  if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;
  if(!strcmp(argv[2],"invalid")){c.mxcsr&=~128u;c.xmm[0].u64[0]=UINT64_C(0x7ff0000000000001);}
  else if(!strcmp(argv[2],"precision")){c.mxcsr&=~4096u;c.xmm[0].u64[0]=UINT64_C(0x3fe0000000000000);}
  else if(!strcmp(argv[2],"denormal")){c.mxcsr&=~256u;c.xmm[0].u64[0]=1;}
  else if(!strcmp(argv[2],"underflow")){c.mxcsr&=~2048u;c.xmm[0].u64[0]=UINT64_C(0xc090c80000000000);}
  else if(!strcmp(argv[2],"overflow")){c.mxcsr&=~1024u;c.xmm[0].u64[0]=UINT64_C(0x4090000000000000);}
  else return 2;win64_math_call(&c,"exp2");return 99;
 }
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);if(bytes<=0||bytes%sizeof(test_case))return 4;count=(size_t)bytes/sizeof(test_case);data=malloc((size_t)bytes);if(!data||fread(data,(size_t)bytes,1,f)!=1)return 5;fclose(f);
 unsigned failures=0;for(size_t i=0;i<count;++i)if(!one(&data[i],1)&&++failures>=16)break;
 unsigned concurrent=0;
#if !defined(_WIN32)
 pthread_t threads[4];if(!failures){for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;for(unsigned i=0;i<4;++i){void*r;if(pthread_join(threads[i],&r)||r)++failures;}concurrent=8192;}
#endif
 printf("{\"success\":%s,\"cases\":%zu,\"bit_exact\":%u,\"max_ULP\":%llu,\"failures\":%u,\"concurrent_cases\":%u,\"full_CPU_RSP_canary\":true,\"host_FP_errno_preserved\":true}\n",failures?"false":"true",count,exact,worst,failures,concurrent);free(data);return failures?1:0;
}
