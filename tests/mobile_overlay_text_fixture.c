#include "cpu64.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int64_t g_image_delta;
static unsigned calls,shown,missing,reject;
static uint64_t string_slot,localizer;
static const uint16_t loading[]={0x0417,0x0430,0};
static const uint16_t help[]={0x0422,0xd83d,0xde00,0};
int frostpunk_android_touch_text(const uint16_t *a,uint32_t an,const uint16_t *b,uint32_t bn){
    assert(a==loading&&an==2&&b==help&&bn==3);++shown;return !reject;
}
void dispatch(CPU *c,uint64_t target){
    const char *keys[]={"UI/Menu/Buttons/Loading","UI/Android/GestureHelpText"};
    assert(target==UINT64_C(0x140E84CD0)&&c->rcx==localizer&&c->r8==GVA(UINT64_C(0x142980530))&&!c->r9);
    assert(rd64(c->rsp+0x28)==1&&!rd64(c->rsp+0x30));
    unsigned key=!strcmp((const char*)(uintptr_t)c->rdx,keys[1]);
    assert(key||!strcmp((const char*)(uintptr_t)c->rdx,keys[0]));++calls;
    string_slot=missing?0:(uintptr_t)(key?help:loading);c->rax=(uintptr_t)&string_slot;c->rsp+=8;
}
#ifdef _MSC_VER
#define __attribute__(unused)
#endif
#include "mobile_overlay_text.inc"
int main(void){
    _Alignas(16) unsigned char image[0x6000]={0},stack[2048],localization[0x100]={0},config[0x60]={0};CPU cpu={0};
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x142980000);
    localizer=(uintptr_t)localization;wr64(localizer+0x60,1);wr32(localizer+0x80,1);
    wr64(GVA(UINT64_C(0x142983988)),localizer);wr64(GVA(UINT64_C(0x142985AB0)),(uintptr_t)config);
    cpu.rsp=(uintptr_t)stack+sizeof(stack)-32;cpu.rcx=99;cpu.rdx=100;CPU before=cpu;
    mobile_overlay_text(&cpu);assert(shown==1&&calls==2&&!memcmp(&cpu,&before,sizeof(cpu)));
    for(unsigned i=0;i<1000;++i)mobile_overlay_text(&cpu);
    assert(shown==1&&calls==2);
    wr64(localizer+0x90,123);mobile_overlay_text(&cpu);assert(shown==2&&calls==4);
    wr64(localizer+0x98,456);mobile_overlay_text(&cpu);assert(shown==3&&calls==6);
    wr64((uintptr_t)config+0x40,789);mobile_overlay_text(&cpu);assert(shown==4&&calls==8);
    reject=1;wr64(localizer+0x90,124);mobile_overlay_text(&cpu);assert(shown==5&&calls==10);
    reject=0;mobile_overlay_text(&cpu);assert(shown==6&&calls==12);
    missing=1;wr64(localizer+0x90,125);mobile_overlay_text(&cpu);assert(shown==6&&calls==13);
    missing=0;mobile_overlay_text(&cpu);assert(shown==7&&calls==15);
    wr64(GVA(UINT64_C(0x142983988)),0);mobile_overlay_text(&cpu);assert(shown==7&&calls==15);
    assert(!memcmp(&cpu,&before,sizeof(cpu)));
    puts("{\"success\":true,\"original_active_localizer\":true,\"language_switch\":true,\"configured_language_override\":true,\"unchanged_language_no_lookup_or_jni\":true,\"utf16_surrogates_retained\":true,\"cpu_preserved\":true,\"failed_delivery_retried\":true}");
}
