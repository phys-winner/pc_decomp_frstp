/* Float trig differential fixture. Original x64 game bytes never execute. */
#include "win64_math.h"
#include <stdio.h>
#include <stdint.h>
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
static LOCAL uint64_t guest_matherr;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return guest_matherr;}
void *win64_pointer(uint64_t address,size_t bytes,int writing){
 (void)address;(void)bytes;(void)writing;fputs("TRIG_FIXTURE_FAIL unexpected pointer access\n",stderr);_Exit(1);
}
#include "trig_route.inc"
static const char *names[]={"sinf","cosf","tanf"};
typedef struct {uint64_t bits;uint32_t operation,mxcsr;int32_t initial_errno;uint32_t handler;uint64_t expected_bits;uint32_t expected_mxcsr;int32_t expected_errno;uint32_t max_ulp,route;} test_case;
_Static_assert(sizeof(test_case)==48,"trig fixture layout");
typedef struct {const test_case *data;size_t count;unsigned id;uint64_t tested,failed,max_ulp[3];} worker;
static void host_env(uint64_t out[2]){
#if defined(__aarch64__)
 __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
 out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void host_prepare(unsigned index){
#if defined(__aarch64__)
 uint64_t control,status;
 __asm__ volatile("mrs %0,fpcr":"=r"(control));
 control=(control&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|((uint64_t)(index&3)<<22)|(UINT64_C(1)<<24);
 status=UINT64_C(0x08000011);__asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(control),"r"(status):"memory");
#else
 _mm_setcsr(0x9fd1u|((index&3)<<13));
#endif
}
static uint32_t ordered(uint32_t bits){return bits&UINT32_C(0x80000000)?~bits:bits|UINT32_C(0x80000000);}
static int run_case(const test_case *t,size_t index,uint64_t *distance_out){
 if(t->operation>2)return 0;
 CPU c,before;memset(&c,0xa5,sizeof(c));c.mxcsr=t->mxcsr;c.xmm[0].u32[0]=(uint32_t)t->bits;before=c;
 guest_errno=t->initial_errno;guest_matherr=t->handler?UINT64_C(0x140123450):0;host_prepare((unsigned)index);
 uint64_t old[2],now[2];host_env(old);errno=1927;
#if defined(_WIN32)
 SetLastError(0x5a17);
#endif
 int handled=t->route?trig_import(&c,"api-ms-win-crt-math-l1-1-0.dll",names[t->operation]):win64_math_call(&c,names[t->operation]);int host_errno=errno;host_env(now);
 uint32_t result=c.xmm[0].u32[0],a=ordered(result),b=ordered((uint32_t)t->expected_bits),mxcsr=c.mxcsr;
 uint64_t distance=a>b?(uint64_t)a-b:(uint64_t)b-a;*distance_out=distance;
 /* Only scalar XMM0.low32 and guest exception status may change. */
 c.xmm[0].u32[0]=before.xmm[0].u32[0];c.mxcsr=before.mxcsr;if(t->route)before.rsp+=8;
 int okay=handled&&distance<=t->max_ulp&&mxcsr==t->expected_mxcsr&&guest_errno==t->expected_errno&&host_errno==1927&&!memcmp(&c,&before,sizeof(c))&&!memcmp(old,now,sizeof(old));
#if defined(_WIN32)
 okay=okay&&GetLastError()==0x5a17;
#endif
 if(!okay)fprintf(stderr,"TRIG_FAIL %zu %s input=%08x mode=%x result=%08x expected=%08x ULP=%llu limit=%u csr=%x expected=%x errno=%d expected=%d hosterrno=%d env=%llx/%llx=>%llx/%llx\n",index,names[t->operation],(unsigned)t->bits,t->mxcsr,result,(unsigned)t->expected_bits,(unsigned long long)distance,t->max_ulp,mxcsr,t->expected_mxcsr,guest_errno,t->expected_errno,host_errno,(unsigned long long)old[0],(unsigned long long)old[1],(unsigned long long)now[0],(unsigned long long)now[1]);
 return okay;
}
#if !defined(_WIN32)
static void *parallel(void *opaque){worker *w=opaque;
 for(unsigned i=0;i<2048;++i){size_t n=((size_t)i*7919+w->id*997)%w->count;uint64_t distance;
  if(!run_case(w->data+n,n,&distance)){++w->failed;break;}++w->tested;if(distance>w->max_ulp[w->data[n].operation])w->max_ulp[w->data[n].operation]=distance;}
 return NULL;
}
#endif
int main(int argc,char **argv){
 if(argc<2)return 2;guest_matherr=0;win64_math_self_test();
 if(!strcmp(argv[1],"--negative")){
  if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;
  if(!strcmp(argv[2],"matherr_sinf")){guest_matherr=1;c.xmm[0].u32[0]=0x7f800000;win64_math_call(&c,"sinf");}
  else if(!strcmp(argv[2],"matherr_cosf")){guest_matherr=1;c.xmm[0].u32[0]=0xff800000;win64_math_call(&c,"cosf");}
  else if(!strcmp(argv[2],"unmasked_invalid")){c.mxcsr&=~128u;c.xmm[0].u32[0]=0x7f800000;win64_math_call(&c,"sinf");}
  else if(!strcmp(argv[2],"unmasked_precision")){c.mxcsr&=~4096u;c.xmm[0].u32[0]=0x3fc90fdb;win64_math_call(&c,"sinf");}
  else if(!strcmp(argv[2],"unmasked_denormal")){c.mxcsr&=~256u;c.xmm[0].u32[0]=1;win64_math_call(&c,"sinf");}
  else if(!strcmp(argv[2],"matherr_tanf")){guest_matherr=1;c.xmm[0].u32[0]=0x7f800000;win64_math_call(&c,"tanf");}
  else if(!strcmp(argv[2],"tanf_unmasked_invalid")){c.mxcsr&=~128u;c.xmm[0].u32[0]=0x7f800000;win64_math_call(&c,"tanf");}
  else if(!strcmp(argv[2],"tanf_unmasked_precision")){c.mxcsr&=~4096u;c.xmm[0].u32[0]=0x3f000000;win64_math_call(&c,"tanf");}
  else if(!strcmp(argv[2],"tanf_tiny_precision")){c.mxcsr&=~4096u;c.xmm[0].u32[0]=0x371a25c1;win64_math_call(&c,"tanf");}
  else if(!strcmp(argv[2],"tanf_min_normal_underflow")){c.mxcsr&=~2048u;c.xmm[0].u32[0]=0x00800000;win64_math_call(&c,"tanf");}
  else if(!strcmp(argv[2],"tanf_subnormal_denormal")){c.mxcsr&=~256u;c.xmm[0].u32[0]=1;win64_math_call(&c,"tanf");}
  else return 2;return 99;
 }
 FILE *f=fopen(argv[1],"rb");if(!f)return 3;if(fseek(f,0,SEEK_END))return 3;long bytes=ftell(f);if(bytes<=0||(size_t)bytes%sizeof(test_case)||fseek(f,0,SEEK_SET))return 3;
 size_t count=(size_t)bytes/sizeof(test_case);test_case *data=malloc((size_t)bytes);if(!data||fread(data,sizeof(*data),count,f)!=count||fclose(f))return 3;
 unsigned failures=0;uint64_t exact=0,worst[3]={0};size_t cases[3]={0},routes=0;
 for(size_t i=0;i<count;++i){uint64_t distance;if(!run_case(data+i,i,&distance)&&++failures>=12)break;if(!distance)++exact;if(distance>worst[data[i].operation])worst[data[i].operation]=distance;++cases[data[i].operation];routes+=data[i].route!=0;}
 uint64_t parallel_count=0;
#if !defined(_WIN32)
 pthread_t threads[4];worker workers[4];
 if(!failures){for(unsigned i=0;i<4;++i){workers[i]=(worker){data,count,i,0,0,{0,0}};if(pthread_create(threads+i,NULL,parallel,workers+i))abort();}
  for(unsigned i=0;i<4;++i){if(pthread_join(threads[i],NULL))abort();failures+=(unsigned)workers[i].failed;parallel_count+=workers[i].tested;}}
#endif
 CPU c={0},before=c;if(win64_math_call(&c,"unimplemented_trig")||memcmp(&c,&before,sizeof(c)))return 4;
 printf("{\"success\":%s,\"cases\":%zu,\"operation_cases\":[%zu,%zu,%zu],\"CRT_pop_cases\":%zu,\"failures\":%u,\"bit_exact_cases\":%llu,\"max_ULP\":{\"sinf\":%llu,\"cosf\":%llu,\"tanf\":%llu},\"concurrent_cases\":%llu,\"full_CPU_preserved\":true,\"native_FPenv_errno_preserved\":true,\"direct_no_rsp_pop\":true,\"CRT_single_pop\":true,\"original_x64_executed\":false,\"game_boot_proven\":false}\n",failures?"false":"true",count,cases[0],cases[1],cases[2],routes,failures,(unsigned long long)exact,(unsigned long long)worst[0],(unsigned long long)worst[1],(unsigned long long)worst[2],(unsigned long long)parallel_count);free(data);return failures?1:0;
}
