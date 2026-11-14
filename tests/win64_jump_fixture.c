/* Production memory validation plus the standalone Win64 capture provider. */
#include "../android/host/win64.c"
#include "win64_jump.h"
#include "jump_native_seed.h"
#include <sys/wait.h>
static _Thread_local unsigned char test_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned n){uint64_t x=0;if(offset+n>sizeof(test_teb)||n>8)abort();memcpy(&x,test_teb+offset,n);return x;}
void frostpunk_teb_write(uint64_t offset,uint64_t x,unsigned n){if(offset+n>sizeof(test_teb)||n>8)abort();memcpy(test_teb+offset,&x,n);}
typedef struct {uint64_t frame;uint32_t mxcsr;uint16_t cw,reserved;unsigned char expected[256];} native_case;
_Static_assert(sizeof(native_case)==272,"native jump dataset record");
typedef struct {const native_case *cases;size_t count;unsigned id;unsigned checks;} thread_input;
static void put64(unsigned char *p,uint64_t value){memcpy(p,&value,8);}
static void setup(CPU *c,uint64_t *stack,unsigned char *buffer,uint64_t frame,uint32_t mxcsr){
 memset(c,0xa5,sizeof(*c));c->rcx=(uintptr_t)buffer;c->rdx=frame;c->rsp=(uintptr_t)stack;c->mxcsr=mxcsr;
 c->rbx=jump_gpr_seed[0];c->rbp=jump_gpr_seed[1];c->rsi=jump_gpr_seed[2];c->rdi=jump_gpr_seed[3];
 c->r12=jump_gpr_seed[4];c->r13=jump_gpr_seed[5];c->r14=jump_gpr_seed[6];c->r15=jump_gpr_seed[7];
 memcpy(c->xmm+6,jump_xmm_seed,160);
}
static void require(int okay,const char *why){if(!okay){fprintf(stderr,"JUMP_FIXTURE_FAIL %s\n",why);abort();}}
static void check_capture(const native_case *row,unsigned salt,int change_round){
 _Alignas(16) unsigned char storage[288];uint64_t stack[4]={UINT64_C(0x141c0e3ba)+salt,0,0,0};
 memset(storage,0xa5,sizeof(storage));unsigned char expected[256];memcpy(expected,row->expected,sizeof(expected));
 CPU c;setup(&c,stack,storage+16,row->frame,row->mxcsr);CPU before=c;
 put64(expected+16,c.rsp+8);put64(expected+80,stack[0]);
 win64_region_add((uintptr_t)storage,sizeof(storage),1);win64_region_add((uintptr_t)stack,sizeof(stack),0);
 if(change_round)win64_jump_set_round((row->cw>>10)&3u);
 win64_set_error(0x5a17);errno=1927;
 require(win64_jump_call(&c,"__intrinsic_setjmp"),"known capture handled");
 require(errno==1927&&win64_get_error()==0x5a17,"native errno and guest LastError preserved");
 require(c.rax==0,"initial result zero");c.rax=before.rax;
 require(!memcmp(&c,&before,sizeof(c)),"all other CPU state and pre-pop RSP preserved");
 require(!memcmp(storage+16,expected,256),"all256bytes equal normalized native VCRUNTIME capture");
 for(unsigned i=0;i<16;++i)require(storage[i]==0xa5&&storage[272+i]==0xa5,"external buffer canaries");
 require(stack[0]==UINT64_C(0x141c0e3ba)+salt&&!stack[1]&&!stack[2]&&!stack[3],"stack and original return slot unchanged");
 win64_region_remove((uintptr_t)stack);win64_region_remove((uintptr_t)storage);
}
static void *thread_main(void *opaque){thread_input *p=opaque;
 /* Native worker starts with CRT default x87 CW even when guest MXCSR differs. */
 native_case first=p->cases[0];first.cw=0x027f;memcpy(first.expected+92,&first.cw,2);first.mxcsr=0x7fa5;memcpy(first.expected+88,&first.mxcsr,4);
 check_capture(&first,p->id,0);++p->checks;
 for(unsigned i=0;i<512;++i){const native_case *row=p->cases+((i*31u+p->id*7u)%p->count);check_capture(row,i+p->id*1000,1);++p->checks;}
 return NULL;
}
int main(int argc,char **argv){
 if(argc<2)return 2;win64_jump_self_test();
 if(!strcmp(argv[1],"--negative")){
  if(argc!=3)return 2;_Alignas(16) unsigned char buffer[288];uint64_t stack[2]={123,0};CPU c={0};
  c.rcx=(uintptr_t)buffer;c.rsp=(uintptr_t)stack;
  size_t bytes=!strcmp(argv[2],"short_buffer")?255:sizeof(buffer);
  win64_region_add((uintptr_t)buffer,bytes,strcmp(argv[2],"readonly")!=0);
  win64_region_add((uintptr_t)stack,!strcmp(argv[2],"short_stack")?7:sizeof(stack),0);
  if(!strcmp(argv[2],"unaligned"))++c.rcx;
  else if(!strcmp(argv[2],"null"))c.rcx=0;
  else if(!strcmp(argv[2],"overflow"))c.rsp=UINT64_MAX-3;
  else if(!strcmp(argv[2],"overlap"))c.rsp=c.rcx+128;
  else if(!strcmp(argv[2],"bad_round"))win64_jump_set_round(4);
  else if(strcmp(argv[2],"short_buffer")&&strcmp(argv[2],"readonly")&&strcmp(argv[2],"short_stack"))return 2;
  win64_jump_call(&c,"__intrinsic_setjmp");return 99;
 }
 FILE *f=fopen(argv[1],"rb");if(!f)return 3;if(fseek(f,0,SEEK_END))return 3;long bytes=ftell(f);if(bytes<=0||(size_t)bytes%sizeof(native_case)||fseek(f,0,SEEK_SET))return 3;
 size_t count=(size_t)bytes/sizeof(native_case);native_case *cases=malloc((size_t)bytes);if(!cases||fread(cases,sizeof(*cases),count,f)!=count||fclose(f))return 3;
 for(size_t i=0;i<count;++i)check_capture(cases+i,(unsigned)i,1);
 pthread_t threads[4];thread_input inputs[4];unsigned concurrent=0;
 for(unsigned i=0;i<4;++i){inputs[i]=(thread_input){cases,count,i,0};if(pthread_create(threads+i,NULL,thread_main,inputs+i))abort();}
 for(unsigned i=0;i<4;++i){if(pthread_join(threads[i],NULL))abort();concurrent+=inputs[i].checks;}
 CPU c={0},before=c;require(!win64_jump_call(&c,"longjmp")&&!memcmp(&c,&before,sizeof(c)),"longjmp remains explicitly unhandled");
 printf("{\"success\":true,\"native_context_cases\":%zu,\"threads\":4,\"concurrent_contexts\":%u,\"buffer_bytes\":256,\"Spare_preserved\":true,\"native_layout_compared\":true,\"full_CPU_preserved_except_RAX\":true,\"x87_RC_thread_local\":true,\"no_rsp_pop\":true,\"longjmp_implemented\":false,\"original_x64_executed\":false,\"game_boot_proven\":false}\n",count,concurrent);free(cases);return 0;
}
