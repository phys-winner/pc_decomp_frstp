#pragma once
#include "cpu64.h"

enum { FLAGS_KEEP, FLAGS_SHIFT, FLAGS_BIT, FLAGS_LOGIC, FLAGS_COMPARE };

static inline int cpu_alias(uint64_t address, unsigned size, const CPU *c) {
    uint64_t cpu = (uintptr_t)c;
    return address > UINT64_MAX - size || (address < cpu + sizeof(*c) && cpu < address + size);
}

/* Checkpoint numbers resume immediately before the original aliased access. */
static __attribute__((always_inline)) inline unsigned fp_visibility_filter(CPU *c) {
    uint64_t rax = c->rax, rcx = c->rcx, rdx = c->rdx, rdi = c->rdi;
    const uint64_t list=c->rbx, stack=c->rsp, frame=c->rbp;
    uint64_t rip = UINT64_C(0x140f8f650), a = 0, b = 0, class_id = 0, mask = 0;
    unsigned pending = FLAGS_KEEP, width = 8, result;
#define FINISH(value) do { result = (value); goto publish; } while (0)
#define ACCESS(address, bytes, checkpoint) do { if (cpu_alias((address), (bytes), c)) FINISH(checkpoint); } while (0)
    ACCESS(list, 8, 2);
    rdi = rd64(list);
    ACCESS(stack + 0x28, 8, 3);
    wr64(stack + 0x28, rdi);
    ACCESS(rdi + 0x248, 1, 4);
    class_id = rcx = rd8(rdi + 0x248);
    rdx = class_id & 31;
    rax = class_id >> 5;
    pending = FLAGS_SHIFT;
    ACCESS(frame + 0x67, 8, 5);
    rcx = rd64(frame + 0x67);
    ACCESS(rcx + rax * 4, 4, 6);
    mask = rax = rd32(rcx + rax * 4);
    pending = FLAGS_BIT;
    if (!((mask >> (class_id & 31)) & 1)) FINISH(0);
    ACCESS(rdi + 0x58, 8, 7);
    rcx = rax = rd64(rdi + 0x58);
    ACCESS(frame + 0x7f, 8, 8);
    rdx = rd64(frame + 0x7f);
    ACCESS(rdx, 8, 9);
    rax &= rd64(rdx);
    a = rax; width = 8; pending = FLAGS_LOGIC;
    if (rax) FINISH(0);
    ACCESS(rdi + 0x40, 8, 10);
    rdx = rd64(rdi + 0x40);
    a = rdx;
    while (rdx) {
        rip = UINT64_C(0x140f8f6a0);
        ACCESS(rdx + 0x58, 8, 11);
        rax = rd64(rdx + 0x58);
        rax = (uint32_t)rax & 0x10u;
        a = rax; width = 4;
        if (rax) FINISH(0);
        ACCESS(rdx + 0x40, 8, 12);
        rdx = rd64(rdx + 0x40);
        a = rdx; width = 8;
    }
    rip = UINT64_C(0x140f8f6b6);
    ACCESS(frame + 0x6f, 4, 13);
    rax = rd32(frame + 0x6f);
    ACCESS(rdi + 0x170, 4, 14);
    a = rd32(rdi + 0x170) & (uint32_t)rax;
    width = 4;
    if (a) FINISH(0);
    ACCESS(frame + 0x77, 4, 15);
    rax = rd32(frame + 0x77);
    ACCESS(rdi + 0x174, 4, 16);
    a = rd32(rdi + 0x174); b = (uint32_t)rax; pending = FLAGS_COMPARE;
    FINISH((int32_t)(uint32_t)a > (int32_t)(uint32_t)b ? 0 : 1);
publish:
    c->rax = rax; c->rcx = rcx; c->rdx = rdx; c->rdi = rdi; c->rip = rip;
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
