#include "cpu64.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t g_image_delta;
void L_000141086C30(CPU *c);
static unsigned width[16],height[16],levels,bound_level,dispatches,overruns,wrong_sizes;
static unsigned char *child;
static unsigned baseline;

void dispatch(CPU *c,uint64_t target){
    if(target==0x140F7A220ull){
        for(unsigned i=0;i<3;++i){
            unsigned divisor=rd32(c->rdx+i*4),value=rd32(c->r8+i*4);
            assert(divisor);wr32(c->rcx+i*4,(value+divisor-1)/divisor);
        }
        c->rax=c->rcx;
    }else if(target==0x140F00CB0ull){
        uint64_t output=rd64(c->rcx+488);
        bound_level=rd32(output+16);
        if(bound_level>=levels)++overruns;
    }else if(target==0x1001){
        assert(bound_level<16);
        if(rd32(c->r8)!=width[bound_level]||rd32(c->r8+4)!=height[bound_level])++wrong_sizes;
        assert(rd32(c->r8+8)==1);++dispatches;
    }else if(target==0x140F01550ull){
        assert(c->rcx==(uintptr_t)child);
        L_000141086C30(c);return;
    }else if(target==0x1002){
        assert(width[levels-1]==1&&height[levels-1]==1);
    }else{
        assert(target==0x140E36040ull||target==0x140F4CD60ull);
    }
    c->rsp+=8;
}
#include "depth-bodies.inc"

int main(int argc,char **argv){
    assert(argc==2);baseline=!strcmp(argv[1],"baseline");
    unsigned char *image=calloc(1,0x4539000),*renderer=calloc(1,1024),*context=calloc(1,10000);
    unsigned char *node=calloc(1,1600),*stack=calloc(1,8192),*manager=calloc(1,600);
    child=calloc(1,1600);
    assert(image&&renderer&&context&&node&&child&&stack&&manager);
    g_image_delta=(intptr_t)image-0x140000000ull;
    wr64(GVA(0x142982A70),(uintptr_t)renderer);wr64(GVA(0x142B3B598),(uintptr_t)context);
    wr64(GVA(0x142982A78),(uintptr_t)manager);wr32(GVA(0x141D682CC),16);
    uint64_t vt[64]={0},first_in[3]={4,0,0},first_out[3]={4,0,0},next_in[3]={4,0,0},next_out[3]={4,0,0};
    vt[168/8]=0x1001;vt[472/8]=0x1002;wr64((uintptr_t)renderer,(uintptr_t)vt);
    wr64((uintptr_t)node+64,(uintptr_t)first_in);wr64((uintptr_t)node+488,(uintptr_t)first_out);
    wr64((uintptr_t)child+64,(uintptr_t)next_in);wr64((uintptr_t)child+488,(uintptr_t)next_out);
    wr64((uintptr_t)node+1280,(uintptr_t)child);wr32((uintptr_t)node+1336,3);
    uint64_t link0[2]={0},link1[8]={0},link2[6]={0};
    link0[1]=(uintptr_t)link1;link1[7]=(uintptr_t)link2;
    wr64((uintptr_t)node+1000,(uintptr_t)link0);wr64((uintptr_t)child+1000,(uintptr_t)link0);
    uint64_t textures[16]={0};wr64((uintptr_t)manager+528,(uintptr_t)textures);
    unsigned cases=0;
    const unsigned widths[]={240,255,256,257,480,600,1200,2400};
    const unsigned heights[]={108,144,255,256,257,540,1080};
    for(unsigned wi=0;wi<8;++wi)for(unsigned hi=0;hi<7;++hi)for(unsigned scale=50;scale<=100;scale+=10){
        unsigned w=widths[wi],h=heights[hi];levels=0;
        wr32((uintptr_t)renderer+0xb4,w);wr32((uintptr_t)renderer+0xb8,h);
        wr32(GVA(0x142983064),w*100/scale);wr32(GVA(0x142983068),h*100/scale);
        while(w>1||h>1){w=(w+15)/16;h=(h+15)/16;width[levels]=w;height[levels++]=h;}
        wr32((uintptr_t)manager+536,levels);
        CPU c={0};c.rsp=(uintptr_t)stack+8000;c.rcx=(uintptr_t)node;
        unsigned before=dispatches;L_000141086940(&c);
        assert(c.rsp==(uintptr_t)stack+8008);
        if(!baseline)assert(dispatches-before==levels&&!overruns&&!wrong_sizes);
        ++cases;
    }
    assert(baseline?(overruns&&wrong_sizes):(!overruns&&!wrong_sizes));
    printf("{\"success\":true,\"cases\":%u,\"baseline\":%u,\"out_of_range_bindings\":%u,\"wrong_dispatch_sizes\":%u}\n",cases,baseline,overruns,wrong_sizes);
    return 0;
}
