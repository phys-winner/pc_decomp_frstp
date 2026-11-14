#include "cpu64.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _MSC_VER
#define __attribute__(unused)
#endif
int64_t g_image_delta;
static _Alignas(16) unsigned char nodes[3][0x400],vtables[3][0x200],stack[32768],renderer[0x700];
static unsigned self_calls,child_calls,visits,cases;
static float expected_x[2],expected_y;
uint64_t frostpunk_teb_read(uint64_t off,unsigned bytes){(void)off;(void)bytes;assert(0);return 0;}
void frostpunk_teb_write(uint64_t off,uint64_t value,unsigned bytes){(void)off;(void)value;(void)bytes;assert(0);}
void frostpunk_unsupported(uint64_t va,const char *text){fprintf(stderr,"unexpected instruction %llx %s\n",(unsigned long long)va,text);assert(0);}
#include "ui_self_original.inc"
void dispatch(CPU *c,uint64_t target){
    for(unsigned i=0;i<3;++i)if(target==GVA(UINT64_C(0x150000010)+i*16)){target-=g_image_delta;break;}
    switch(target){
#include "ui_self_dispatch.inc"
    case UINT64_C(0x140F97150):++visits;break; /* No pending UI actions in this fixture. */
    case UINT64_C(0x140F9A0B0):wr8(c->rdx,0);break; /* No additional camera/shadow override. */
    case UINT64_C(0x140F9A4D0):break; /* Sizes are supplied; attachment still uses the original link writes. */
    case UINT64_C(0x140ECB500):c->rax=0;break; /* No pending global alpha mask. */
    case UINT64_C(0x150000010):++self_calls;break;
    case UINT64_C(0x150000020):
    case UINT64_C(0x150000030):{
        unsigned i=target==UINT64_C(0x150000020)?0:1;
        assert(c->rcx==(uintptr_t)nodes[i+1]);
        assert(fabsf(rdss(c->rdx+48)-expected_x[i])<.001f&&fabsf(rdss(c->rdx+52)-expected_y)<.001f);
        assert(fabsf(rdss(c->rdx)-1.4f)<.001f&&fabsf(rdss(c->rdx+20)-1.4f)<.001f);
        ++child_calls;break;
    }
    default:fprintf(stderr,"unexpected original UI dispatch %llx\n",(unsigned long long)target);assert(0);
    }
    c->rsp+=8;
}
static void matrix(uint64_t address,float x,float y,float scale){
    float m[16]={scale,0,0,0,0,scale,0,0,0,0,1,0,x,y,0,1};memcpy((void*)(uintptr_t)address,m,64);
}
static void invoke(CPU *cpu,uint64_t target){push64(cpu,0);dispatch(cpu,target);}
static void check(unsigned flag,unsigned child_flag){
    memset(nodes,0,sizeof(nodes));memset(vtables,0,sizeof(vtables));
    for(unsigned i=0;i<3;++i){
        uint64_t node=(uintptr_t)nodes[i];wr64(node,(uintptr_t)vtables[i]);
        wr64((uintptr_t)vtables[i]+480,UINT64_C(0x150000010)+i*16+g_image_delta);
        wrss(node+256,i?30:1280);wrss(node+260,i?30:720);
        wr32(node+64,1);
        for(unsigned j=0;j<4;++j)wrss(node+288+j*4,1);
        matrix(node+80,i?(i==1?876:934):-606,i?655:-345,i?1.4f:1);
    }
    CPU cpu={.rsp=(uintptr_t)stack+sizeof(stack)-512};uint64_t original_rsp=cpu.rsp;
    cpu.rcx=(uintptr_t)nodes[0];cpu.rdx=(uintptr_t)nodes[1];invoke(&cpu,UINT64_C(0x140F951F0));
    cpu.rcx=(uintptr_t)nodes[0];cpu.rdx=(uintptr_t)nodes[2];invoke(&cpu,UINT64_C(0x140F951F0));
    assert(rd64((uintptr_t)nodes[0]+344)==(uintptr_t)nodes[1]&&rd64((uintptr_t)nodes[1]+376)==(uintptr_t)nodes[2]);
    assert(rd64((uintptr_t)nodes[1]+360)==(uintptr_t)nodes[0]&&rd64((uintptr_t)nodes[2]+360)==(uintptr_t)nodes[0]);
    cpu.rcx=(uintptr_t)nodes[0];cpu.rdx=flag;cpu.r8=0;invoke(&cpu,UINT64_C(0x140F994A0));
    assert(rd32((uintptr_t)nodes[0]+48)==flag&&!rd32((uintptr_t)nodes[1]+48)&&!rd32((uintptr_t)nodes[2]+48));
    wr32((uintptr_t)nodes[1]+48,child_flag);
    self_calls=child_calls=visits=0;expected_x[0]=270;expected_x[1]=328;expected_y=310;
    _Alignas(16) float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},color[4]={1,1,1,1};
    cpu.rcx=(uintptr_t)nodes[0];cpu.rdx=(uintptr_t)identity;cpu.r8=(uintptr_t)color;cpu.r9=0;
    wr32(cpu.rsp+32,0);invoke(&cpu,UINT64_C(0x140F9B260));
    assert(cpu.rsp==original_rsp);
    if(flag&2){assert(!self_calls&&!child_calls&&visits==1);}
    else{if(self_calls!=!(flag&1))fprintf(stderr,"flag=%u childflag=%u self=%u child=%u visits=%u\n",flag,child_flag,self_calls,child_calls,visits);assert(self_calls==!(flag&1));assert(child_calls==((child_flag&1||child_flag&0x4002)?1:2));}
    assert(rd64((uintptr_t)nodes[0]+344)==(uintptr_t)nodes[1]&&rd64((uintptr_t)nodes[1]+376)==(uintptr_t)nodes[2]);
    ++cases;
}
int main(void){
    unsigned char *image=calloc(1,0x4539000);assert(image);g_image_delta=(intptr_t)image-INT64_C(0x140000000);
#include "ui_self_constants.inc"
    wr32(GVA(UINT64_C(0x14296FAE0)),0);wr64(GVA(UINT64_C(0x142982AC0)),(uintptr_t)renderer);
    const unsigned flags[]={0,1,2,3};
    for(unsigned repeat=0;repeat<32;++repeat)for(unsigned i=0;i<4;++i){check(flags[i],0);check(flags[i],1);check(flags[i],2);check(flags[i],0x4000);}
    printf("{\"success\":true,\"original_traversal\":true,\"original_matrix_multiply\":true,\"original_attach\":true,\"original_set_flag\":true,\"cases\":%u,\"self_flag_preserves_children\":true,\"hidden_flag_blocks_children\":true,\"incoming_2d_matrix_preserved\":true}\n",cases);
    free(image);return 0;
}
