#include "cpu64.h"
#include "leaf_cases.h"
#include <inttypes.h>
#include <stdio.h>

int64_t g_image_delta;
void frostpunk_unsupported(uint64_t va, const char *text) {
    fprintf(stderr, "Unsupported guest instruction 0x%" PRIx64 ": %s\n", va, text); abort();
}
void frostpunk_cpuid(CPU *c) { (void)c; fputs("Unexpected CPUID in leaf probe\n", stderr); abort(); }
uint64_t frostpunk_teb_read(uint64_t off, unsigned bytes) {
    (void)off; (void)bytes; fputs("Unexpected TEB read in leaf probe\n", stderr); abort();
}
void frostpunk_teb_write(uint64_t off, uint64_t value, unsigned bytes) {
    (void)off; (void)value; (void)bytes; fputs("Unexpected TEB write in leaf probe\n", stderr); abort();
}
void dispatch(CPU *c, uint64_t target) {
    (void)c; fprintf(stderr, "Unexpected dispatch to 0x%" PRIx64 "\n", target); abort();
}
void dispatch_jmp(CPU *c, uint64_t target) { dispatch(c, target); }
void frostpunk_leaf(CPU *c);

int main(void) {
    _Static_assert(sizeof(float) == 4, "The guest requires binary32 floats");
    uint64_t checksum = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof leaf_cases / sizeof leaf_cases[0]; ++i) {
        const struct leaf_case *test = &leaf_cases[i];
        uint32_t params[2] = { test->a, test->b };
        uint64_t stack[2] = { UINT64_C(0x123456789ABCDEF0), 0 };
        CPU c = {0};
        c.rcx = (uintptr_t)params;
        c.rsp = (uintptr_t)stack;
        c.mxcsr = 0x1F80u;
        c.xmm[1].u32[0] = test->x;
        for (unsigned lane = 1; lane < 4; ++lane)
            c.xmm[1].u32[lane] = 0xABCD0000u + lane;
        c.cf = c.zf = c.pf = 1;
        frostpunk_leaf(&c);
        if (c.xmm[0].u32[0] != test->result || c.rsp != (uintptr_t)(stack + 1) ||
            c.rcx != (uintptr_t)params || params[0] != test->a || params[1] != test->b ||
            c.cf != 1 || c.zf != 1 || c.pf != 1 || c.of || c.af || c.sf) {
            fprintf(stderr, "FAIL case %zu: a=%08x b=%08x x=%08x expected=%08x actual=%08x\n",
                    i, test->a, test->b, test->x, test->result, c.xmm[0].u32[0]);
            return 1;
        }
        for (unsigned lane = 1; lane < 4; ++lane)
            if (c.xmm[0].u32[lane] != 0xABCD0000u + lane) return 2;
        checksum = (checksum ^ c.xmm[0].u32[0]) * UINT64_C(1099511628211);
    }
    printf("PASS real Frostpunk SSE leaf: %zu cases, checksum=%016" PRIx64 "\n",
           sizeof leaf_cases / sizeof leaf_cases[0], checksum);
    return 0;
}
