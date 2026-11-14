#include "cpu64.h"
#include "bswap_cases.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>

int64_t g_image_delta;
static uint64_t expected_fallthrough;
static unsigned fallthrough_count;
void frostpunk_unsupported(uint64_t va, const char *text) {
    fprintf(stderr, "Unsupported 0x%" PRIx64 ": %s\n", va, text); abort();
}
void frostpunk_cpuid(CPU *c) { (void)c; abort(); }
uint64_t frostpunk_teb_read(uint64_t off, unsigned bytes) { (void)off; (void)bytes; abort(); }
void frostpunk_teb_write(uint64_t off, uint64_t value, unsigned bytes) {
    (void)off; (void)value; (void)bytes; abort();
}
void dispatch(CPU *c, uint64_t target) {
    (void)c;
    if (target != expected_fallthrough || fallthrough_count++) abort();
}
void dispatch_jmp(CPU *c, uint64_t target) { (void)c; (void)target; abort(); }
extern void (*const bswap_functions[])(CPU *c);

int main(void) {
    _Static_assert(offsetof(CPU, rip) == 16 * 8, "GPR layout changed");
    uint64_t checksum = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof bswap_cases / sizeof bswap_cases[0]; ++i) {
        const struct bswap_case *test = &bswap_cases[i];
        const struct bswap_variant *variant = &bswap_variants[test->variant];
        uint64_t gpr[16], actual[16], xmm[32];
        for (unsigned r = 0; r < 16; ++r)
            gpr[r] = UINT64_C(0x9E3779B97F4A7C15) ^ ((uint64_t)r * UINT64_C(0x0102040810204081)) ^ ((uint64_t)i << 32);
        for (unsigned k = 0; k < 32; ++k)
            xmm[k] = UINT64_C(0xABCD12345678EEFF) ^ ((uint64_t)k * UINT64_C(0x1122448811224488));
        gpr[variant->reg] = test->value;
        CPU c = {0};
        memcpy(&c.rax, gpr, sizeof gpr);
        memcpy(c.xmm, xmm, sizeof xmm);
        eflags_unpack(&c, test->flags);
        expected_fallthrough = variant->fallthrough;
        fallthrough_count = 0;
        bswap_functions[test->variant](&c);
        memcpy(actual, &c.rax, sizeof actual);
        for (unsigned r = 0; r < 16; ++r) {
            uint64_t expected = r == variant->reg ? test->result : gpr[r];
            if (actual[r] != expected) {
                fprintf(stderr, "FAIL case %zu reg%u width%u: expected=%016" PRIx64 " actual=%016" PRIx64 "\n",
                        i, r, variant->bits, expected, actual[r]);
                return 1;
            }
        }
        if ((eflags_pack(&c) & 0x8D5u) != test->flags || memcmp(c.xmm, xmm, sizeof xmm) || fallthrough_count != 1)
            return 2;
        checksum = (checksum ^ test->result ^ test->flags) * UINT64_C(1099511628211);
    }
    printf("PASS BSWAP32/64 native oracle: %zu cases, %zu encodings, checksum=%016" PRIx64 "\n",
           sizeof bswap_cases / sizeof bswap_cases[0], sizeof bswap_variants / sizeof bswap_variants[0], checksum);
    return 0;
}
