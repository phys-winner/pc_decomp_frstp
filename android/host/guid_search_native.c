/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native forms of the two GUID searches of the game thread (2026-11-22, CPU task on Adreno 650 in
 * native Vulkan): together with the GUID comparison they call per element (140E1E6C0, already
 * native in guid_native.c) they were about 5 % of the game thread in a developed city.
 *
 * 140E92AF0  (rcx = container, rdx = GUID) -> the first element of the pointer array
 *            [container+0x20] (count [container+0x28], int32) whose GUID at +8 equals the key, else 0.
 * 140E92B80  (rcx = registry,  rdx = GUID) -> the same over every container of the pointer array
 *            [registry+0x40] (count [registry+0x48]); a container's elements are [+0x18], count [+0x20].
 * Both return 0 at once for the null GUID at 14296FC18.
 *
 * The lifted bodies stay (fp_cpu_original_L_..., weakened by build/cpu-20261122/build.py); these
 * functions keep everything a caller can observe: the result in rax, the saved registers, the stack
 * pointer, the register saves in the caller's home area and the values the original pushes below the
 * stack pointer (last pushed return address included).  The element arrays and counts are read again
 * in every iteration, as the original does.  GUIDs compare equal exactly when all sixteen bytes are
 * equal, which is when the original comparison returns 0.  debug.fp.native_guid_search=0 runs the
 * lifted bodies. */
#include "cpu64.h"
#include <stdio.h>
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#endif

void fp_cpu_original_L_000140E92AF0(CPU *);
void fp_cpu_original_L_000140E92B80(CPU *);

/* 0 = lifted bodies, 1 = native, 2 = native checked against the lifted body on every call
 * (GUID_SEARCH_VERIFY_MISMATCH / GUID_SEARCH_VERIFY lines; diagnostics only). */
static int search_mode(void) {
#ifdef __ANDROID__
    static _Atomic int mode = -1;
    int value = atomic_load_explicit(&mode, memory_order_relaxed);
    if (value < 0) {
        int saved = errno; char setting[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.fp.native_guid_search", setting); errno = saved;
        value = setting[0] == '0' ? 0 : setting[0] == '2' ? 2 : 1;
        atomic_store_explicit(&mode, value, memory_order_relaxed);
    }
    return value;
#else
    return 1;
#endif
}
static int search_enabled(void) { return search_mode() != 0; }
static void native_92AF0(CPU *c);
static void native_92B80(CPU *c);
static void verify(CPU *c, void (*native)(CPU *), void (*original)(CPU *), const char *name) {
    static _Atomic unsigned long calls, mismatches;
    CPU copy = *c;
    original(&copy);
    native(c);
    unsigned long n = atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed) + 1;
    if (copy.rax != c->rax || copy.rsp != c->rsp || copy.rbx != c->rbx || copy.rbp != c->rbp || copy.rsi != c->rsi ||
        copy.rdi != c->rdi || copy.r14 != c->r14 || copy.r15 != c->r15) {
        unsigned long m = atomic_fetch_add_explicit(&mismatches, 1, memory_order_relaxed) + 1;
        if (m <= 20) fprintf(stderr, "GUID_SEARCH_VERIFY_MISMATCH %s rax=%llx/%llx\n", name,
                             (unsigned long long)copy.rax, (unsigned long long)c->rax);
    }
    if (!(n % 20000)) fprintf(stderr, "GUID_SEARCH_VERIFY calls=%lu mismatches=%lu\n", n, atomic_load(&mismatches));
}

static inline int guid_equal(uint64_t a, uint64_t b) {
    uint64_t a0, a1, b0, b1;
    memcpy(&a1, (const void *)(uintptr_t)(a + 8), 8); memcpy(&b1, (const void *)(uintptr_t)(b + 8), 8);
    if (a1 != b1) return 0;
    memcpy(&a0, (const void *)(uintptr_t)a, 8); memcpy(&b0, (const void *)(uintptr_t)b, 8);
    return a0 == b0;
}

void L_000140E92AF0(CPU *c) {
    int mode = search_mode();
    if (!mode) { fp_cpu_original_L_000140E92AF0(c); return; }
    if (mode == 2) { verify(c, native_92AF0, fp_cpu_original_L_000140E92AF0, "140E92AF0"); return; }
    native_92AF0(c);
}
static void native_92AF0(CPU *c) {
    uint64_t rsp = c->rsp, self = c->rcx, key = c->rdx, result = 0;
    wr64(rsp + 0x8, c->rbx); wr64(rsp + 0x10, c->rbp); wr64(rsp + 0x18, c->rsi);
    wr64(rsp - 0x8, c->rdi);                           /* push rdi; sub rsp, 0x20 */
    uint64_t returned = UINT64_C(0x140E92B19);
    if (!guid_equal(key, GVA(0x14296FC18))) {
        for (uint32_t i = 0; (int32_t)i < (int32_t)rd32(self + 0x28); ++i) {
            returned = UINT64_C(0x140E92B44);
            uint64_t element = rd64(rd64(self + 0x20) + 8ull * i);
            if (guid_equal(element + 8, key)) { result = rd64(rd64(self + 0x20) + 8ull * (uint64_t)(int64_t)(int32_t)i); break; }
        }
    }
    wr64(rsp - 0x30, returned);                        /* the last call's return address */
    c->rax = result;
    c->rsp = rsp + 8;
}

void L_000140E92B80(CPU *c) {
    int mode = search_mode();
    if (!mode) { fp_cpu_original_L_000140E92B80(c); return; }
    if (mode == 2) { verify(c, native_92B80, fp_cpu_original_L_000140E92B80, "140E92B80"); return; }
    native_92B80(c);
}
static void native_92B80(CPU *c) {
    uint64_t rsp = c->rsp, self = c->rcx, key = c->rdx, result = 0;
    wr64(rsp + 0x8, c->rbx); wr64(rsp + 0x10, c->rbp); wr64(rsp + 0x18, c->rsi);
    wr64(rsp - 0x8, c->rdi); wr64(rsp - 0x10, c->r14); wr64(rsp - 0x18, c->r15);   /* push rdi, r14, r15 */
    uint64_t returned = UINT64_C(0x140E92BAD);
    if (!guid_equal(key, GVA(0x14296FC18))) {
        uint64_t at = rd64(self + 0x40), end = at + 8ull * (uint64_t)(int64_t)(int32_t)rd32(self + 0x48);
        for (; at != end && !result; at += 8) {
            uint64_t container = rd64(at);
            for (uint32_t i = 0; (int32_t)i < (int32_t)rd32(container + 0x20); ++i) {
                returned = UINT64_C(0x140E92BE4);
                uint64_t element = rd64(rd64(container + 0x18) + 8ull * i);
                if (guid_equal(element + 8, key)) {
                    result = rd64(rd64(container + 0x18) + 8ull * (uint64_t)(int64_t)(int32_t)i);
                    break;
                }
            }
        }
    }
    wr64(rsp - 0x40, returned);                        /* the last call's return address */
    c->rax = result;
    c->rsp = rsp + 8;
}
