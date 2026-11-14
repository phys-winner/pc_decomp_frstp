#include "temperature_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int64_t g_image_delta;
static unsigned calls;
static unsigned char tree[8][0x280],unrelated[0x280];
static uint64_t target;
void dispatch(CPU *cpu,uint64_t address){
    assert(address==UINT64_C(0x140EBE470)&&cpu->rcx==target&&cpu->rdx==UINT64_C(0x80000000)&&cpu->r8==0);
    assert((cpu->rsp&15)==8&&pop64(cpu)==UINT64_C(0xe5ee0010));
    wr32(target+0x1e0,rd32(target+0x1e0)&~UINT32_C(0x80000000));++calls;
}
static void setup(void){
    static const char *const names[]={"","FRAME","HUD","TOP","EXT_RESOURCES_PANEL","ANI","TEMPERATURE_OVERLAY_BUTTON","freeze_ball_blur"};
    memset(tree,0,sizeof(tree));memset(unrelated,0,sizeof(unrelated));
    for(unsigned i=0;i<8;++i){
        wr64((uintptr_t)tree[i]+0x1d8,(uintptr_t)names[i]);
        wr32((uintptr_t)tree[i]+0x1e0,UINT32_C(0x84100200));
        if(i<7)wr64((uintptr_t)tree[i]+0x220,(uintptr_t)tree[i+1]);
    }
    target=(uintptr_t)tree[7];wr64(target,GVA(UINT64_C(0x141DA2AA8)));
    wr64((uintptr_t)unrelated+0x1d8,(uintptr_t)"freeze_ball_blur");
    wr64((uintptr_t)unrelated,GVA(UINT64_C(0x141DA2AA8)));wr32((uintptr_t)unrelated+0x1e0,UINT32_C(0x84100200));
    wr64((uintptr_t)tree[0]+0x220,(uintptr_t)unrelated);wr64((uintptr_t)unrelated+0x240,(uintptr_t)tree[1]);
}
int main(void){
    _Alignas(16) unsigned char stack[4096];memset(stack,0,sizeof(stack));
    CPU cpu={0};memset(&cpu,0x5a,sizeof(cpu));cpu.rsp=(uintptr_t)stack+sizeof(stack)-32;cpu.rcx=(uintptr_t)tree[0];
    unsigned char saved_tree[sizeof(tree)],saved_other[sizeof(unrelated)];
    for(unsigned cycle=0;cycle<1024;++cycle){
        setup();g_image_delta=cycle&1?INT64_C(0x100000000):0;wr64(target,GVA(UINT64_C(0x141DA2AA8)));
        memcpy(saved_tree,tree,sizeof(tree));memcpy(saved_other,unrelated,sizeof(unrelated));CPU before=cpu;unsigned count=calls;
        fp_temperature_ui_before(&cpu);assert(calls==count+1&&!memcmp(&before,&cpu,sizeof(cpu)));
        wr32((uintptr_t)saved_tree+7*sizeof(tree[0])+0x1e0,UINT32_C(0x04100200));
        assert(!memcmp(saved_tree,tree,sizeof(tree))&&!memcmp(saved_other,unrelated,sizeof(unrelated)));
        fp_temperature_ui_before(&cpu);assert(calls==count+1);
    }
    unsigned negatives=0;
    for(unsigned missing=0;missing<7;++missing){setup();wr64((uintptr_t)tree[missing]+0x220,0);unsigned count=calls;fp_temperature_ui_before(&cpu);assert(calls==count);++negatives;}
    setup();wr64(target,GVA(UINT64_C(0x141D78A60)));unsigned count=calls;fp_temperature_ui_before(&cpu);assert(calls==count);++negatives;
    setup();wr64(target+0x1d8,(uintptr_t)"temperature_background");fp_temperature_ui_before(&cpu);assert(calls==count);++negatives;
    setup();wr64(target+0x1d8,0);fp_temperature_ui_before(&cpu);assert(calls==count);++negatives;
    setup();wr64((uintptr_t)tree[1]+0x1d8,(uintptr_t)"other");wr64((uintptr_t)tree[1]+0x240,(uintptr_t)tree[1]);fp_temperature_ui_before(&cpu);assert(calls==count);++negatives;
    cpu.rcx=0;fp_temperature_ui_before(&cpu);assert(calls==count);++negatives;
    printf("{\"success\":true,\"temperature_cycles\":1024,\"negative_controls\":%u,\"other_ui_unchanged\":true,\"guest_cpu_preserved\":true,\"no_repeated_commands\":true}\n",negatives);
}
