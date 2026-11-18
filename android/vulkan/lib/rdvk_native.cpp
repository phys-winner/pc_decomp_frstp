/* Calls from the native device into translated engine functions. */
#include "rdvk.h"

namespace rdvk {
uint64_t guest_call(CPU* c, uint64_t va, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    rdvk_fn fn = host->static_target(va);
    if (!fn) stop("guest_call: no lifted function at 0x%llx", (unsigned long long)va);
    CPU saved = *c;
    /* Below the caller's frame: 16-byte aligned call site, 32 bytes of shadow space, return slot. */
    c->rsp = ((saved.rsp - 256) & ~uint64_t(15)) - 8;
    wr64(c->rsp, UINT64_C(0xE6100FF8));  /* never used: lifted functions return to the host */
    c->rcx = a0; c->rdx = a1; c->r8 = a2; c->r9 = a3;
    fn(c);
    uint64_t result = c->rax;
    uint64_t rbx = c->rbx, rbp = c->rbp, rsi = c->rsi, rdi = c->rdi, r12 = c->r12, r13 = c->r13, r14 = c->r14, r15 = c->r15;
    if (rbx != saved.rbx || rbp != saved.rbp || rsi != saved.rsi || rdi != saved.rdi || r12 != saved.r12 || r13 != saved.r13 ||
        r14 != saved.r14 || r15 != saved.r15)
        stop("guest_call 0x%llx changed a non-volatile register", (unsigned long long)va);
    *c = saved;
    return result;
}

}  // namespace rdvk
