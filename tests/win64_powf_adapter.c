#include "win64_math.h"
#include <errno.h>
#include <math.h>
#include <fenv.h>
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
#include "powf_route.inc"
typedef struct {uint64_t bits;uint32_t operation,mxcsr;int32_t initial_errno;uint32_t handler;uint64_t expected;uint32_t expected_mxcsr;int32_t expected_errno;uint32_t tolerance,original_width;} test_case;
_Static_assert(sizeof(test_case)==48,"case48");
static test_case *data;static size_t count,*accepted,accepted_count;static unsigned long long worst;static unsigned exact;
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
/* Admission is measured on this platform, independently of UCRT result bits.
 * In particular UCRT may round 2^-125 one ULP below the adapter's lower band
 * while Bionic returns its exact boundary. It must still be compared. */
static int admitted(const test_case*t){
 if(!(t->operation&2u))return 1;
 uint32_t xb=(uint32_t)t->bits,yb=(uint32_t)(t->bits>>32);
 if(!(yb&0x7fffffffu)||xb==0x3f800000u||yb==0x3f800000u||!(xb&0x7fffffffu))return 1;
 uint64_t before[2],after[2],wanted[2];env_get(before);wanted[0]=before[0];wanted[1]=0;
#if defined(__aarch64__)
 wanted[0]&=~((UINT64_C(1)<<24)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15));
#else
 wanted[0]=0x1f80;
