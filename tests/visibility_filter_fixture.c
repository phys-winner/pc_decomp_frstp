#include "cpu64.h"
#include <errno.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int filter_reference(CPU*);
int filter_candidate(CPU*);
static unsigned char arena[16384],before[16384],expected[16384];
static uint64_t seed=UINT64_C(0x63756c6c66696c74);
static uint32_t random32(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return (uint32_t)seed;}
static CPU*prepare(unsigned kind,unsigned alias,unsigned chain){
 for(unsigned i=0;i<sizeof(arena);++i)arena[i]=(unsigned char)random32();
 CPU*c=(CPU*)(arena+8192);uint64_t node=(uintptr_t)arena+1024,mask=(uintptr_t)arena+512,exclude=(uintptr_t)arena+600;
 c->rbx=(uintptr_t)arena;c->rsp=(uintptr_t)arena+4096;c->rbp=(uintptr_t)arena+3072;
 c->cf=random32()&1;c->zf=random32()&1;c->sf=random32()&1;c->of=random32()&1;c->pf=random32()&1;c->af=random32()&1;
 unsigned class_id=random32()&255;
 wr64(c->rbx,node);wr8(node+0x248,class_id);wr32(mask+4*(class_id>>5),kind==0?0:1u<<(class_id&31));
 wr64(c->rbp+0x67,mask);wr64(c->rbp+0x7f,exclude);wr64(exclude,kind==1?0x200:0);
 wr64(node+0x58,0x200);wr64(node+0x40,chain?(uintptr_t)arena+5000:0);
 for(unsigned i=0;i<chain;++i){uint64_t parent=(uintptr_t)arena+5000+i*128;wr64(parent+0x58,kind==2&&i+1==chain?0x10:0);wr64(parent+0x40,i+1<chain?parent+128:0);}
 wr32(c->rbp+0x6f,4);wr32(node+0x170,kind==3?4:0);
 wr32(c->rbp+0x77,kind==5?random32():5);wr32(node+0x174,kind==4?6:kind==5?random32():random32()%6);
 if(alias==1){c->rbx=(uintptr_t)&c->rdi;c->rdi=node;}
 if(alias==2){c->rsp=(uintptr_t)&c->rax-0x28;}
 if(alias==3){wr64(c->rbp+0x67,(uintptr_t)&c->rcx);wr8(node+0x248,0);}
 if(alias==4){node=(uintptr_t)&c->rax-0x248;wr64(c->rbx,node);c->rax=0;wr32(mask,0);}
 if(alias==5){uint64_t parent=(uintptr_t)&c->rax-0x58;wr64(node+0x40,parent);wr64(parent+0x40,0);}
 if(alias==6){wr64(c->rbp+0x7f,(uintptr_t)&c->rax);}
 if(alias==7){c->rsp=node+0x58-0x28;}
 if(alias==8){c->rsp=mask+4*(class_id>>5)-0x28;}
 if(alias==9){c->rsp=c->rbp+0x67-0x28;}
 return c;
}
static uint64_t ns(void){
#ifdef _WIN32
 LARGE_INTEGER counter,frequency;QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);return (uint64_t)(counter.QuadPart*(1000000000.0/frequency.QuadPart));
#else
 struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
#endif
}
int main(int argc,char**argv){
 (void)argv;unsigned cases=0;
 for(unsigned kind=0;kind<6;++kind)for(unsigned alias=0;alias<10;++alias)for(unsigned chain=0;chain<5;++chain)for(unsigned n=0;n<128;++n){
  CPU*c=prepare(kind,alias,chain);memcpy(before,arena,sizeof(arena));errno=1797;int old=filter_reference(c),old_errno=errno;memcpy(expected,arena,sizeof(arena));
  memcpy(arena,before,sizeof(arena));errno=1797;int actual=filter_candidate(c),new_errno=errno;
  if(old!=actual||old_errno!=new_errno||memcmp(arena,expected,sizeof(arena))){size_t at=0;while(at<sizeof(arena)&&arena[at]==expected[at])++at;fprintf(stderr,"CULL_FILTER_MISMATCH kind=%u alias=%u chain=%u n=%u return=%d/%d byte=%zu\n",kind,alias,chain,n,old,actual,at);return 1;}++cases;
 }
 if(argc>1){
  printf("{\"pairs\":[");
  for(unsigned kind=0;kind<6;++kind){CPU*c=prepare(kind,0,4);memcpy(before,arena,sizeof(arena));
   for(unsigned pair=0;pair<8;++pair){uint64_t times[2]={0};
    for(unsigned j=0;j<2;++j){unsigned variant=(pair+j)%2;uint64_t start=ns();for(unsigned i=0;i<100000;++i){if(variant)filter_candidate(c);else filter_reference(c);}times[variant]=ns()-start;}
    printf("%s{\"kind\":%u,\"original_ns\":%.3f,\"native_ns\":%.3f}",kind||pair?",":"",kind,times[0]/100000.0,times[1]/100000.0);
   }
  }
  printf("],\"success\":true}\n");
 }else printf("{\"success\":true,\"cases\":%u,\"full_cpu_memory_errno\":true,\"alias_fallbacks\":true}\n",cases);
 return 0;
}
