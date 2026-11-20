#include "cpu64.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int64_t g_image_delta;
static unsigned calls,shown,missing,reject;
static uint64_t string_slot,localizer;
static const uint16_t loading[]={0x0417,0x0430,0};
static const uint16_t help[]={0x0422,0xd83d,0xde00,0};
static const uint16_t label[]={0x0171,0};
#ifdef _MSC_VER
#define __attribute__(unused)
#endif
#include "mobile_overlay_text.inc"
enum { LABEL_COUNT=sizeof(overlay_label_keys)/sizeof(*overlay_label_keys), LOOKUPS=LABEL_COUNT+2 };
int frostpunk_android_touch_text(const uint16_t *a,uint32_t an,const uint16_t *b,uint32_t bn){
    assert(a==loading&&an==2&&bn==3+2*LABEL_COUNT);
    assert(!memcmp(b,help,3*sizeof(*b))&&b[3]==0x1e);
    for(unsigned i=0;i<LABEL_COUNT;++i){
        assert(b[4+2*i]==label[0]);
        if(i)assert(b[3+2*i]==0x1f);
    }
    ++shown;return !reject;
}
void dispatch(CPU *c,uint64_t target){
    const char *key=(const char*)(uintptr_t)c->rdx;
    const uint16_t *value=NULL;
    assert(target==UINT64_C(0x140E84CD0)&&c->rcx==localizer&&c->r8==GVA(UINT64_C(0x142980530))&&!c->r9);
    assert(rd64(c->rsp+0x28)==1&&!rd64(c->rsp+0x30));
    if(!strcmp(key,"UI/Menu/Buttons/Loading"))value=loading;
    else if(!strcmp(key,"UI/Android/GestureHelpText"))value=help;
    else for(unsigned i=0;i<LABEL_COUNT;++i)if(!strcmp(key,overlay_label_keys[i]))value=label;
    assert(value);++calls;
    string_slot=missing?0:(uintptr_t)value;c->rax=(uintptr_t)&string_slot;c->rsp+=8;
}
int main(void){
    _Alignas(16) unsigned char image[0x6000]={0},stack[2048],localization[0x100]={0},config[0x60]={0};CPU cpu={0};
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x142980000);
    localizer=(uintptr_t)localization;wr64(localizer+0x60,1);wr32(localizer+0x80,1);
    wr64(GVA(UINT64_C(0x142983988)),localizer);wr64(GVA(UINT64_C(0x142985AB0)),(uintptr_t)config);
    cpu.rsp=(uintptr_t)stack+sizeof(stack)-32;cpu.rcx=99;cpu.rdx=100;CPU before=cpu;
    mobile_overlay_text(&cpu);assert(shown==1&&calls==LOOKUPS&&!memcmp(&cpu,&before,sizeof(cpu)));
    for(unsigned i=0;i<1000;++i)mobile_overlay_text(&cpu);
    assert(shown==1&&calls==LOOKUPS);
    wr64(localizer+0x90,123);mobile_overlay_text(&cpu);assert(shown==2&&calls==2*LOOKUPS);
    wr64(localizer+0x98,456);mobile_overlay_text(&cpu);assert(shown==3&&calls==3*LOOKUPS);
    wr64((uintptr_t)config+0x40,789);mobile_overlay_text(&cpu);assert(shown==4&&calls==4*LOOKUPS);
    reject=1;wr64(localizer+0x90,124);mobile_overlay_text(&cpu);assert(shown==5&&calls==5*LOOKUPS);
    reject=0;mobile_overlay_text(&cpu);assert(shown==6&&calls==6*LOOKUPS);
    missing=1;wr64(localizer+0x90,125);mobile_overlay_text(&cpu);assert(shown==6&&calls==6*LOOKUPS+1);
    missing=0;mobile_overlay_text(&cpu);assert(shown==7&&calls==7*LOOKUPS+1);
    wr64(GVA(UINT64_C(0x142983988)),0);mobile_overlay_text(&cpu);assert(shown==7&&calls==7*LOOKUPS+1);
    assert(!memcmp(&cpu,&before,sizeof(cpu)));
    puts("{\"success\":true,\"original_active_localizer\":true,\"language_switch\":true,\"configured_language_override\":true,\"unchanged_language_no_lookup_or_jni\":true,\"utf16_surrogates_retained\":true,\"cpu_preserved\":true,\"failed_delivery_retried\":true}");
}
