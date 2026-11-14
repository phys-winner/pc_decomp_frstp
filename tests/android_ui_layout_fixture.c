#define _GNU_SOURCE
#include "cpu64.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
int64_t g_image_delta;
static _Alignas(16) unsigned char layout[0x300],children[4][0x300],stack[32768];
static uint64_t vtable[128];
static unsigned allocations,relayouts,flag_calls,defer_body_anchor,pending_body_anchor;
void frostpunk_unsupported(uint64_t rip,const char *instruction){fprintf(stderr,"unsupported layout %llx %s\n",(unsigned long long)rip,instruction);abort();}
#include "ui_layout_original.inc"
void dispatch(CPU *c,uint64_t target){
 switch(target){
#include "ui_layout_dispatch.inc"
 case 1:c->rax=0;break; /* Plain non-text fixtures have no font measurement. */
 case UINT64_C(0x140DCE870):case UINT64_C(0x140DCE2C0):{
  unsigned count=(unsigned)c->rdx;if(!count)count=8;uint64_t old=rd64(c->rcx);void *p=realloc((void*)(uintptr_t)old,count*8);assert(p);
  if(!old)++allocations;wr64(c->rcx,(uintptr_t)p);wr32(c->rcx+12,count);break;}
 case UINT64_C(0x141AE0C20):c->rax=(uintptr_t)malloc((size_t)c->rcx);assert(c->rax);++allocations;break;
 case UINT64_C(0x140E2BAA0):if(c->rcx){free((void*)(uintptr_t)c->rcx);--allocations;}break;
 case UINT64_C(0x141AE3216):memset((void*)(uintptr_t)c->rcx,(int)c->rdx,(size_t)c->r8);c->rax=c->rcx;break;
 case UINT64_C(0x140F95A20):break; /* Render dirty notification has no layout arithmetic. */
 default:fprintf(stderr,"unexpected original layout call=%llx\n",(unsigned long long)target);abort();
 }
 c->rsp+=8;
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
static unsigned applied_scale;
static void call_value(CPU *cpu,uint64_t function,uint64_t object,unsigned value){
 (void)cpu;
 if(function==UINT64_C(0x140EC0F60)){wr32(object+0x1e0,rd32(object+0x1e0)|value);++flag_calls;}
 else if(function==UINT64_C(0x140EBE470)){wr32(object+0x1e0,rd32(object+0x1e0)&~value);++flag_calls;}
 else if(function==UINT64_C(0x140EC30A0)){
  if(defer_body_anchor&&object==(uintptr_t)children[1])pending_body_anchor=value+1;
  else wr32(object+0x13c,value);
 }
 else if(function==UINT64_C(0x140EC39C0))wr32(object+0x138,value);
 else {assert(function==UINT64_C(0x141096AE0));++relayouts;}
}
static void call(CPU *cpu,uint64_t function,uint64_t object,const float *vector,float a,float b,float d){
 (void)a;(void)b;(void)d;assert(function==UINT64_C(0x140EC4C70)||function==UINT64_C(0x140F99FB0));
 if(function==UINT64_C(0x140EC4C70))memcpy((void*)(uintptr_t)(object+0x210),vector,16);
 CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=object;c.rdx=(uintptr_t)vector;push64(&c,0);dispatch(&c,UINT64_C(0x140F99FB0));
}
static void layout_position(CPU *cpu,uint64_t object,float x,float y){
 float position[4]={x,y,0,1};CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=object;c.rdx=(uintptr_t)position;
 push64(&c,0);dispatch(&c,UINT64_C(0x140F99F00));
}
#include "mobile_selection_columns.inc"
static CPU cpu;
static void flow(void){CPU c=cpu;c.rcx=(uintptr_t)layout;push64(&c,0);dispatch(&c,UINT64_C(0x141097460));assert(!allocations&&c.rsp==cpu.rsp);}
static void node(unsigned char *p,const char *name,float w,float h){
 wr64((uintptr_t)p,(uintptr_t)vtable);wr64((uintptr_t)p+0x1d8,(uintptr_t)name);wr32((uintptr_t)p+0x1e4,(uint32_t)(p-children[0]+1));
 wrss((uintptr_t)p+0x100,w);wrss((uintptr_t)p+0x104,h);wrss((uintptr_t)p+0xf0,1);wrss((uintptr_t)p+0xf4,1);
 wrss((uintptr_t)p+0x210,w);wrss((uintptr_t)p+0x214,h);
}
static void setup(void){
 memset(layout,0,sizeof(layout));memset(children,0,sizeof(children));
 node(layout,"GENERATOR_SUBPANEL",331,588);wrss((uintptr_t)layout+0x214,242);wr64((uintptr_t)layout,GVA(UINT64_C(0x141DE0A48)));
 const char *names[]={"EXT_BUILDING_HEADER","TAB_LAYOUT","BODY_LAYOUT","HIDDEN_LAYOUT"};
 float sizes[]={225,45,313,900};for(unsigned i=0;i<4;++i){node(children[i],names[i],331,sizes[i]);
  wr64((uintptr_t)children[i]+0x230,(uintptr_t)layout);wr64((uintptr_t)children[i]+0x168,(uintptr_t)layout);
  if(i<3){wr64((uintptr_t)children[i]+0x240,(uintptr_t)children[i+1]);wr64((uintptr_t)children[i]+0x178,(uintptr_t)children[i+1]);}}
 wr32((uintptr_t)children[0]+0x13c,6); /* Actual header stretches with its parent unless detached. */
 wr32((uintptr_t)children[1]+0x13c,6); /* Actual TAB_CONTROLLER also stretches and feeds width back. */
 wr32((uintptr_t)children[3]+0x13c,6); /* Hidden bodies must remain safe when shown later. */
 wr32((uintptr_t)children[3]+0x34,2);wr32((uintptr_t)children[3]+0x1e0,2);
 wr64((uintptr_t)layout+0x220,(uintptr_t)children[0]);wr64((uintptr_t)layout+0x158,(uintptr_t)children[0]);
}
static void setup_generator(void){
 setup();wr32((uintptr_t)layout+0x1e4,0x60000);wrss((uintptr_t)layout+0x294,5);
 for(unsigned i=0;i<4;++i)wr32((uintptr_t)children[i]+0x1e4,0x60001+i);
 wr64((uintptr_t)children[1]+0x1d8,(uintptr_t)"TAB_CONTROLLER");
 wrss((uintptr_t)children[1]+0x104,358);wrss((uintptr_t)children[1]+0x214,64);
 wr32((uintptr_t)children[2]+0x1e0,4); /* Generator has a single visible body after its header. */
}
int main(void){
#ifdef _MSC_VER
 void *image=VirtualAlloc((void*)(uintptr_t)UINT64_C(0x140000000),0x2b90000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
 void *image=mmap((void*)(uintptr_t)UINT64_C(0x140000000),0x2b90000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
 assert((uintptr_t)image==UINT64_C(0x140000000));g_image_delta=0;
#include "ui_layout_constants.inc"
 wr32(GVA(UINT64_C(0x14296FAE0)),0);vtable[0x100/8]=1;vtable[0x1c8/8]=UINT64_C(0x140F964F0);
 wr64(GVA(UINT64_C(0x141DE0A48))+0x1c8,UINT64_C(0x140F964F0));cpu.rsp=(uintptr_t)stack+sizeof(stack)-64;setup();
 flow();assert(rdss((uintptr_t)children[1]+0xc4)==225&&rdss((uintptr_t)layout+0x104)==588);
 /* Direct positioning is overwritten by the real original flow. */
 wrss((uintptr_t)children[1]+0xc0,342);wrss((uintptr_t)children[1]+0xc4,0);flow();
 assert(rdss((uintptr_t)children[1]+0xc0)==0&&rdss((uintptr_t)children[1]+0xc4)==225);
 wr32((uintptr_t)children[0]+0x1e0,4);wrss((uintptr_t)layout+0x280,342);wr8((uintptr_t)layout+0x2a9,1);flow();
 assert(rdss((uintptr_t)children[0]+0x100)==673); /* Original anchors stretch even a flow-excluded header. */
 for(unsigned n=1;n<627;++n)flow();
 assert(rdss((uintptr_t)layout+0x100)==214765&&rdss((uintptr_t)children[1]+0x100)==214765);
 setup_generator();flow();assert(rdss((uintptr_t)layout+0x104)==588);
 applied_scale=200;float generator_height=225;selection_columns(&cpu,(uintptr_t)layout,0,&generator_height,1);
 for(unsigned n=0;n<627;++n)flow();
 assert(rdss((uintptr_t)layout+0x100)==673&&rdss((uintptr_t)layout+0x104)==394&&generator_height==394);
 assert(rdss((uintptr_t)children[1]+0xc0)==342&&rdss((uintptr_t)children[1]+0xc4)==36&&rdss((uintptr_t)children[1]+0x100)==331);
 applied_scale=100;generator_height=225;selection_columns(&cpu,(uintptr_t)layout,0,&generator_height,1);flow();
 assert(rdss((uintptr_t)layout+0x100)==331&&rdss((uintptr_t)layout+0x104)==588&&generator_height==588&&rd32((uintptr_t)children[1]+0x13c)==6);
 relayouts=flag_calls=0;
 setup();flow();
 applied_scale=200;float height=225;CPU saved=cpu;assert(selection_columns(&cpu,(uintptr_t)layout,0,&height,1)==1);assert(!memcmp(&cpu,&saved,sizeof(cpu)));
 flow();assert(rdss((uintptr_t)children[0]+0xc0)==0&&rdss((uintptr_t)children[0]+0xc4)==0);
 assert(rdss((uintptr_t)children[1]+0xc0)==342&&rdss((uintptr_t)children[1]+0xc4)==36);
 assert(rdss((uintptr_t)children[2]+0xc0)==342&&rdss((uintptr_t)children[2]+0xc4)==81);
 assert(rdss((uintptr_t)layout+0x104)==394&&height==394&&rdss((uintptr_t)layout+0x100)==673);
 assert(rdss((uintptr_t)children[0]+0x100)==331&&rd32((uintptr_t)children[0]+0x13c)==0);
 assert(relayouts==1&&flag_calls==1);for(unsigned n=0;n<627;++n){height=225;selection_columns(&cpu,(uintptr_t)layout,0,&height,1);flow();}
 assert(relayouts==1&&flag_calls==1&&rdss((uintptr_t)children[2]+0xc4)==81);
 assert(rdss((uintptr_t)layout+0x100)==673&&rdss((uintptr_t)children[1]+0x100)==331&&rd32((uintptr_t)children[1]+0x13c)==0);
 assert(rdss((uintptr_t)children[3]+0x100)==331&&rd32((uintptr_t)children[3]+0x13c)==0);
 wr32((uintptr_t)children[3]+0x34,0);wr32((uintptr_t)children[3]+0x1e0,0);flow();
 assert(rdss((uintptr_t)layout+0x100)==673&&rdss((uintptr_t)children[3]+0x100)==331);
 wr32((uintptr_t)children[3]+0x34,2);wr32((uintptr_t)children[3]+0x1e0,2);flow();
 applied_scale=100;height=225;selection_columns(&cpu,(uintptr_t)layout,0,&height,1);flow();
 assert(!(rd32((uintptr_t)children[0]+0x1e0)&4)&&rdss((uintptr_t)children[1]+0xc0)==0&&rdss((uintptr_t)children[1]+0xc4)==225);
 assert(rdss((uintptr_t)layout+0x104)==583&&rdss((uintptr_t)layout+0x100)==331&&rd32((uintptr_t)children[0]+0x13c)==6&&height==583&&relayouts==2&&flag_calls==2);
 assert(rd32((uintptr_t)children[1]+0x13c)==6&&rd32((uintptr_t)children[3]+0x13c)==6);
 /* A queued anchor property may settle after a native measure; recover that one stale width. */
 setup();flow();applied_scale=200;defer_body_anchor=1;height=225;
 selection_columns(&cpu,(uintptr_t)layout,0,&height,1);flow();
 assert(pending_body_anchor==1&&rdss((uintptr_t)children[1]+0x100)==673);
 wr32((uintptr_t)children[1]+0x13c,pending_body_anchor-1);pending_body_anchor=0;defer_body_anchor=0;
 selection_columns(&cpu,(uintptr_t)layout,0,&height,1);flow();
 assert(rdss((uintptr_t)layout+0x100)==673&&rdss((uintptr_t)children[1]+0x100)==331&&rdss((uintptr_t)children[1]+0x214)==45);
 for(unsigned n=0;n<627;++n){height=225;selection_columns(&cpu,(uintptr_t)layout,0,&height,1);flow();}
 assert(rdss((uintptr_t)layout+0x100)==673&&rdss((uintptr_t)children[1]+0x100)==331&&!memcmp(&cpu,&saved,sizeof(cpu)));
 /* The residents wrapper must shrink with its table and carry the action row with it. */
 setup();wrss((uintptr_t)layout+0x104,500);wrss((uintptr_t)layout+0x214,242);
 for(unsigned i=0;i<3;++i){wrss((uintptr_t)children[i]+0x104,(float[]){16,154,30}[i]);wr32((uintptr_t)children[i]+0x13c,0);}
 flow();assert(rdss((uintptr_t)layout+0x104)==500&&rdss((uintptr_t)children[2]+0xc4)==170);
 wr8((uintptr_t)layout+0x2a9,1);flow();assert(rdss((uintptr_t)layout+0x104)==200);
 wrss((uintptr_t)children[1]+0x104,78);flow();assert(rdss((uintptr_t)layout+0x104)==124&&rdss((uintptr_t)children[2]+0xc4)==94);
 wr32((uintptr_t)children[2]+0x30,2);flow();assert(rdss((uintptr_t)layout+0x104)==124);
 wr8((uintptr_t)layout+0x2aa,1);flow();assert(rdss((uintptr_t)layout+0x104)==94);
 puts("{\"success\":true,\"real_original_flow_overwrites_manual_positions\":true,\"native_header_exclusion\":true,\"native_body_flow_retained\":true,\"native_shrink_588_to_394\":true,\"native_citizen_wrapper_shrink_500_to_200_to_124\":true,\"native_actions_follow_table_height\":true,\"hidden_children_excluded\":true,\"original_width_runaway_214765_reproduced\":true,\"stable_width_673_after_627_passes\":true,\"actual_generator_single_tab_body\":true,\"generator_restore_height_588\":true,\"hidden_body_anchors_detached\":true,\"deferred_anchor_width_recovered\":true,\"restore_scale100\":true,\"cpu_preserved\":true}");
}
