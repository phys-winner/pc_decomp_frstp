#include "cpu64.h"
#include <stdio.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <xmmintrin.h>
#endif
void bounds_reference(CPU*);
void bounds_candidate(CPU*);
void bounds_full_reference(CPU*);
void bounds_full_candidate(CPU*);
int64_t g_image_delta;
static uint32_t debug_flag;
void dispatch(CPU*c,uint64_t target){(void)c;(void)target;abort();}
static unsigned char arena[8192],before[8192],expected[8192];
static uint64_t seed=UINT64_C(0x626f756e6473786d);
static uint32_t random32(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return (uint32_t)seed;}
static uint64_t control(void){
#ifdef __aarch64__
 uint64_t x;__asm__ volatile("mrs %0,fpcr":"=r"(x));return x;
#else
 return _mm_getcsr();
#endif
}
static uint64_t status(void){
#ifdef __aarch64__
 uint64_t x;__asm__ volatile("mrs %0,fpsr":"=r"(x));return x;
#else
 return _mm_getcsr()&0x3f;
#endif
}
static void configure(unsigned mode){
#ifdef __aarch64__
 uint64_t x=((uint64_t)(mode&3)<<22)|((uint64_t)((mode>>2)&1)<<24)|((uint64_t)((mode>>3)&1)<<25);
 __asm__ volatile("msr fpcr,%0\nmsr fpsr,xzr"::"r"(x):"memory");
#else
 _mm_setcsr(0x1f80|((mode&3)<<13)|(((mode>>2)&1)<<15)|(((mode>>3)&1)<<6));
#endif
}
static void restore_control(uint64_t value){
#ifdef __aarch64__
 __asm__ volatile("msr fpcr,%0"::"r"(value):"memory");
#else
 _mm_setcsr((unsigned)value);
#endif
}
static uint32_t floating(unsigned kind){
 static const uint32_t edge[]={0,0x80000000,1,0x807fffff,0x00800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0xffc54321,0x7f812345};
 if(kind==0){float x=(int32_t)(random32()%2000)-1000;uint32_t r;memcpy(&r,&x,4);return r;}
 if(kind==1)return edge[random32()%12];
 return random32();
}
static CPU*prepare(unsigned kind,unsigned alias){
 for(unsigned i=0;i<sizeof(arena);++i)arena[i]=(unsigned char)random32();
 CPU*c=(CPU*)(arena+6144);
 c->rsp=(uintptr_t)arena+4096;c->rsi=(uintptr_t)arena+1024;c->rbx=(uintptr_t)arena+2048;c->rdi=(uintptr_t)arena+3072;
 if(alias==1)c->rsi=c->rbx;
 if(alias==2)c->rsi=c->rdi+16;
 if(alias==3)c->rsi=c->rsp+0x20;
 if(alias==4)c->rbx=c->rsp+0x50;
 if(alias==5)c->rdi=c->rsp+0x10;
 if(alias==6)c->rsi=(uintptr_t)&c->xmm[2];
 if(alias==7)c->rbx=(uintptr_t)&c->xmm[0];
 if(alias==8)c->rdi=(uintptr_t)&c->xmm[4];
 if(alias==9)c->rsp=(uintptr_t)&c->xmm[0]-0x20;
 for(unsigned i=0;i<8;++i){wr32(c->rsi+4*i,floating(kind));wr32(c->rbx+4*i,floating(kind));}
 for(unsigned i=0;i<16;++i)wr32(c->rdi+4*i,floating(kind));
 return c;
}
int main(int argc,char**argv){
 (void)argv;
 uint64_t saved=control();unsigned cases=0;g_image_delta=(int64_t)(uintptr_t)&debug_flag-INT64_C(0x14296fae0);
 for(unsigned whole=0;whole<2;++whole)for(unsigned kind=0;kind<3;++kind)for(unsigned alias=0;alias<10;++alias)for(unsigned mode=0;mode<16;++mode)for(unsigned n=0;n<32;++n){
  CPU*c=prepare(kind,alias);
  if(whole){c->rcx=c->rsi;c->rdx=c->rbx;c->r8=c->rdi;c->rsp+=0x98;}
  memcpy(before,arena,sizeof(arena));configure(mode);errno=1797;
  if(whole)bounds_full_reference(c);else bounds_reference(c);
  uint64_t old_status=status();int old_errno=errno;memcpy(expected,arena,sizeof(arena));
  memcpy(arena,before,sizeof(arena));configure(mode);errno=1797;
  if(whole)bounds_full_candidate(c);else bounds_candidate(c);
  uint64_t new_status=status();int new_errno=errno;
  if(old_status!=new_status||old_errno!=new_errno||memcmp(arena,expected,sizeof(arena))){
   size_t at=0;while(at<sizeof(arena)&&arena[at]==expected[at])++at;
   restore_control(saved);fprintf(stderr,"BOUNDS_LOCAL_MISMATCH whole=%u kind=%u alias=%u mode=%u n=%u byte=%zu status=%llu/%llu\n",whole,kind,alias,mode,n,at,(unsigned long long)old_status,(unsigned long long)new_status);return 1;
  }++cases;
 }
 restore_control(saved);
#ifdef __aarch64__
 if(argc>1){
  CPU*c=prepare(0,0);c->rcx=c->rsi;c->rdx=c->rbx;c->r8=c->rdi;c->rsp+=0x98;
  uint64_t entry_rsp=c->rsp;
  for(unsigned i=0;i<16;++i)wr32(c->r8+4*i,i%5==0?0x3f800000:0);
  for(unsigned i=0;i<4;++i){wr32(c->rcx+4*i,0xc1200000);wr32(c->rcx+16+4*i,0x41200000);wr32(c->rdx+4*i,0xbf800000);wr32(c->rdx+16+4*i,0x3f800000);}
  printf("{\"success\":true,\"pairs\":[");
  for(unsigned pair=0;pair<8;++pair){double time_ns[2];
   for(unsigned step=0;step<2;++step){unsigned variant=(pair+step)&1;void(*fn)(CPU*)=variant?bounds_full_candidate:bounds_full_reference;
    struct timespec a,b;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&a);
    for(unsigned i=0;i<100000;++i){c->rsp=entry_rsp;fn(c);}
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&b);time_ns[variant]=((b.tv_sec-a.tv_sec)*1e9+b.tv_nsec-a.tv_nsec)/100000.0;
   }
   printf("%s{\"original_ns\":%.3f,\"native_ns\":%.3f}",pair?",":"",time_ns[0],time_ns[1]);
  }printf("]}\n");return 0;
 }
#else
 (void)argc;
#endif
 printf("{\"success\":true,\"cases\":%u,\"full_CPU_memory_errno_FP\":true}\n",cases);
}
