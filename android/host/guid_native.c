#include "cpu64.h"
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#endif

void fp_original_L_000140E1E6C0(CPU *);

static int guid_enabled(void) {
#ifdef __ANDROID__
    static _Atomic int enabled = -1;
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        int saved = errno; char setting[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.fp.native_guid", setting); errno = saved;
        value = setting[0] != '0';
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
#else
    return 1;
#endif
}

static int overlaps_cpu(uint64_t address, const CPU *c) {
    uint64_t cpu = (uintptr_t)c;
    return address < cpu + sizeof(*c) && cpu < address + 16;
}

void L_000140E1E6C0(CPU *c) {
    uint64_t left = c->rcx, right = c->rdx;
    /* shortcut: preserve bytewise accesses at page edges and aliases with CPU state. */
    if (!guid_enabled() || !left || !right || left > UINT64_MAX - 16 || right > UINT64_MAX - 16 ||
        (left & 4095) > 4080 || (right & 4095) > 4080 || overlaps_cpu(left, c) || overlaps_cpu(right, c)) {
        fp_original_L_000140E1E6C0(c); return;
    }
    uint64_t a, b;
    memcpy(&a, (const void *)(uintptr_t)(left + 8), 8);
    memcpy(&b, (const void *)(uintptr_t)(right + 8), 8);
    uint64_t difference = a ^ b;
    if (!difference) {
        memcpy(&a, (const void *)(uintptr_t)left, 8);
        memcpy(&b, (const void *)(uintptr_t)right, 8);
        difference = a ^ b;
    }
    unsigned shift = difference ? (7u - (unsigned)__builtin_clzll(difference) / 8u) * 8u : 0;
    uint32_t byte_a = (uint8_t)(a >> shift), byte_b = (uint8_t)(b >> shift);
    SET32(c->r8, byte_b);
    SET32(c->r9, flags_sub(c, byte_a, byte_b, 4));
    SET32(c->rax, R32(c->r9));
    if (!difference) {
        flags_logicz(c, 0, 4);
        c->rip = UINT64_C(0x140e1e6d0);
    } else {
        c->rip = UINT64_C(0x140e1e6e6);
    }
    c->rsp += 8;
}
