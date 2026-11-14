/* Differential check of the fastlift flag helpers against the canonical cpu64.h ones.
 * Built for Android AArch64 and run on a device by tools/test_fastlift_flags.py. */
#include "fastlift.h"
#include <stdio.h>
#include <stdlib.h>

void frostpunk_unsupported(uint64_t va, const char *text) { (void)va; (void)text; abort(); }

static uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t rnd(void) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}
static uint64_t operand(void) {
    static const uint64_t edge[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, UINT64_C(0x7fffffff),
        UINT64_C(0x80000000), UINT64_C(0xffffffff), UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
        UINT64_C(0xffffffffffffffff), UINT64_C(0x100000000), UINT64_C(0xfffffffe)};
    uint64_t r = rnd();
    switch (r & 7) {
    case 0: return edge[(r >> 3) % (sizeof edge / sizeof *edge)];
    case 1: return (r >> 8) & 0xff;
    case 2: return edge[(r >> 3) % (sizeof edge / sizeof *edge)] + ((r >> 20) & 3) - 1;
    default: return rnd();
    }
}

static unsigned long long failures, checks;
static void seed(CPU *c, FL *f) {
    uint64_t r = rnd();
    c->cf = f->cf = r & 1; c->zf = f->zf = (r >> 1) & 1; c->sf = f->sf = (r >> 2) & 1;
    c->of = f->of = (r >> 3) & 1; c->pf = f->pf = (r >> 4) & 1; c->af = f->af = (r >> 5) & 1;
    f->lt = (r >> 6) & 1;
}
static int same(const CPU *c, const FL *f) {
    return c->cf == f->cf && c->zf == f->zf && c->sf == f->sf && c->of == f->of && c->pf == f->pf && c->af == f->af;
}
static void expect(int ok, const char *what, uint64_t a, uint64_t b, uint64_t n, int sz) {
    ++checks;
    if (ok) return;
    if (++failures <= 20)
        fprintf(stderr, "MISMATCH %s a=%016llx b=%016llx n=%llu sz=%d\n", what, (unsigned long long)a,
                (unsigned long long)b, (unsigned long long)n, sz);
}

