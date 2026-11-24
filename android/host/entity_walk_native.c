/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native form of the entity walk 140EB45F0 of the game thread (2026-11-22, CPU task on Adreno 650 in
 * native Vulkan).  The walk calls virtual method +0xF8 of every entity of the global list
 * [[142A88998]+0x118] (count [+0x120]) whose byte +0x58 lacks bit 4, then 140F85900 (a thunk to the
 * entity's virtual method +0x1D8 with [entity+0x188] | edx) for every element of the walker's own list
 * [rcx+0x2008] (count [rcx+0x2010]).  With the methods it calls it was about 9 % of the game thread in
 * a developed city, nearly all of it first touches of entity cache lines.
 *
 * In a developed city (12 600 entities per frame, a third of them flagged) the method +0xF8 is
 * 140F87380 for about 6 000 entities and 140FBAAD0 for about 2 400.  140F87380 does nothing unless byte
 * +0xAF0 is set; 140FBAAD0 calls 140F87380 and then does nothing more unless byte +0xC09 is set.  For
 * such an entity with those bytes clear the call has no effect besides its stack writes below the stack
 * pointer (dead), so it is skipped; any other entity, or a set byte, gets the original call with the
 * same registers and return address (the lifted method runs as before).  The look-ahead prefetches the
 * lines the loop reads: +0x58, the object's first line (its vtable pointer), +0xAF0 and +0xC09.
 * The list and its count are read once before the loop, as the original does, so a callee that changes
 * the list sees the same iteration as before.
 *
 * debug.fp.native_walk: 0 = the lifted body, 1 = native (default), 2 = native with counters
 * (ENTITY_WALK_STATS lines).  debug.fp.walk_prefetch = "ahead,mask" (mask: 1 = +0x58, 2 = +0,
 * 4 = +0xAF0, 8 = +0xC09, 16 = +0x180); read once. */
#include "cpu64.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <sys/system_properties.h>
#endif

void fp_cpu_original_L_000140EB45F0(CPU *);
void L_000140FBAAD0(CPU *);
void L_000140F87380(CPU *);
extern __attribute__((visibility("hidden"))) _Atomic uint64_t fp_lookup_cache[32768];
__attribute__((visibility("hidden"))) void fp_lookup_anchor(void);

/* fl_icall of tools/fastlift.py: the published translation of a guest target, else the dispatcher. */
static inline void icall(CPU *c, uint64_t target) {
    uint64_t tag = target - UINT64_C(0x140000000) + 1;
    uint64_t entry = __atomic_load_n((uint64_t *)&fp_lookup_cache[((target >> 2) ^ (target >> 14)) & 32767u], __ATOMIC_RELAXED);
    if (__builtin_expect((entry >> 32) == tag && tag != 0, 1)) {
        ((void (*)(CPU *))((intptr_t)(uintptr_t)&fp_lookup_anchor + (intptr_t)(int32_t)(uint32_t)entry))(c);
        return;
    }
    dispatch(c, target);
}

static int setting(const char *name, int fallback, char *text, size_t size) {
#ifdef __ANDROID__
    int saved = errno; char value[PROP_VALUE_MAX] = {0};
    int length = __system_property_get(name, value); errno = saved;
    if (text && size) { strncpy(text, value, size - 1); text[size - 1] = 0; }
    return length > 0 ? atoi(value) : fallback;
#else
    (void)name; if (text && size) text[0] = 0; return fallback;
#endif
}
static _Atomic int walk_ready;
static int walk_mode = 1;
static unsigned walk_ahead = 12, walk_mask = 1 | 2 | 4 | 8;
static void walk_settings(void) {
    if (atomic_load_explicit(&walk_ready, memory_order_acquire)) return;
    int mode = setting("debug.fp.native_walk", 1, NULL, 0);
    walk_mode = mode < 0 || mode > 2 ? 1 : mode;
    char text[96]; unsigned ahead, mask;
    setting("debug.fp.walk_prefetch", 0, text, sizeof(text));
    if (text[0] && sscanf(text, "%u,%u", &ahead, &mask) == 2 && ahead < 64) { walk_ahead = ahead; walk_mask = mask; }
    fprintf(stderr, "ENTITY_WALK_NATIVE mode=%d prefetch ahead=%u mask=%u\n", walk_mode, walk_ahead, walk_mask);
    atomic_store_explicit(&walk_ready, 1, memory_order_release);
}

static inline void prefetch(uint64_t address) { __builtin_prefetch((const void *)(uintptr_t)address); }

/* Counters of mode 2: per call totals and the methods called, by target. */
typedef struct { uint64_t calls, entities, flagged, clean, dirty, other, second, simple_clean, simple_dirty; } WalkStats;
static _Atomic uint64_t walk_totals[sizeof(WalkStats) / sizeof(uint64_t)];
static _Atomic uint64_t target_va[64], target_count[64];
static void count_target(uint64_t va) {
    unsigned slot = (unsigned)((va >> 4) * 0x9E3779B1u) & 63u;
    for (unsigned i = 0; i < 64; ++i, slot = (slot + 1) & 63u) {
        uint64_t seen = atomic_load_explicit(&target_va[slot], memory_order_relaxed);
        if (seen == va || (!seen && atomic_compare_exchange_strong(&target_va[slot], &seen, va)) || seen == va) {
            atomic_fetch_add_explicit(&target_count[slot], 1, memory_order_relaxed);
            return;
        }
    }
}
static void report(const WalkStats *s) {
    const uint64_t *v = (const uint64_t *)s;
    for (unsigned i = 0; i < sizeof(WalkStats) / sizeof(uint64_t); ++i) atomic_fetch_add_explicit(&walk_totals[i], v[i], memory_order_relaxed);
    uint64_t calls = atomic_load_explicit(&walk_totals[0], memory_order_relaxed);
    if (calls % 600) return;
    fprintf(stderr, "ENTITY_WALK_STATS calls=%llu entities=%llu flagged=%llu clean=%llu dirty=%llu other=%llu second=%llu\n",
            (unsigned long long)calls, (unsigned long long)walk_totals[1], (unsigned long long)walk_totals[2],
            (unsigned long long)walk_totals[3], (unsigned long long)walk_totals[4], (unsigned long long)walk_totals[5],
            (unsigned long long)walk_totals[6]);
    fprintf(stderr, "ENTITY_WALK_SIMPLE clean=%llu dirty=%llu\n", (unsigned long long)walk_totals[7], (unsigned long long)walk_totals[8]);
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t va = atomic_load_explicit(&target_va[i], memory_order_relaxed), n = atomic_load_explicit(&target_count[i], memory_order_relaxed);
        if (va && n * 50 >= walk_totals[5]) fprintf(stderr, "ENTITY_WALK_TARGET %llx %llu\n", (unsigned long long)va, (unsigned long long)n);
    }
}

