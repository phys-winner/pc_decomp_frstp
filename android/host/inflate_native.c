#include "cpu64.h"
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#endif

/* zlib inflate_fast, guest L_000141C05470.
 * The load window spends about 17% of its samples in the recompiled copy of
 * this function. Callers reload the stream pointers and avail counts, and
 * hold/bits at state+0x40/+0x44. The mode and msg writes are what the outer
 * inflate loop branches on. This entry still restores the callee-saved
 * registers, the [rsp+8] home store, the ret slot, and the flags of the
 * epilogue's final ADD rsp, 0x38.
 */
void fp_original_L_000141C05470(CPU *c);

static int inflate_enabled(void) {
#ifdef __ANDROID__
    static _Atomic int enabled = -1;
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        int saved = errno;
        char setting[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.fp.native_inflate", setting);
        errno = saved;
        value = setting[0] != '0';
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
#else
    return 1;
#endif
}

static uint32_t mem32(uint64_t address) { return rd32(address); }
static uint64_t mem64(uint64_t address) { return rd64(address); }
static void put32(uint64_t address, uint32_t value) { wr32(address, value); }
static void put64(uint64_t address, uint64_t value) { wr64(address, value); }

static uint8_t ld8(const uint8_t *p) {
    return *(volatile const uint8_t *)p;
}

static void st8(uint8_t *p, uint8_t value) {
    *(volatile uint8_t *)p = value;
}

static uint32_t mask_bits(uint32_t count) {
    count &= 31u;
    return count ? (1u << count) - 1u : 0u;
}

static void pull_pair(uint8_t **in, uint32_t *hold, uint32_t *bits) {
    uint32_t have = *bits;
    *hold += (uint32_t)ld8(*in + 1) << (have & 31u);
    *hold += (uint32_t)ld8(*in + 2) << ((have + 8u) & 31u);
    *in += 2;
    *bits = have + 16u;
}

static void pull_byte(uint8_t **in, uint32_t *hold, uint32_t *bits) {
    *hold += (uint32_t)ld8(*in + 1) << (*bits & 31u);
    *in += 1;
    *bits += 8u;
}

static void drop_bits(uint32_t *hold, uint32_t *bits, uint32_t count) {
    *hold >>= count & 31u;
    *bits -= count;
}

/* `from` is one byte behind the first source byte, matching the guest copy.
 * A non-overlapping run is a memcpy. An overlapping match (distance 1 or 2)
 * stays a byte loop so each stored byte is visible to the next read, which is
 * what the guest's 3-byte unroll does.
 */
static uint8_t *copy_out(uint8_t *out, uint8_t *from, uint32_t count) {
    uint8_t *src = from + 1;
    uint8_t *dst = out + 1;
    uintptr_t s = (uintptr_t)src;
    uintptr_t d = (uintptr_t)dst;
    if (count == 0)
        return out;
    if (s + count <= d || s >= d + count) {
        memcpy(dst, src, count);
        return out + count;
    }
    while (count--) {
        uint8_t byte = ld8(from + 1);
        from++;
        out++;
        st8(out, byte);
    }
    return out;
}

static void inflate_fast(uint8_t *strm, uint32_t start) {
    uint8_t *state = (uint8_t *)(uintptr_t)mem64((uint64_t)(strm + 0x28));
    uint8_t *in = (uint8_t *)(uintptr_t)mem64((uint64_t)strm) - 1;
    uint32_t avail_in = mem32((uint64_t)(strm + 8));
    uint8_t *last = in + (avail_in - 5u);
    uint8_t *out = (uint8_t *)(uintptr_t)mem64((uint64_t)(strm + 0x10)) - 1;
    uint32_t avail_out = mem32((uint64_t)(strm + 0x18));
    uint8_t *end = out + (uint32_t)(avail_out - 257u);
    uint8_t *beg = out - (uint32_t)(start - avail_out);
    uint32_t wsize = mem32((uint64_t)(state + 0x2c));
    uint32_t whave = mem32((uint64_t)(state + 0x30));
    uint32_t wnext = mem32((uint64_t)(state + 0x34));
    uint8_t *window = (uint8_t *)(uintptr_t)mem64((uint64_t)(state + 0x38));
    uint32_t hold = mem32((uint64_t)(state + 0x40));
    uint32_t bits = mem32((uint64_t)(state + 0x44));
    uint32_t *lcode = (uint32_t *)(uintptr_t)mem64((uint64_t)(state + 0x58));
    uint32_t *dcode = (uint32_t *)(uintptr_t)mem64((uint64_t)(state + 0x60));
    uint32_t lmask = mask_bits(mem32((uint64_t)(state + 0x68)));
    uint32_t dmask = mask_bits(mem32((uint64_t)(state + 0x6c)));
    int bad = 0;

    for (;;) {
        uint32_t here, op, len;
        if (bits < 15u)
            pull_pair(&in, &hold, &bits);
        here = lcode[hold & lmask];
        drop_bits(&hold, &bits, (here >> 8) & 0xffu);
        op = here & 0xffu;
        len = here >> 16;
        if (op == 0) {
            out++;
            st8(out, (uint8_t)len);
        } else {
            while ((op & 0x10u) == 0) {
                if (op & 0x40u) {
                    if (op & 0x20u)
                        put32((uint64_t)state, 0xbu);
                    else {
                        put64((uint64_t)(strm + 0x20), GVA(0x1428A3270));
                        put32((uint64_t)state, 0x1bu);
                    }
                    bad = 1;
                    break;
                }
                here = lcode[(here >> 16) + (hold & mask_bits(op))];
                drop_bits(&hold, &bits, (here >> 8) & 0xffu);
                op = here & 0xffu;
                len = here >> 16;
                if (op == 0) {
                    out++;
                    st8(out, (uint8_t)len);
                    break;
                }
            }
            if (bad || op == 0)
                goto symbol_done;
            op &= 0xfu;
            if (op) {
                if (bits < op)
                    pull_byte(&in, &hold, &bits);
                len += hold & mask_bits(op);
                drop_bits(&hold, &bits, op);
            }
            if (bits < 15u)
                pull_pair(&in, &hold, &bits);
            here = dcode[hold & dmask];
            drop_bits(&hold, &bits, (here >> 8) & 0xffu);
            op = here & 0xffu;
            while ((op & 0x10u) == 0) {
                if (op & 0x40u) {
                    put64((uint64_t)(strm + 0x20), GVA(0x1428A3258));
                    put32((uint64_t)state, 0x1bu);
                    bad = 1;
                    break;
                }
                here = dcode[(here >> 16) + (hold & mask_bits(op))];
                drop_bits(&hold, &bits, (here >> 8) & 0xffu);
                op = here & 0xffu;
            }
            if (bad)
                goto symbol_done;
            {
                uint32_t dist = here >> 16;
                uint32_t extra = op & 0xfu;
                while (bits < extra)
                    pull_byte(&in, &hold, &bits);
                dist += hold & mask_bits(extra);
                drop_bits(&hold, &bits, extra);
                {
                    uint32_t have_out = (uint32_t)(uintptr_t)out - (uint32_t)(uintptr_t)beg;
                    uint8_t *from;
                    uint32_t left = len;
                    if (dist <= have_out) {
                        from = out - dist;
                    } else {
                        uint32_t back = dist - have_out;
                        if (back > whave) {
                            put64((uint64_t)(strm + 0x20), GVA(0x1428A3238));
                            put32((uint64_t)state, 0x1bu);
                            bad = 1;
                            goto symbol_done;
                        }
                        if (wnext == 0) {
                            from = window - 1 + (wsize - back);
                            if (back < left) {
                                out = copy_out(out, from, back);
                                left -= back;
                                from = out - dist;
                            }
                        } else if (wnext < back) {
                            uint32_t head = back - wnext;
                            from = window - 1 + (wsize - head);
                            if (head < left) {
                                out = copy_out(out, from, head);
                                left -= head;
                                from = window - 1;
                                if (wnext < left) {
                                    out = copy_out(out, from, wnext);
                                    left -= wnext;
                                    from = out - dist;
                                }
                            }
                        } else {
                            from = window - 1 + (wnext - back);
                            if (back < left) {
                                out = copy_out(out, from, back);
                                left -= back;
                                from = out - dist;
                            }
                        }
                    }
                    out = copy_out(out, from, left);
                }
            }
        }
    symbol_done:
        if (bad)
            break;
        if ((uintptr_t)in >= (uintptr_t)last || (uintptr_t)out >= (uintptr_t)end)
            break;
    }

    {
        uint32_t drop = bits >> 3;
        in -= drop;
        bits -= drop * 8u;
        hold &= mask_bits(bits);
        put64((uint64_t)strm, (uint64_t)(uintptr_t)(in + 1));
        put64((uint64_t)(strm + 0x10), (uint64_t)(uintptr_t)(out + 1));
        put32((uint64_t)(strm + 8), (uint32_t)((uint32_t)(uintptr_t)last - (uint32_t)(uintptr_t)in) + 5u);
        put32((uint64_t)(strm + 0x18), (uint32_t)((uint32_t)(uintptr_t)end - (uint32_t)(uintptr_t)out) + 0x101u);
        put32((uint64_t)(state + 0x40), hold);
        put32((uint64_t)(state + 0x44), bits);
    }
}

void L_000141C05470(CPU *c) {
    uint64_t rdi = c->rdi, rbx = c->rbx, rbp = c->rbp, rsi = c->rsi;
    uint64_t r12 = c->r12, r13 = c->r13, r14 = c->r14, r15 = c->r15;
    if (!inflate_enabled()) {
        fp_original_L_000141C05470(c);
        return;
    }
    wr64(c->rsp + 8, c->rcx);
    inflate_fast((uint8_t *)(uintptr_t)c->rcx, (uint32_t)c->rdx);
    c->rdi = rdi;
    c->rbx = rbx;
    c->rbp = rbp;
    c->rsi = rsi;
    c->r12 = r12;
    c->r13 = r13;
    c->r14 = r14;
    c->r15 = r15;
    flags_add(c, c->rsp - 0x78u, 0x38u, 8);
    /* The lifted body sets rip at the epilogue label and the ret does not. */
    c->rip = UINT64_C(0x141C058FA);
    c->rsp += 8;
}
