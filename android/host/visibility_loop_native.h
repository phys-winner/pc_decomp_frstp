#pragma once
#include "visibility_filter_native.h"

/* Restricted accesses are CPU-disjoint; aliased loads resume outside this helper. */
static __attribute__((always_inline)) inline unsigned fp_visibility_filter_loop(CPU *restrict c, unsigned *rejected_run) {
    if(!*rejected_run){
        uint64_t remaining=c->r12-c->r15;
        if(c->r15>=c->r12 || remaining>UINT32_MAX/8u || cpu_alias(c->rbx,(unsigned)(remaining*8),c) ||
           cpu_alias(c->rsp+0x28,8,c) || cpu_alias(c->rbp+0x67,0x20,c)){c->rip=UINT64_C(0x140f8f650);return 2;}
        *rejected_run=1;
    }
    uint64_t rax = c->rax, rcx = c->rcx, rdx = c->rdx, rdi = c->rdi;
    const uint64_t list=c->rbx, stack=c->rsp, frame=c->rbp;
    uint64_t rip = UINT64_C(0x140f8f650), a = 0, b = 0, class_id = 0, mask = 0;
    unsigned pending = FLAGS_KEEP, width = 8, result;
#define FINISH(value) do { result = (value); goto publish; } while (0)
#define ACCESS(address, bytes, checkpoint) do { if (cpu_alias((address), (bytes), c)) FINISH(checkpoint); } while (0)
    
    rdi = rd64(list);
    ACCESS(rdi + 0x40, 0x209, 3);
    
    wr64(stack + 0x28, rdi);
    
    class_id = rcx = rd8(rdi + 0x248);
    rdx = class_id & 31;
    rax = class_id >> 5;
    pending = FLAGS_SHIFT;
    
    rcx = rd64(frame + 0x67);
    ACCESS(rcx + rax * 4, 4, 6);
    mask = rax = rd32(rcx + rax * 4);
    pending = FLAGS_BIT;
    if (!((mask >> (class_id & 31)) & 1)) FINISH(0);
    
    rcx = rax = rd64(rdi + 0x58);
    
    rdx = rd64(frame + 0x7f);
    ACCESS(rdx, 8, 9);
    rax &= rd64(rdx);
    a = rax; width = 8; pending = FLAGS_LOGIC;
    if (rax) FINISH(0);
    
    rdx = rd64(rdi + 0x40);
    a = rdx;
    while (rdx) {
        rip = UINT64_C(0x140f8f6a0);
        ACCESS(rdx + 0x40, 0x20, 11);
        rax = rd64(rdx + 0x58);
        rax = (uint32_t)rax & 0x10u;
        a = rax; width = 4;
        if (rax) FINISH(0);
        
        rdx = rd64(rdx + 0x40);
        a = rdx; width = 8;
    }
    rip = UINT64_C(0x140f8f6b6);
    
    rax = rd32(frame + 0x6f);
    
    a = rd32(rdi + 0x170) & (uint32_t)rax;
    width = 4;
    if (a) FINISH(0);
    
    rax = rd32(frame + 0x77);
    
    a = rd32(rdi + 0x174); b = (uint32_t)rax; pending = FLAGS_COMPARE;
    FINISH((int32_t)(uint32_t)a > (int32_t)(uint32_t)b ? 0 : 1);
publish:
    c->rax = rax; c->rcx = rcx; c->rdx = rdx; c->rdi = rdi; c->rip = rip;
    /* A reject reaches ADD/INC/CMP before a load or callback; passing and aliased paths reset the run. */
    if(!result)return result;
    *rejected_run=0;
    /* Guest loads cannot observe these deferred flags: every access was checked. */
    switch (pending) {
    case FLAGS_SHIFT: (void)op_shr(c, class_id, 5, 8); break;
    case FLAGS_BIT:
        (void)op_shr(c, class_id, 5, 8);
        op_bt(c, mask, class_id & 31, 4); break;
    case FLAGS_LOGIC: (void)flags_logicz(c, a, width); break;
    case FLAGS_COMPARE: (void)flags_sub(c, a, b, 4); break;
    default: break;
    }
    return result;
#undef ACCESS
#undef FINISH
}

/* Batch rejects while no guest access can observe deferred arithmetic flags. */
static __attribute__((always_inline)) inline unsigned fp_visibility_filter_batch(CPU *restrict c) {
    if(cpu_alias(c->rbp+0x57,8,c)){c->rip=UINT64_C(0x140f8f650);return 2;}
    unsigned run=0,deferred=0;
    for(;;){
        unsigned result=fp_visibility_filter_loop(c,&run);
        if(result){
            if(deferred&&result<=3)(void)flags_sub(c,c->r15,c->r12,8);
            return result;
        }
        if(!run)return 0;
        c->rip=UINT64_C(0x140f8f87f);
        c->rbx+=8;
        ++c->r15;
        /* SHR/BT and logical operations retain AF at aliased checkpoints. */
        c->af=((c->r15^c->r12^(c->r15-c->r12))>>4)&1;
        c->r14=rd64(c->rbp+0x57);
        if(c->r15==c->r12){(void)flags_sub(c,c->r15,c->r12,8);return 17;}
        deferred=1;
    }
}
