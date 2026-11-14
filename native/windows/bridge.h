#pragma once
#include "cpu64.h"

#define FP_NATIVE_MAX_STACK_BYTES 256u

#ifdef __cplusplus
extern "C" {
#endif
/* Caller validates target and [c->rsp, c->rsp + 40 + stack_bytes). Stack bytes
 * must be a multiple of eight, at most 32 arguments. Only RAX/RDX/XMM0/XMM1,
 * RIP and the single eight-byte return-address pop are reflected in CPU. */
void fp_native_call(CPU *c, uint64_t target, unsigned stack_bytes);

/* Caller must verify CPUID.XSAVE/OSXSAVE and a supported XCR selector first. */
uint64_t fp_xgetbv(uint32_t selector);

/* Both bounds must describe committed writable memory owned by this thread.
 * fn must return normally: native exception unwinding across this switch is
 * unsupported. The Windows NT_TIB stack bounds are restored on normal return. */
void fp_on_alt_stack(void *stack_top, void *stack_limit,
    void (*fn)(void *), void *context);
#ifdef __cplusplus
}
#endif