#define BINARY(name) do { CPU c; FL f; seed(&c, &f); \
    uint64_t r1 = name(&c, a, b, sz), r2 = name##_fl(&f, a, b, sz); \
    expect(r1 == r2 && same(&c, &f), #name, a, b, 0, sz); } while (0)
#define UNARY(name) do { CPU c; FL f; seed(&c, &f); \
    uint64_t r1 = name(&c, a, sz), r2 = name##_fl(&f, a, sz); \
    expect(r1 == r2 && same(&c, &f), #name, a, 0, 0, sz); } while (0)
#define SHIFT(name) do { CPU c; FL f; seed(&c, &f); \
    uint64_t r1 = name(&c, a, n, sz), r2 = name##_fl(&f, a, n, sz); \
    expect(r1 == r2 && same(&c, &f), #name, a, 0, n, sz); } while (0)
#define DOUBLE(name) do { CPU c; FL f; seed(&c, &f); \
    uint64_t r1 = name(&c, a, b, n, sz), r2 = name##_fl(&f, a, b, n, sz); \
    expect(r1 == r2 && same(&c, &f), #name, a, b, n, sz); } while (0)

static float bits_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static uint32_t float_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static uint32_t float_pattern(void) {
    static const uint32_t edge[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
        0xffc00000u, 0x7fa00000u, 0xffa00000u, 0x7f800001u, 0x00000001u, 0x807fffffu, 0x00800000u, 0x7f7fffffu, 0xff7fffffu};
    uint64_t r = rnd();
    if ((r & 3) == 0) return edge[(r >> 2) % (sizeof edge / sizeof *edge)];
    if ((r & 3) == 1) return (uint32_t)(r >> 8) | 0x7f800000u;      /* NaN and infinity payloads */
    return (uint32_t)(r >> 8);
}

int main(int argc, char **argv) {
    unsigned long long rounds = argc > 1 ? strtoull(argv[1], 0, 10) : 2000000ull;
    static const int sizes[] = {1, 2, 4, 8};
    for (unsigned long long i = 0; i < rounds; ++i) {
        int sz = sizes[rnd() & 3];
        uint64_t a = operand(), b = operand(), n = (rnd() & 1) ? (rnd() & 0xff) : (rnd() % 70);
        BINARY(flags_add); BINARY(flags_sub); BINARY(flags_adc); BINARY(flags_sbb);
        UNARY(flags_logicz); UNARY(flags_incs); UNARY(flags_decs);
        SHIFT(op_shl); SHIFT(op_shr); SHIFT(op_sar); SHIFT(op_rol); SHIFT(op_ror);
        SHIFT(op_bt); SHIFT(op_bts); SHIFT(op_btr); SHIFT(op_btc);
        DOUBLE(op_shld); DOUBLE(op_shrd);
        { CPU c; FL f; seed(&c, &f); uint64_t r1 = op_bsf(&c, b, a, sz), r2 = op_bsf_fl(&f, b, a, sz);
          expect(r1 == r2 && same(&c, &f), "op_bsf", a, b, 0, sz); }
        { CPU c; FL f; seed(&c, &f); uint64_t r1 = op_bsr(&c, b, a, sz), r2 = op_bsr_fl(&f, b, a, sz);
          expect(r1 == r2 && same(&c, &f), "op_bsr", a, b, 0, sz); }
        { CPU c; FL f; seed(&c, &f); bitscan_undef_flags(&c); bitscan_undef_flags_fl(&f);
          expect(same(&c, &f), "bitscan_undef_flags", 0, 0, 0, 0); }
        { CPU c; FL f; seed(&c, &f); uint64_t p1 = eflags_pack(&c), p2 = eflags_pack_fl(&f);
          eflags_unpack(&c, a); eflags_unpack_fl(&f, a);
          expect(p1 == p2 && same(&c, &f), "eflags", a, 0, 0, 0); }
        { CPU c; FL f; seed(&c, &f); double x = bits_float(float_pattern()), y = bits_float(float_pattern());
          if (rnd() & 1) { uint64_t u = rnd(), v = rnd(); memcpy(&x, &u, 8); memcpy(&y, &v, 8); }
          if (!(rnd() & 7)) y = x;
          sse_compare(&c, x, y); sse_compare_fl(&f, x, y);
          expect(same(&c, &f), "sse_compare", 0, 0, 0, 0); }
        /* The signed-compare shortcut: lt must equal (sf != of) and nothing else may change. */
        { CPU c; FL f; seed(&c, &f); uint64_t r1 = flags_sub(&c, a, b, sz), r2 = flags_sub_lt(&f, a, b, sz);
          expect(r1 == r2 && same(&c, &f) && f.lt == (uint64_t)(f.sf != f.of), "flags_sub_lt", a, b, 0, sz); }
        { CPU c; FL f; seed(&c, &f); uint64_t r1 = flags_logicz(&c, a, sz), r2 = flags_logicz_lt(&f, a, sz);
          expect(r1 == r2 && same(&c, &f) && f.lt == (uint64_t)(f.sf != f.of), "flags_logicz_lt", a, 0, 0, sz); }
        { CPU c; FL f; seed(&c, &f); uint64_t r1 = flags_decs(&c, a, sz), r2 = flags_decs_lt(&f, a, sz);
          expect(r1 == r2 && same(&c, &f) && f.lt == (uint64_t)(f.sf != f.of), "flags_decs_lt", a, 0, 0, sz); }
        /* Scalar SSE add/multiply: the fast form against the canonical one, bit for bit. */
        { float x = bits_float(float_pattern()), y = bits_float(float_pattern());
          expect(float_bits(cpu64_addss(x, y)) == float_bits(frostpunk_addss(x, y)), "addss", float_bits(x), float_bits(y), 0, 4);
          expect(float_bits(cpu64_mulss(x, y)) == float_bits(frostpunk_mulss(x, y)), "mulss", float_bits(x), float_bits(y), 0, 4); }
    }
    /* Guest memory accessors: unaligned round trips against the canonical ones. */
    static unsigned char memory[256];
    for (unsigned i = 0; i < 200000; ++i) {
        uint64_t address = (uint64_t)(uintptr_t)memory + 16 + (rnd() % 200), v = rnd();
        wr8(address, (uint8_t)v);   expect(cpu64_rd8(address) == (uint8_t)v && rd8(address) == (uint8_t)v, "rw8", v, 0, 0, 1);
        wr16(address, (uint16_t)v); expect(cpu64_rd16(address) == (uint16_t)v && rd16(address) == (uint16_t)v, "rw16", v, 0, 0, 2);
        wr32(address, (uint32_t)v); expect(cpu64_rd32(address) == (uint32_t)v && rd32(address) == (uint32_t)v, "rw32", v, 0, 0, 4);
        wr64(address, v);           expect(cpu64_rd64(address) == v && rd64(address) == v, "rw64", v, 0, 0, 8);
        XMM x, y, z; x.u64[0] = rnd(); x.u64[1] = rnd();
        wrxm(address, x); y = cpu64_rdxm(address); z = rdxm(address);
        expect(!memcmp(&x, &y, 16) && !memcmp(&x, &z, 16), "rwxm", v, 0, 0, 16);
        float fv = bits_float((uint32_t)v); wrss(address, fv);
        expect(float_bits(cpu64_rdss(address)) == (uint32_t)v && float_bits(rdss(address)) == (uint32_t)v, "rwss", v, 0, 0, 4);
        double dv; memcpy(&dv, &v, 8); wrsd(address, dv); double back = rdsd(address), canon = cpu64_rdsd(address);
        expect(!memcmp(&back, &v, 8) && !memcmp(&canon, &v, 8), "rwsd", v, 0, 0, 8);
    }
    printf("FASTLIFT_FLAGS checks=%llu failures=%llu %s\n", checks, failures, failures ? "FAILED" : "passed");
    return failures != 0;
}
