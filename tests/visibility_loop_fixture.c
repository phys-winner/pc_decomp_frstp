#include "cpu64.h"
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include "constants.h"
void old_L_000140F8F530(CPU*);void new_L_000140F8F530(CPU*);
int64_t old_g_image_delta,new_g_image_delta;
#define BYTES (2u*1024u*1024u)
#define MAX_N 513u
static unsigned char arena[BYTES],before[BYTES],expected[BYTES];
#define MAX_CALLBACKS 16384u
static CPU current,initial,want,observed[MAX_CALLBACKS],expected_observed[MAX_CALLBACKS];
static unsigned char observed_stack[MAX_CALLBACKS][576],expected_stack[MAX_CALLBACKS][576];
static unsigned observations,expected_observations,group_count,entities,detail_mode,benchmark,alias_case,kind_case,iteration_case;
static uint64_t stack_top,checks,seed=UINT64_C(0x76697369626c652d);
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return(uint32_t)seed;}
static uint64_t at(unsigned offset){return(uintptr_t)arena+offset;}
static void finish(CPU*c){c->rip=pop64(c);}
static void callback(CPU*c,uint64_t target){
 c->rip=target;
 if(!benchmark&&observations==MAX_CALLBACKS)abort();
 if(!benchmark){observed[observations]=*c;memcpy(observed_stack[observations],(void*)(uintptr_t)(stack_top-512),576);}
 ++observations;
 if(target==UINT64_C(0x140f8f450)){
  wr64(c->r8,at(0x400));wr32(c->r8+8,group_count);wr32(c->r8+12,group_count);wr64(c->r8+16,at(0x500));
 }else if(target==UINT64_C(0x140e2c200)||target==UINT64_C(0x140e2c6c0)){
  static const float box[8]={-1,-1,-1,0,1,1,1,0};memcpy((void*)(uintptr_t)c->rcx,box,32);
 }else if(target==UINT64_C(0x140e0a510)){
  static const float sphere[5]={0,0,0,1,1};memcpy((void*)(uintptr_t)c->rcx,sphere,20);
  if(iteration_case&1)wr64(stack_top+0x18,(uintptr_t)&current.xmm[9]);
 }else if(target==UINT64_C(0x140d9e420)){
  uint64_t values=rd64(c->rcx);unsigned n=rd32(c->rcx+8);if(n>=rd32(c->rcx+12))abort();wr64(values+8u*n,rd64(c->rdx));wr32(c->rcx+8,n+1);
 }else if(target!=UINT64_C(0x141ae327c)&&target!=UINT64_C(0xe5b10008)&&target!=UINT64_C(0x140e2c890)&&target!=UINT64_C(0x140e2c640)&&target!=UINT64_C(0x140e0a5c0)){
  fprintf(stderr,"UNEXPECTED_VISIBILITY_CALLBACK %llx\n",(unsigned long long)target);abort();
 }
 uint64_t value=UINT64_C(0x123456789abcdef0)^target;
 c->rax=value;c->rcx=value+1;c->rdx=value+2;c->r8=value+3;c->r9=value+4;c->r10=value+5;c->r11=value+6;
 c->cf=1;c->pf=0;c->zf=1;c->sf=0;c->of=1;c->af=1;
 for(unsigned i=0;i<6;++i){c->xmm[i].u64[0]=value+i;c->xmm[i].u64[1]=value-i;}
 if(target==UINT64_C(0x141ae327c)){c->xmm[0].u32[0]=0x3f13cd3a;c->xmm[0].u32[1]=0;c->xmm[0].u64[1]=0;}
 if(target==UINT64_C(0x140e2c890)||target==UINT64_C(0x140e2c640)||target==UINT64_C(0x140e0a5c0))SET8L(c->rax,kind_case==9?0:1);
 finish(c);
}
void old_fp_dispatch_main(CPU*c,uint64_t target,void(*linked)(CPU*)){(void)linked;callback(c,target);}
void new_fp_dispatch_main(CPU*c,uint64_t target,void(*linked)(CPU*)){(void)linked;callback(c,target);}
void old_dispatch(CPU*c,uint64_t target){callback(c,target);}void new_dispatch(CPU*c,uint64_t target){callback(c,target);}
unsigned old_fp_test_detail_mode(CPU*c){(void)c;return detail_mode;}unsigned new_fp_test_detail_mode(CPU*c){(void)c;return detail_mode;}
float old_fp_test_detail_threshold(float q,unsigned mode){return mode?.04f:q;}float new_fp_test_detail_threshold(float q,unsigned mode){return mode?.04f:q;}
static uint64_t control(void){uint64_t v;__asm__ volatile("mrs %0,fpcr":"=r"(v));return v;}
static uint64_t status(void){uint64_t v;__asm__ volatile("mrs %0,fpsr":"=r"(v));return v;}
static void floating(uint64_t c,uint64_t s){__asm__ volatile("msr fpcr,%0\nmsr fpsr,%1\nisb"::"r"(c),"r"(s):"memory");}
static void arguments(void){
 current.rcx=at(0x100);current.rdx=at(0x200);current.r8=alias_case==3?(uintptr_t)&current.xmm[9]:alias_case==7?stack_top-272:at(0x300);current.r9=4;current.rsp=stack_top;
 wr32(stack_top+0x28,5);wr64(stack_top+0x30,alias_case==4?(uintptr_t)&current.xmm[9]:alias_case==8?stack_top-272:at(0x380));wr64(stack_top,UINT64_C(0xdeadbeef));wr32(at(0x208),0);
}
static void prepare(unsigned kind,unsigned groups,unsigned n,unsigned alias,unsigned iteration){
 memset(arena,(int)(iteration&255),sizeof(arena));for(unsigned i=0;i<sizeof(current);++i)((unsigned char*)&current)[i]=(unsigned char)rnd();
 current.cf=rnd()&1;current.pf=rnd()&1;current.zf=rnd()&1;current.sf=rnd()&1;current.af=rnd()&1;current.of=rnd()&1;current.mxcsr=0x1f80;
 alias_case=alias;kind_case=kind;iteration_case=iteration;group_count=groups;entities=n;detail_mode=kind>=7|| (kind<5&&(iteration&1))?40:0;stack_top=at(0x1f8000);
 memset((void*)(uintptr_t)at(0x100),0,128);wr64(at(0x110),at(0x800));wr64(at(0x200),at(0x8000));wr32(at(0x208),0);wr32(at(0x20c),groups*n+1);
 for(unsigned i=0;i<8;++i)wr32(at(0x300)+i*4,kind==0?0:UINT32_MAX);wr64(at(0x380),kind==1?0x200:0);
 wr64(at(0x500),at(0x580));wr64(at(0x588),UINT64_C(0xe5b10008));
 for(unsigned g=0;g<groups;++g){
  uint64_t block=at(0x800+g*256),list=at(0x2000+g*MAX_N*8);wr64(at(0x400)+g*16,block);wr8(at(0x408)+g*16,kind>=8?0:1);wr64(block+0x68,list);wr32(block+0x70,n);
  if(alias==6&&g==0)list=stack_top-272,wr64(block+0x68,list);
  if(alias==1&&g==0&&n<=12){list=(uintptr_t)&current.xmm[9];wr64(block+0x68,list);}
  for(unsigned i=0;i<n;++i){
   uint64_t node=at(0x10000+(g*MAX_N+i)*640);if(alias==2&&g==0&&n<=2)node=stack_top-0x300+i*640;
   wr64(list+i*8,node);unsigned type=rnd()&255;wr8(node+0x248,type);wr64(node+0x58,0x200);wr64(node+0x40,(kind==2||(iteration>>1)%5)?at(0x1000):0);
   wr32(node+0x170,kind==3?4:0);static const uint32_t priorities[]={0x80000000u,0x7fffffffu,0xffffffffu,5,6,0};wr32(node+0x174,kind==4?priorities[iteration%6]:0);
   if(kind==7||kind==8)wr32(node+0x170,i&1?4:0);
   if(alias==5)wr64(node+0x40,(uintptr_t)&current.xmm[9]-0x40);
   if(alias==9)wr64(list+i*8,(uintptr_t)&current.rdi-0x248);
   if(kind==6){unsigned k=i%6;wr64(node+0x40,k==2?at(0x1000):0);wr32(node+0x170,k==3?4:0);wr32(node+0x174,k==4?6:0);}
  }
 }
 unsigned chain=kind==2?1+iteration%4:(iteration>>1)%5;for(unsigned p=0;p<5;++p){uint64_t parent=at(0x1000+p*128);wr64(parent+0x58,kind==2&&p+1==chain?0x10:0);wr64(parent+0x40,p+1<chain?parent+128:0);}wr64(at(0x180000+0x20f8),at(0x188000));wr32(at(0x188020),0x42700000);
 if(alias==3)memset(&current.xmm[9],kind?255:0,32);
 if(alias==4)current.xmm[9].u64[0]=kind==1?0x200:0;
 if(alias==5){current.xmm[9].u64[0]=0;current.xmm[10].u64[1]=kind==2?0x10:0;}
 if(kind>=7)memset(&current.xmm[9],255,32);
 arguments();initial=current;memcpy(before,arena,sizeof(arena));
}
static void check(unsigned kind,unsigned groups,unsigned n,unsigned alias,unsigned iteration){
 prepare(kind,groups,n,alias,iteration);observations=0;errno=1797;floating(0,0x15);old_L_000140F8F530(&current);uint64_t old_status=status();int old_errno=errno;
 want=current;memcpy(expected,arena,sizeof(arena));expected_observations=observations;memcpy(expected_observed,observed,observations*sizeof(CPU));memcpy(expected_stack,observed_stack,observations*sizeof(observed_stack[0]));
 current=initial;memcpy(arena,before,sizeof(arena));observations=0;errno=1797;floating(0,0x15);new_L_000140F8F530(&current);uint64_t new_status=status();int new_errno=errno;
 if(memcmp(&want,&current,sizeof(CPU))||memcmp(expected,arena,sizeof(arena))||old_status!=new_status||old_errno!=new_errno||observations!=expected_observations||memcmp(observed,expected_observed,observations*sizeof(CPU))||memcmp(observed_stack,expected_stack,observations*sizeof(observed_stack[0]))){
  size_t cpu_at=0,mem_at=0;while(cpu_at<sizeof(CPU)&&((unsigned char*)&current)[cpu_at]==((unsigned char*)&want)[cpu_at])++cpu_at;while(mem_at<BYTES&&arena[mem_at]==expected[mem_at])++mem_at;
  fprintf(stderr,"VISIBILITY_LOOP_MISMATCH kind=%u groups=%u n=%u alias=%u iteration=%u cpu=%zu memory=%zu observations=%u/%u\n",kind,groups,n,alias,iteration,cpu_at,mem_at,expected_observations,observations);exit(1);
 }
 ++checks;
}
static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return(uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static double measure(void(*fn)(CPU*)){uint64_t start=ns();for(unsigned i=0;i<128;++i){arguments();observations=0;fn(&current);}return(double)(ns()-start)/128;}
int main(int argc,char**argv){
 (void)argv;uint64_t saved_control=control(),saved_status=status();
 void*image=mmap(NULL,0x4539000u,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(image==MAP_FAILED)return 2;
 for(unsigned i=0;i<sizeof(constants)/sizeof(*constants);++i)memcpy((unsigned char*)image+constants[i].offset,constants[i].value,4);
 wr64((uintptr_t)image+0x2b3b598,at(0x180000));old_g_image_delta=new_g_image_delta=(intptr_t)image-INT64_C(0x140000000);if(mprotect(image,0x4539000u,PROT_READ))return 2;
 if(argc>1){benchmark=1;floating(0,0x15);printf("{\"success\":true,\"pairs\":[");
  for(unsigned kind=0;kind<7;++kind){prepare(kind,4,MAX_N,0,0);for(unsigned pair=0;pair<8;++pair){double a,b;if(pair&1){b=measure(new_L_000140F8F530);a=measure(old_L_000140F8F530);}else{a=measure(old_L_000140F8F530);b=measure(new_L_000140F8F530);}printf("%s{\"kind\":%u,\"original_ns\":%.3f,\"native_ns\":%.3f}",kind||pair?",":"",kind,a,b);}}puts("]}");
 }else{unsigned counts[]={0,1,2,3,8,64,256,MAX_N};for(unsigned kind=0;kind<7;++kind)for(unsigned g=0;g<3;++g)for(unsigned j=0;j<8;++j)for(unsigned n=0;n<16;++n)check(kind,g==2?4:g,counts[j],0,n);
  for(unsigned alias=1;alias<=2;++alias)for(unsigned kind=0;kind<7;++kind)for(unsigned n=0;n<4;++n)check(kind,1,alias==1?8:1,alias,n);
  for(unsigned alias=3;alias<=5;++alias)for(unsigned kind=0;kind<7;++kind)for(unsigned n=0;n<4;++n)check(kind,1,8,alias,n);
  for(unsigned alias=6;alias<=9;++alias)for(unsigned n=0;n<4;++n)check(0,1,2,alias,n);
  for(unsigned kind=7;kind<=9;++kind)for(unsigned n=0;n<8;++n)check(kind,1,8,0,n);
  printf("{\"success\":true,\"queries\":%llu,\"whole_actual_objects\":true,\"full_cpu_memory_callbacks_fp_errno\":true}\n",(unsigned long long)checks);
 }
 floating(saved_control,saved_status);return 0;
}