#endif
 int error=errno;fenv_t saved;fegetenv(&saved);env_set(wanted);
 const int modes[4]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
 if(fesetround(modes[(t->mxcsr>>13)&3u])){fesetenv(&saved);env_set(before);errno=error;return -1;}
 feclearexcept(FE_ALL_EXCEPT);float x,y,z;memcpy(&x,&xb,4);memcpy(&y,&yb,4);
 float(*volatile native)(float,float)=powf;z=native(x,y);uint32_t result;memcpy(&result,&z,4);
 int flags=fetestexcept(FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW);
 fesetenv(&saved);env_set(before);errno=error;env_get(after);
 if(memcmp(before,after,sizeof(before)))return -1;
 return !flags&&result>=UINT32_C(0x01000000)&&result<=UINT32_C(0x7f000000);
}
static uint32_t trunc_float(uint32_t b){int e=(int)(b>>23)-127;uint32_t m=(b&0x7fffffu)|0x800000u;return e<0?0:e>31?UINT32_MAX:e<23?m>>(23-e):m<<(e-23);}
static uint32_t root_floor(uint32_t n){uint32_t lo=0,hi=65536;while(lo+1<hi){uint32_t m=lo+(hi-lo)/2;if((uint64_t)m*m<=n)lo=m;else hi=m;}return lo;}
static int consumer_match(uint32_t a,uint32_t b,uint32_t width){uint32_t an=trunc_float(a),bn=trunc_float(b);uint32_t ar=root_floor(an),br=root_floor(bn);return an==bn&&ar==br&&((uint64_t)ar*an==width)==((uint64_t)br*bn==width);}
static int one(const test_case*t,int verbose){
 int eligible=admitted(t);if(eligible<=0)return eligible<0?0:2;
 CPU c,before,expected;memset(&c,0xa5,sizeof(c));c.xmm[0].u32[0]=(uint32_t)t->bits;c.xmm[1].u32[0]=(uint32_t)(t->bits>>32);c.mxcsr=t->mxcsr;before=c;
 guest_errno=t->initial_errno;handler=t->handler;int native_errno=errno;errno=1379;uint64_t old[2],now[2];env_get(old);
 int handled=(t->operation&1u)?powf_import(&c,"api-ms-win-crt-math-l1-1-0.dll","powf"):win64_math_call(&c,"powf");env_get(now);int saved=errno;errno=native_errno;
 uint64_t value=c.xmm[0].u32[0],distance=value>t->expected?value-t->expected:t->expected-value;
 expected=before;expected.xmm[0].u32[0]=(uint32_t)value;expected.mxcsr=t->expected_mxcsr;if(t->operation&1u)expected.rsp+=8;
 int ok=handled&&(!t->original_width||consumer_match((uint32_t)value,(uint32_t)t->expected,t->original_width))&&distance<=t->tolerance&&guest_errno==t->expected_errno&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(old,now,sizeof(old))&&saved==1379;
 if(verbose){if(!distance)++exact;if(distance>worst)worst=distance;}
 if(!ok&&verbose)fprintf(stderr,"POWF_FAIL input=%llx mode=%x result=%llx expected=%llx ulp=%llu/%u csr=%x/%x errno=%d/%d CPU=%d hostenv=%d hosterrno=%d\n",(unsigned long long)t->bits,t->mxcsr,(unsigned long long)value,(unsigned long long)t->expected,(unsigned long long)distance,t->tolerance,c.mxcsr,t->expected_mxcsr,guest_errno,t->expected_errno,memcmp(&c,&expected,sizeof(c)),memcmp(old,now,sizeof(old)),saved);
 return ok;
}
static void *worker(void*arg){size_t offset=(size_t)(uintptr_t)arg;uint64_t old[2],wanted[2];env_get(old);wanted[0]=old[0];wanted[1]=old[1];
#if defined(__aarch64__)
 wanted[0]=(old[0]&~(UINT64_C(3)<<22))|((offset&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
 wanted[0]=0x9f95u|((offset&3u)<<13);
#endif
 env_set(wanted);for(size_t i=0;i<2048;++i)if(one(&data[accepted[(offset*997+i*37)%accepted_count]],0)!=1){env_set(old);return(void*)1;}env_set(old);return NULL;
}
int main(int argc,char**argv){
 if(argc<2)return 2;win64_math_self_test();
 if(!strcmp(argv[1],"--negative")){
  if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;
  c.xmm[0].u32[0]=0x40000000;c.xmm[1].u32[0]=0x3f2aaaab;
  if(!strcmp(argv[2],"precision"))c.mxcsr&=~4096u;
  else if(!strcmp(argv[2],"pole")){c.xmm[0].u32[0]=0;c.xmm[1].u32[0]=0xbf800000;}
  else if(!strcmp(argv[2],"negative"))c.xmm[0].u32[0]=0xbf800000;
  else if(!strcmp(argv[2],"subnormal_base"))c.xmm[0].u32[0]=1;
  else if(!strcmp(argv[2],"subnormal_power"))c.xmm[1].u32[0]=1;
  else if(!strcmp(argv[2],"NaN"))c.xmm[0].u32[0]=0x7fc12345;
  else if(!strcmp(argv[2],"infinity"))c.xmm[1].u32[0]=0x7f800000;
  else if(!strcmp(argv[2],"lower_band"))c.xmm[1].u32[0]=0xc2fc0000;
  else if(!strcmp(argv[2],"upper_band"))c.xmm[1].u32[0]=0x42ff0000;
  else if(!strcmp(argv[2],"underflow"))c.xmm[1].u32[0]=0xc3160000;
  else if(!strcmp(argv[2],"overflow"))c.xmm[1].u32[0]=0x43000000;
  else return 2;win64_math_call(&c,"powf");return 99;
 }
 FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);if(bytes<=0||bytes%sizeof(test_case))return 4;count=(size_t)bytes/sizeof(test_case);data=malloc((size_t)bytes);if(!data||fread(data,(size_t)bytes,1,f)!=1)return 5;fclose(f);
 accepted=malloc(count*sizeof(*accepted));if(!accepted)return 5;
 unsigned failures=0,boundary=0;size_t excluded=0,consumer=0,direct=0,route=0;
 for(size_t i=0;i<count;++i){int okay=one(&data[i],1);if(okay==2){++excluded;continue;}if(!okay){if(++failures>=16)break;continue;}
  accepted[accepted_count++]=i;if(data[i].original_width)++consumer;if(data[i].operation&1u)++route;else ++direct;
  uint32_t xb=(uint32_t)data[i].bits,yb=(uint32_t)(data[i].bits>>32);
  if((data[i].operation&2u)&&(xb&0x7fffffffu)&&xb!=0x3f800000u&&(yb&0x7fffffffu)&&yb!=0x3f800000u&&
    (data[i].expected<UINT32_C(0x01000000)||data[i].expected>UINT32_C(0x7f000000)))++boundary;
 }
 unsigned concurrent=0;
#if !defined(_WIN32)
 pthread_t threads[4];if(!failures){for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;for(unsigned i=0;i<4;++i){void*r;if(pthread_join(threads[i],&r)||r)++failures;}concurrent=8192;}
#endif
 printf("{\"success\":%s,\"cases\":%zu,\"checked_cases\":%zu,\"excluded_native_result_band_or_exception\":%zu,\"UCRT_boundary_rows_admitted\":%u,\"consumer_cases\":%zu,\"direct_cases\":%zu,\"CRT_return_pop_cases\":%zu,\"bit_exact\":%u,\"max_ULP\":%llu,\"failures\":%u,\"concurrent_cases\":%u,\"full_CPU_RSP_canary\":true,\"host_FP_errno_preserved\":true}\n",failures?"false":"true",count,accepted_count,excluded,boundary,consumer,direct,route,exact,worst,failures,concurrent);free(accepted);free(data);return failures?1:0;
}
