#include "cpu64.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _MSC_VER
#define __attribute__(unused)
#endif
typedef int fixture_mutex_t;
#define pthread_mutex_t fixture_mutex_t
#define PTHREAD_MUTEX_INITIALIZER 0
static void fixture_mutex_lock(fixture_mutex_t *p){(void)p;}
static void fixture_mutex_unlock(fixture_mutex_t *p){(void)p;}
#define pthread_mutex_lock fixture_mutex_lock
#define pthread_mutex_unlock fixture_mutex_unlock
int64_t g_image_delta;
static unsigned calls,regions,entered;
static _Alignas(16) unsigned char image[0x140000],editor[0x800],stack[32768];
static uint64_t nodes[4];static uint16_t buffer[300];
void win64_region_add(uint64_t address,size_t bytes,int writable){assert(address&&bytes<=514&&!writable);++regions;}
void win64_region_remove(uint64_t address){assert(address&&regions);--regions;}
void win64_window_ime_key(uint32_t key,uint32_t scan){
    assert(key==13&&scan==28&&rd32((uintptr_t)editor+0x680)==4);
    assert(buffer[0]=='A'&&buffer[1]==0x0416&&buffer[2]==0xd83d&&buffer[3]==0xde04&&!buffer[4]);++entered;
}
#include "text_editor_original.inc"
void dispatch(CPU *c,uint64_t target){
    ++calls;
    switch(target){
#include "text_editor_dispatch.inc"
    case UINT64_C(0x141AE320A):memcpy((void*)(uintptr_t)c->rcx,(void*)(uintptr_t)c->rdx,(size_t)c->r8);c->rax=c->rcx;c->rsp+=8;return;
    case UINT64_C(0x141AE3210):memmove((void*)(uintptr_t)c->rcx,(void*)(uintptr_t)c->rdx,(size_t)c->r8);c->rax=c->rcx;c->rsp+=8;return;
    case UINT64_C(0x140FB55E0):c->rsp+=8;return; /* Text rendering is outside this storage/cursor oracle. */
    default:fprintf(stderr,"unexpected original text call=%llx\n",(unsigned long long)target);assert(0);
    }
}
#include "text_input_production.inc"
static CPU cpu;
static void setup(const uint16_t *text,unsigned units,unsigned cursor,int selection){
    frostpunk_mobile_text_cancel();memset(editor,0,sizeof(editor));memset(buffer,0,sizeof(buffer));
    memcpy(buffer,text,units*2);wr64((uintptr_t)editor,GVA(UINT64_C(0x141DAC248)));
    wr32((uintptr_t)editor+0x1e4,17);wr8((uintptr_t)editor+0x6c8,1);wr8((uintptr_t)editor+0x6c9,(uint8_t)selection);
    wr64((uintptr_t)editor+0x678,(uintptr_t)buffer);wr32((uintptr_t)editor+0x680,units);wr32((uintptr_t)editor+0x684,256);wr32((uintptr_t)editor+0x688,cursor);
    nodes[0]=(uintptr_t)editor;nodes[1]=17;wr32(GVA(UINT64_C(0x142A89090)),1);
    fp_mobile_text_process(&cpu);
}
static void expect(const uint16_t *text,unsigned units,unsigned cursor){
    assert(rd32((uintptr_t)editor+0x680)==units&&rd32((uintptr_t)editor+0x688)==cursor&&!memcmp(buffer,text,units*2)&&!buffer[units]);
    assert(!regions);
}
static void process(void){CPU before=cpu;fp_mobile_text_process(&cpu);assert(!memcmp(&cpu,&before,sizeof(cpu)));}
int main(void){
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x142960000);
    wr64(GVA(UINT64_C(0x142A89088)),(uintptr_t)nodes);cpu.rsp=(uintptr_t)stack+sizeof(stack)-64;
    const uint16_t text[]={'A',0xd83d,0xde00,'B',0xd83d,0xde03,'C'};
    assert(fp_mobile_ime_visible_fraction()==1);frostpunk_mobile_ime_viewport(.34f);assert(fp_mobile_ime_visible_fraction()==.34f);
    frostpunk_mobile_ime_viewport(NAN);frostpunk_mobile_ime_viewport(-1);frostpunk_mobile_ime_viewport(2);assert(fp_mobile_ime_visible_fraction()==.34f);
    const uint16_t removed[]={'A',0xd83d,0xde03,'C'};
    for(int selection=0;selection<2;++selection){
        setup(text,7,3,selection);assert(frostpunk_mobile_text_submit(2,NULL,0,1,1));process();expect(removed,4,1);
    }
    setup(text,7,3,1);wr32((uintptr_t)editor+0x690,3);wr32((uintptr_t)editor+0x694,1);
    assert(frostpunk_mobile_text_submit(2,NULL,0,1,1));process();
    const uint16_t selected[]={'A','B','C'};expect(selected,3,1);assert(rd32((uintptr_t)editor+0x690)==1&&rd32((uintptr_t)editor+0x694)==1);
    const uint16_t insert[]={0x0416,0xd83d,0xde04};
    setup(selected,3,1,1);wr32((uintptr_t)editor+0x690,1);wr32((uintptr_t)editor+0x694,1);
    assert(frostpunk_mobile_text_submit(0,insert,3,0,0));process();
    const uint16_t inserted[]={'A',0x0416,0xd83d,0xde04,'C'};expect(inserted,5,4);
    const uint16_t bad[]={0xd83d};assert(!frostpunk_mobile_text_submit(0,bad,1,0,0));
    setup(text,7,2,1);assert(frostpunk_mobile_text_submit(2,NULL,0,1,0));process();expect(text,7,2);
    setup(text,7,3,1);assert(frostpunk_mobile_text_submit(2,NULL,0,1,0));wr32((uintptr_t)editor+0x1e4,18);process();expect(text,7,3);
    setup(text,7,3,1);assert(frostpunk_mobile_text_submit(2,NULL,0,1,0));frostpunk_mobile_text_cancel();process();expect(text,7,3);
    assert(fp_mobile_ime_visible_fraction()==1&&fp_mobile_text_focused_editor()==(uintptr_t)editor);
    setup(text,7,3,1);for(unsigned i=0;i<16;++i)assert(frostpunk_mobile_text_submit(2,NULL,0,0,0));assert(!frostpunk_mobile_text_submit(2,NULL,0,0,0));process();expect(text,7,3);
    const uint16_t single[]={'A'},pair[]={0xd83d,0xde00,0};
    setup(single,1,1,1);wr32((uintptr_t)editor+0x684,2);
    CPU original=cpu;original.rcx=(uintptr_t)editor;original.rdx=(uintptr_t)pair;
    push64(&original,0);dispatch(&original,UINT64_C(0x140FC30A0));
    assert(rd32((uintptr_t)editor+0x680)==2&&buffer[1]==0xd83d&&!buffer[2]);
    setup(single,1,1,1);wr32((uintptr_t)editor+0x684,2);
    assert(frostpunk_mobile_text_submit(0,pair,2,0,0));process();expect(single,1,1);
    setup(text,7,1,1);wr32((uintptr_t)editor+0x684,7);
    const uint16_t letter[]={'Z'};assert(frostpunk_mobile_text_submit(0,letter,1,0,0));process();expect(text,7,1);
    setup(text,7,2,1);assert(frostpunk_mobile_text_submit(0,letter,1,0,0));process();expect(text,7,2);
    setup(text,7,3,1);wr32((uintptr_t)editor+0x690,2);wr32((uintptr_t)editor+0x694,1);
    assert(frostpunk_mobile_text_submit(0,letter,1,0,0));process();expect(text,7,3);
    const uint16_t ac[]={'A','C'},partial[]={'Z',0xd83d,0xde00,'Q'},azc[]={'A','Z','C'};
    setup(ac,2,1,1);wr32((uintptr_t)editor+0x684,4);
    assert(frostpunk_mobile_text_submit(0,partial,4,0,0));process();expect(azc,3,2);
    setup(single,1,1,1);assert(frostpunk_mobile_text_submit(0,insert,3,0,0));
    assert(frostpunk_mobile_text_submit(3,NULL,0,13,28)&&frostpunk_mobile_text_editing());process();assert(entered==1);
    setup(text,7,3,1);assert(frostpunk_mobile_text_submit(2,NULL,0,1,0));
    wr32((uintptr_t)editor+0x1e0,2);process();expect(text,7,3);assert(!frostpunk_mobile_text_submit(2,NULL,0,1,0)&&!frostpunk_mobile_text_editing()&&!fp_mobile_text_focused_editor());
    puts("{\"success\":true,\"real_original_insert_delete_cursor_selection\":true,\"codepoint_before_after\":true,\"selection_retained\":true,\"invalid_utf16_rejected\":true,\"max_fit_preserves_pairs_and_tail\":true,\"inside_pair_boundaries_rejected\":true,\"insert_applied_before_enter\":true,\"focus_reuse_and_cancel\":true,\"queue_bound\":true,\"cpu_preserved\":true}");
}
