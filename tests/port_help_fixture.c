#include "cpu64.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int64_t g_image_delta;
static unsigned calls,shown,missing;
static uint64_t string_slot;
static const uint16_t title[]={0x0421,0x043f,0};
static const uint16_t body[]={0x0422,0xd83d,0xde00,0};
static const uint16_t close_text[]={0x0417,0};
void frostpunk_android_show_help(const uint16_t *a,uint32_t an,const uint16_t *b,uint32_t bn,const uint16_t *c,uint32_t cn){
    assert(a==title&&an==2&&b==body&&bn==3&&c==close_text&&cn==1);++shown;
}
void dispatch(CPU *c,uint64_t target){
    const char *keys[]={"UI/Android/GestureHelp","UI/Android/GestureHelpText","UI/Android/GestureHelpClose"};
    const uint16_t *texts[]={title,body,close_text};
    assert(target==UINT64_C(0x140E84CD0)&&c->rcx==1234&&c->r8==GVA(UINT64_C(0x142980530))&&!c->r9);
    assert(rd64(c->rsp+0x28)==1&&rd64(c->rsp+0x30)==1);
    assert(calls<3&&!strcmp((const char*)(uintptr_t)c->rdx,keys[calls]));
    string_slot=missing?0:(uintptr_t)texts[calls++];c->rax=(uintptr_t)&string_slot;c->rsp+=8;
}
#ifdef _MSC_VER
#define __attribute__(unused)
#endif
#include "port_help.inc"
int main(void){
    _Alignas(16) unsigned char image[0x4000]={0},stack[2048];CPU cpu={0};
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x142980000);
    wr64(GVA(UINT64_C(0x142983988)),1234);cpu.rsp=(uintptr_t)stack+sizeof(stack)-32;
    cpu.rcx=99;cpu.rdx=100;cpu.r8=101;cpu.r9=102;CPU before=cpu;
    fp_port_gesture_help(&cpu);assert(shown==1&&calls==3&&!memcmp(&cpu,&before,sizeof(cpu)));
    calls=0;missing=1;fp_port_gesture_help(&cpu);assert(shown==1&&!memcmp(&cpu,&before,sizeof(cpu)));
    wr64(GVA(UINT64_C(0x142983988)),0);fp_port_gesture_help(&cpu);assert(shown==1);
    puts("{\"success\":true,\"original_active_localizer\":true,\"utf16_surrogates_retained\":true,\"cpu_preserved\":true,\"missing_localization_closed\":true}");
}