static void walk(CPU *c, WalkStats *stats) {
    uint64_t rsp0 = c->rsp, self = c->rcx;
    uint32_t phase = (uint32_t)c->rdx;
    /* mov [rsp+8], rbx; mov [rsp+0x10], rbp; mov [rsp+0x18], rsi; push rdi; push r14; push r15; sub rsp, 0x20 */
    wr64(rsp0 + 0x8, c->rbx); wr64(rsp0 + 0x10, c->rbp); wr64(rsp0 + 0x18, c->rsi);
    wr64(rsp0 - 0x8, c->rdi); wr64(rsp0 - 0x10, c->r14); wr64(rsp0 - 0x18, c->r15);
    uint64_t frame = rsp0 - 0x38;
    uint64_t world = rd64(GVA(0x142A88998));
    uint64_t list = rd64(world + 0x118);
    uint64_t bytes = (uint64_t)(int64_t)(int32_t)rd32(world + 0x120) * 8;
    uint64_t n = bytes >> 3;
    if (list > bytes + list) n = 0;
    uint64_t mesh = GVA(0x140FBAAD0), simple = GVA(0x140F87380);
    unsigned ahead = walk_ahead, mask = walk_mask;
    for (uint64_t i = 0; i != n; ++i) {
        if (i + ahead < n) {
            uint64_t f = rd64(list + 8 * (i + ahead));
            if (mask & 1) prefetch(f + 0x58);
            if (mask & 2) prefetch(f);
            if (mask & 4) prefetch(f + 0xAF0);
            if (mask & 8) prefetch(f + 0xC09);
            if (mask & 16) prefetch(f + 0x180);
        }
        uint64_t entity = rd64(list + 8 * i);
        if (stats) stats->entities++;
        if (rd8(entity + 0x58) & 0x10) { if (stats) stats->flagged++; continue; }
        uint64_t vtable = rd64(entity), method = rd64(vtable + 0xF8);
        if (method == simple) {
            if (!rd8(entity + 0xAF0)) { if (stats) stats->simple_clean++; continue; }
            if (stats) stats->simple_dirty++;
        } else if (method == mesh) {
            if (!rd8(entity + 0xAF0) && !rd8(entity + 0xC09)) { if (stats) stats->clean++; continue; }
            if (stats) stats->dirty++;
        } else if (stats) { stats->other++; count_target(method); }
        c->rsp = frame; c->rcx = entity; c->rax = vtable;
        push64(c, UINT64_C(0x140EB4654));
        if (method == mesh) L_000140FBAAD0(c); else if (method == simple) L_000140F87380(c); else icall(c, method);
    }
    int64_t count = (int32_t)rd32(self + 0x2010);
    uint64_t items = rd64(self + 0x2008);
    for (int64_t i = 0; i < count; ++i) {
        uint64_t item = rd64(items + 8ull * (uint64_t)i);
        if (stats) stats->second++;
        /* 140F85900: mov rax, [rcx]; mov edx, [rcx+0x188]; or edx, r?d; jmp [rax+0x1D8] */
        c->rsp = frame; c->rcx = item; c->rdx = phase;
        push64(c, UINT64_C(0x140EB467E));
        c->rax = rd64(item);
        c->rdx = (uint32_t)(rd32(item + 0x188) | phase);
        icall(c, rd64(c->rax + 0x1D8));
    }
    c->rsp = rsp0 + 8;
    if (stats) stats->calls++;
}

void L_000140EB45F0(CPU *c) {
    walk_settings();
    if (!walk_mode) { fp_cpu_original_L_000140EB45F0(c); return; }
    if (walk_mode == 2) { WalkStats s; memset(&s, 0, sizeof(s)); walk(c, &s); report(&s); return; }
    walk(c, NULL);
}
