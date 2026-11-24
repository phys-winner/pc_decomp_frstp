/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native form of the scene visibility query 140F8F530 of the game thread (2026-11-22, CPU task on
 * Adreno 650 in native Vulkan).  With the functions it calls per object it was about 20 % of the game
 * thread in a developed city: the loop itself waited on the first touch of every object, and every
 * object that needed a box test went through eight lifted calls (140E2C6C0 -> 140E398D0, 140E2C050 x3
 * with a TLS read each, 140E39940; 140E2C890; 140E2C640) and the screen-size test through five more
 * (140E2C200, 140E0A510 -> 140E2C4B0, 140E2C4E0; 140E0A5C0).
 *
 * The query (vtable slot +0x40 of the scene's object-query class; rcx = query, rdx = output pointer
 * vector {data, int count, int capacity}, r8 = layer bit set, r9d = excluded mask, [rsp+0x28] = the
 * largest accepted priority, [rsp+0x30] = pointer to the excluded flag mask):
 *   cells = 140F8F450(query, query->root)       quadtree nodes that touch the frustum, {node, inside}
 *   tan_half = tanf(camera->fov * pi/360)       the import, through its thunk 141AE327C
 *   for every object of every cell, in order:
 *     layer bit [obj+0x248] set; ([obj+0x58] & *mask) == 0; no ancestor ([+0x40] chain) with bit 4 of
 *     [+0x58]; ([obj+0x170] & r9d) == 0; (int)[obj+0x174] <= priority;
 *     unless the cell is inside or bit 42 of [obj+0x58] is set: the world box of the local box
 *     [obj+0x150] under the matrix [obj+0x110] must pass the six frustum planes and be valid;
 *     if query[+0x44] (or the mobile detail filter of mobile_detail.c): the camera is inside the
 *     bounding sphere of the eight transformed corners, or its projected size passes the threshold;
 *     then the object is appended to the output vector (growth by 140D9FF80).
 *   the cell list is released (its allocator's vtable +8, or 140E2BA60).
 *
 * Every arithmetic step is the original's packed or scalar SSE operation in the same order with the
 * same operands (the lifted bodies are plain IEEE float operations with -ffp-contract=off; minps/maxps
 * are select-by-compare, mulss keeps the x86 NaN rule), so every comparison sees the same values.
 * NaN payloads cannot change a decision: each decision is a comparison.  The constants that the
 * game's static initializers fill (box seeds 14296F930, matrix row 1429703B0, the |row| multiplier
 * 144011890) are read from guest memory at run time.  Calls that have effects stay the game's own:
 * the cell gather, the tanf thunk, the vector growth and the release of the cell list, with the
 * original's guest frame layout (registers pushed, xmm6-8 saved, exception state) and return
 * addresses.  The lifted body stays as fp_cpu_original_L_000140F8F530 (weakened by
 * build/cpu-20261122/build.py) and runs when MXCSR or FPCR are not the defaults, when 140E2C050's
 * thread-safe static is not yet initialized for this thread (its first call takes the init path) or
 * when the game's box assertions are on (dword_14296FAE0).
 *
 * debug.fp.native_visibility: 0 = the lifted body, 1 = native (default), 2 = the lifted body, then a
 * native pass without effects on every call, comparing the appended objects (VISIBILITY_VERIFY*
 * lines), 3 = native with per-stage counters (VISIBILITY_STATS lines).  debug.fp.vis_prefetch =
 * "ahead,parent,mask" tunes the look-ahead (mask: 1 = +0x248, 2 = +0x40/+0x58, 4 = +0x170, 8 = the box
 * at +0x110/+0x150, 16 = the parent's +0x58 at the parent distance); read once. */
#include "cpu64.h"
#include "mobile_detail.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <sys/system_properties.h>
#endif

#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)

typedef float float4 __attribute__((vector_size(16)));
typedef int32_t int4 __attribute__((vector_size(16)));
typedef uint32_t uint4 __attribute__((vector_size(16)));

void fp_cpu_original_L_000140F8F530(CPU *);
void L_000140F8F450(CPU *);
void L_000141AE327C(CPU *);
void L_000140D9FF80(CPU *);
void L_000140E2BA60(CPU *);

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
static int visibility_mode(void) {
    static _Atomic int mode = -1;
    int value = atomic_load_explicit(&mode, memory_order_relaxed);
    if (value < 0) {
        value = setting("debug.fp.native_visibility", 1, NULL, 0);
        if (value < 0 || value > 3) value = 1;
        atomic_store_explicit(&mode, value, memory_order_relaxed);
    }
    return value;
}
/* Look-ahead in objects, parent look-ahead, field mask (see the header comment). */
static _Atomic int prefetch_ready;
static unsigned prefetch_ahead = 12, prefetch_parent = 4, prefetch_mask = 1 | 2 | 4 | 16;
static void prefetch_settings(void) {
    if (atomic_load_explicit(&prefetch_ready, memory_order_acquire)) return;
    char text[96];
    setting("debug.fp.vis_prefetch", 0, text, sizeof(text));
    unsigned ahead, parent, mask;
    if (text[0] && sscanf(text, "%u,%u,%u", &ahead, &parent, &mask) == 3 && ahead < 64 && parent < 64) {
        prefetch_ahead = ahead; prefetch_parent = parent; prefetch_mask = mask;
    }
    fprintf(stderr, "VISIBILITY_NATIVE mode=%d prefetch ahead=%u parent=%u mask=%u\n", visibility_mode(), prefetch_ahead,
            prefetch_parent, prefetch_mask);
    atomic_store_explicit(&prefetch_ready, 1, memory_order_release);
}

static inline float4 ld4(uint64_t address) { float4 v; memcpy(&v, (const void *)(uintptr_t)address, 16); return v; }
static inline float4 select4(int4 mask, float4 a, float4 b) {
    uint4 ua, ub, m = (uint4)mask; memcpy(&ua, &a, 16); memcpy(&ub, &b, 16);
    ua = (ua & m) | (ub & ~m); memcpy(&a, &ua, 16); return a;
}
/* minps d, s / maxps d, s: d < s ? d : s and d > s ? d : s per lane (sse_minf/sse_maxf). */
static inline float4 minps(float4 d, float4 s) { return select4(d < s, d, s); }
static inline float4 maxps(float4 d, float4 s) { return select4(d > s, d, s); }
#define SPLAT(v, i) __builtin_shufflevector((v), (v), (i), (i), (i), (i))
static inline void prefetch(uint64_t address) { __builtin_prefetch((const void *)(uintptr_t)address); }

static uint64_t fpcr(void) {
#if defined(__aarch64__)
    uint64_t value; __asm__ volatile("mrs %0, fpcr" : "=r"(value)); return value;
#else
    return 0;
#endif
}

/* Per-call constants of the object tests. */
typedef struct {
    float4 planes[6], half, mask, k, row3_zero, seed_min, seed_max, campos;
    float one, tan_half, threshold;
} Constants;

/* 140E2C6C0 (140E398D0, 140E2C050 x3, 140E39940), 140E2C890 and 140E2C640: 1 if the world box of the
 * object's local box passes the six planes and is valid. */
static inline int box_visible(const Constants *k, uint64_t obj) {
    float4 lb = ld4(obj + 0x150), ub = ld4(obj + 0x160);
    float4 m0 = ld4(obj + 0x110), m1 = ld4(obj + 0x120), m2 = ld4(obj + 0x130), m3 = ld4(obj + 0x140);
    float4 center = (lb + ub) * k->half;
    float4 ext = ub - center;
    float4 one = {k->one, k->one, k->one, k->one};
    float4 t3 = one * m3, t1 = SPLAT(center, 1) * m1, t2 = SPLAT(center, 2) * m2, t0 = SPLAT(center, 0) * m0;
    float4 world = (t3 + t2) + (t1 + t0);
    float4 a0 = maxps(m0, m0 * k->k), a1 = maxps(m1, m1 * k->k), a2 = maxps(m2, m2 * k->k);
    float4 e1 = SPLAT(ext, 1) * a1, e2 = SPLAT(ext, 2) * a2, e0 = SPLAT(ext, 0) * a0;
    float4 extent = (k->row3_zero + e2) + (e1 + e0);
    float4 bmin = world - extent, bmax = world + extent;
    const float4 zero = {0, 0, 0, 0};
    for (unsigned i = 0; i < 3; ++i) {
        float4 p0 = k->planes[2 * i], p1 = k->planes[2 * i + 1];
        float4 v5 = maxps(p0, zero) * bmax + minps(p0, zero) * bmin;
        float4 v6 = maxps(p1, zero) * bmax + minps(p1, zero) * bmin;
        float4 v7 = __builtin_shufflevector(v5, v6, 2, 6, 3, 7) + __builtin_shufflevector(v5, v6, 0, 4, 1, 5);
        int4 outside = (__builtin_shufflevector(v7, v7, 2, 3, 0, 1) + v7) < zero;
        if (outside[0] | outside[1] | outside[2] | outside[3]) return 0;
    }
    return !(bmin[0] > bmax[0]) && !(bmin[1] > bmax[1]) && !(bmin[2] > bmax[2]);
}

/* (q.w + q.y) + (q.z + q.x): movhlps, addps, shufps 0x11, addps. */
static inline float horizontal(float4 q) { return (q[3] + q[1]) + (q[2] + q[0]); }

/* 140E2C200 (the eight corners), 140E0A510 (140E2C4B0, 140E2C4E0), 140E0A5C0 and the projected size of
 * the query: 1 if the object stays. */
static inline int size_visible(const Constants *k, uint64_t obj) {
    float4 lb = ld4(obj + 0x150), ub = ld4(obj + 0x160);
    float4 m0 = ld4(obj + 0x110), m1 = ld4(obj + 0x120), m2 = ld4(obj + 0x130), m3 = ld4(obj + 0x140);
    float4 hx = SPLAT(ub, 0) * m0, hz = SPLAT(ub, 2) * m2, lx = SPLAT(lb, 0) * m0;
    float4 ly = SPLAT(lb, 1) * m1, lz = SPLAT(lb, 2) * m2, hy = SPLAT(ub, 1) * m1;
    float4 mn = k->seed_min, mx = k->seed_max, p;
    float4 a = lx + ly, b = hy + lx, c = hx + ly, d = hx + hy;
    p = (a + lz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (a + hz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (b + lz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (b + hz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (c + lz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (c + hz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (d + lz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    p = (d + hz) + m3; mn = minps(mn, p); mx = maxps(mx, p);
    float4 center = (mx + mn) * k->half;
    float4 h = (mx - mn) * k->half;
    float radius = sqrtf(horizontal((h * h) * k->mask));
    float4 d0 = center - k->campos;
    float distance2 = horizontal((d0 * d0) * k->mask);
    if (frostpunk_mulss(radius, radius) > distance2) return 1;
    float4 d1 = k->campos - center;
    float distance = sqrtf(horizontal((d1 * d1) * k->mask));
    float m = sse_maxf(distance, radius);
    float difference = frostpunk_mulss(m, m) - frostpunk_mulss(radius, radius);
    float side = sqrtf(sse_maxf(difference, k->one));
    float projected = (radius / k->tan_half) / side;
    return projected > k->threshold;
}

typedef struct {
    uint64_t calls, cells, objects, layer, flags, parent, masked, skipped_box, box, box_rejected, size, size_rejected,
        accepted, fallback;
} Stats;
static _Atomic uint64_t stats_total[sizeof(Stats) / sizeof(uint64_t)];

/* Output of a pass without effects (verify mode). */
typedef struct { uint64_t *items; uint32_t count, capacity, overflow; } Collected;

/* Appends like the original's inline push_back (140F8F80D..140F8F872); growth is the game's. */
static void append(CPU *c, uint64_t frame, uint64_t out, uint64_t obj) {
    int32_t count = (int32_t)rd32(out + 8);
    if (count == (int32_t)rd32(out + 0xC)) {
        uint64_t data = rd64(out), slot = frame + 0x28;
        wr64(slot, obj);
        c->rsp = frame;
        if (data <= slot && slot < data + 8ull * (uint64_t)(int64_t)count) {
            int32_t index = (int32_t)((int64_t)(slot - data) >> 3);
            c->rcx = out; c->rdx = (uint32_t)(count + 1); push64(c, UINT64_C(0x140F8F847)); L_000140D9FF80(c);
            uint64_t grown = rd64(out);
            wr64(grown + 8ull * (uint64_t)(int64_t)(int32_t)rd32(out + 8), rd64(grown + 8ull * (uint64_t)(int64_t)index));
            wr32(out + 8, rd32(out + 8) + 1);
            return;
        }
        c->rcx = out; c->rdx = (uint32_t)(count + 1); push64(c, UINT64_C(0x140F8F865)); L_000140D9FF80(c);
        count = (int32_t)rd32(out + 8);
    }
    wr64(rd64(out) + 8ull * (uint64_t)(int64_t)count, obj);
    wr32(out + 8, rd32(out + 8) + 1);
}

/* 0 = the original must run (nothing was changed yet), 1 = done.  With collected != NULL nothing is
 * appended to the output vector; the objects are recorded instead. */
static int run(CPU *c, Collected *collected, Stats *stats) {
    uint64_t rsp0 = c->rsp;
    if ((c->mxcsr & ~0x3fu) != 0x1f80u || fpcr() || rd32(GVA(0x14296FAE0))) return 0;
    /* 140E2C050's static: guard <= this thread's epoch, else its first call initializes. */
    uint64_t tls = rd_gs64(0x58);
    int32_t epoch = (int32_t)rd32(rd64(tls + 8ull * rd32(GVA(0x14400AA98))) + 0x1C);
    if ((int32_t)rd32(GVA(0x1440118A0)) > epoch) return 0;
    unsigned detail = fp_test_detail_mode(c);
    uint64_t self = c->rcx, out = c->rdx, layers = c->r8;
    uint32_t excluded = (uint32_t)c->r9;
    int32_t priority = (int32_t)rd32(rsp0 + 0x28);
    uint64_t flag_mask = rd64(rsp0 + 0x30);
    /* Prologue: home slots, pushes, frame, xmm6-8 saves, exception state (the guest frame of the
     * original, so that the calls below run with the same stack and an unwind would find it). */
    wr32(rsp0 + 0x20, excluded); wr64(rsp0 + 0x18, layers); wr64(rsp0 + 0x8, self);
    wr64(rsp0 - 0x08, c->rbp); wr64(rsp0 - 0x10, c->rsi); wr64(rsp0 - 0x18, c->rdi); wr64(rsp0 - 0x20, c->r12);
    wr64(rsp0 - 0x28, c->r13); wr64(rsp0 - 0x30, c->r14); wr64(rsp0 - 0x38, c->r15);
    uint64_t frame = rsp0 - 0x138;
    wr64(frame + 0x80, UINT64_C(0xFFFFFFFFFFFFFFFE));
    wr64(rsp0 + 0x10, c->rbx);
    wrxm(rsp0 - 0x48, c->xmm[6]); wrxm(rsp0 - 0x58, c->xmm[7]); wrxm(rsp0 - 0x68, c->xmm[8]);
    uint64_t planes = rd64(rd64(GVA(0x142B3B598)) + 0x20F8) + 0x2B0;
    wr64(frame + 0x58, planes);
    wr64(frame + 0x30, 0); wr64(frame + 0x38, 0); wr64(frame + 0x40, 0);
    c->rsp = frame; c->rcx = self; c->rdx = rd64(self + 0x10); c->r8 = frame + 0x30;
    push64(c, UINT64_C(0x140F8F5AC)); L_000140F8F450(c);
    uint64_t camera = rd64(rd64(GVA(0x142B3B598)) + 0x20F8);
    c->rsp = frame; c->rcx = camera;
    sse_load_ss(&c->xmm[0], frostpunk_mulss(rdss(camera + 0x20), rdss(GVA(0x141D5A300))));
    push64(c, UINT64_C(0x140F8F5CC)); L_000141AE327C(c);
    Constants k;
    k.tan_half = c->xmm[0].f32[0];
    k.one = rdss(GVA(0x1421F52B8));
    k.threshold = fp_test_detail_threshold(rdss(GVA(0x141D83E94)), detail);
    for (unsigned i = 0; i < 6; ++i) k.planes[i] = ld4(planes + 16ull * i);
    k.half = ld4(GVA(0x141D51140)); k.mask = ld4(GVA(0x142898370)); k.k = ld4(GVA(0x144011890));
    k.row3_zero = (float4){0, 0, 0, 0} * ld4(GVA(0x1429703B0));
    k.seed_min = ld4(GVA(0x14296F930)); k.seed_max = ld4(GVA(0x14296F940));
    k.campos = ld4(camera + 0x3E0);
    unsigned ahead = prefetch_ahead, parent_ahead = prefetch_parent, fields = prefetch_mask;
    int32_t cells = (int32_t)rd32(frame + 0x38);
    uint64_t block = rd64(frame + 0x30);
    for (int32_t i = 0; i < cells; ++i) {
        uint64_t entry = block + 16ull * (uint64_t)(int64_t)i;
        uint8_t inside = rd8(entry + 8);
        uint64_t node = rd64(entry);
        uint64_t list = rd64(node + 0x68);
        int64_t n = (int32_t)rd32(node + 0x70);
        uint64_t end = list + (uint64_t)n * 8;
        uint64_t m = ((uint64_t)n * 8 + 7) >> 3;
        if (list > end) m = 0;
        if (stats) stats->cells++;
        for (uint64_t j = 0; j != m; ++j) {
            if (j + ahead < m) {
                uint64_t f = rd64(list + 8 * (j + ahead));
                if (fields & 1) prefetch(f + 0x248);
                if (fields & 2) { prefetch(f + 0x40); prefetch(f + 0x58); }
                if (fields & 4) prefetch(f + 0x170);
                if (fields & 8) { prefetch(f + 0x110); prefetch(f + 0x150); }
            }
            if ((fields & 16) && j + parent_ahead < m) {
                uint64_t parent = rd64(rd64(list + 8 * (j + parent_ahead)) + 0x40);
                if (parent) prefetch(parent + 0x58);
            }
            uint64_t obj = rd64(list + 8 * j);
            if (stats) stats->objects++;
            unsigned layer = rd8(obj + 0x248);
            if (!((rd32(layers + 4ull * (layer >> 5)) >> (layer & 31)) & 1)) { if (stats) stats->layer++; continue; }
            uint64_t flags = rd64(obj + 0x58);
            if (flags & rd64(flag_mask)) { if (stats) stats->flags++; continue; }
            uint64_t ancestor = rd64(obj + 0x40);
            while (ancestor && !(rd64(ancestor + 0x58) & 0x10)) ancestor = rd64(ancestor + 0x40);
            if (ancestor) { if (stats) stats->parent++; continue; }
            if ((rd32(obj + 0x170) & excluded) || (int32_t)rd32(obj + 0x174) > priority) { if (stats) stats->masked++; continue; }
            if (!inside && !(flags & UINT64_C(0x40000000000))) {
                if (stats) stats->box++;
                if (!box_visible(&k, obj)) { if (stats) stats->box_rejected++; continue; }
            } else if (stats) stats->skipped_box++;
            uint8_t gate = rd8(self + 0x44);
            if (gate || detail) {
                if (stats) stats->size++;
                if (!size_visible(&k, obj)) { if (stats) stats->size_rejected++; continue; }
            }
            if (stats) stats->accepted++;
            if (collected) {
                if (collected->count == collected->capacity) {
                    uint32_t grown = collected->capacity ? collected->capacity * 2 : 4096;
                    uint64_t *items = realloc(collected->items, grown * sizeof(uint64_t));
                    if (!items) { collected->overflow = 1; continue; }
                    collected->items = items; collected->capacity = grown;
                }
                collected->items[collected->count++] = obj;
            } else {
                append(c, frame, out, obj);
            }
        }
    }
    /* Release of the cell list (140F8F8C3). */
    int32_t capacity = (int32_t)rd32(frame + 0x3C);
    if (capacity > 0) {
        wr32(frame + 0x38, 0);
        uint64_t allocator = rd64(frame + 0x40), data = rd64(frame + 0x30);
        c->rsp = frame;
        if (allocator) {
            c->rax = rd64(allocator); c->rcx = allocator; c->rdx = data;
            c->r8 = (uint64_t)(int64_t)capacity << 4; c->r9 = 8;
            push64(c, UINT64_C(0x140F8F8EB)); dispatch(c, rd64(c->rax + 8));
        } else {
            c->rcx = data;
            push64(c, UINT64_C(0x140F8F8F5)); L_000140E2BA60(c);
        }
    }
    c->rsp = rsp0 + 8;
    if (stats) stats->calls++;
    return 1;
}

static void add_stats(const Stats *s) {
    const uint64_t *v = (const uint64_t *)s;
    for (unsigned i = 0; i < sizeof(Stats) / sizeof(uint64_t); ++i) atomic_fetch_add_explicit(&stats_total[i], v[i], memory_order_relaxed);
    uint64_t calls = atomic_load_explicit(&stats_total[0], memory_order_relaxed);
    if (s->calls && !(calls % 600)) {
        uint64_t t[sizeof(Stats) / sizeof(uint64_t)];
        for (unsigned i = 0; i < sizeof(Stats) / sizeof(uint64_t); ++i) t[i] = atomic_load_explicit(&stats_total[i], memory_order_relaxed);
        fprintf(stderr, "VISIBILITY_STATS calls=%llu cells=%llu objects=%llu layer=%llu flags=%llu parent=%llu masked=%llu "
                "inside_or_flag=%llu box=%llu box_rejected=%llu size=%llu size_rejected=%llu accepted=%llu fallback=%llu\n",
                (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2], (unsigned long long)t[3],
                (unsigned long long)t[4], (unsigned long long)t[5], (unsigned long long)t[6], (unsigned long long)t[7],
                (unsigned long long)t[8], (unsigned long long)t[9], (unsigned long long)t[10], (unsigned long long)t[11],
                (unsigned long long)t[12], (unsigned long long)t[13]);
    }
}

static void verify(CPU *c) {
    static _Atomic uint64_t calls, mismatches, objects, fallbacks;
    static _Thread_local Collected collected;
    CPU entry = *c;
    uint64_t out = c->rdx;
    int32_t before = (int32_t)rd32(out + 8);
    fp_cpu_original_L_000140F8F530(c);
    int32_t after = (int32_t)rd32(out + 8);
    CPU done = *c, again = entry;
    collected.count = 0; collected.overflow = 0;
    int ran = run(&again, &collected, NULL);
    uint64_t n = atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed) + 1;
    if (!ran) {
        atomic_fetch_add_explicit(&fallbacks, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&objects, (uint64_t)(after - before), memory_order_relaxed);
        int32_t differ = -1;
        if (collected.overflow || (int64_t)collected.count != (int64_t)after - before) differ = 0;
        else for (uint32_t i = 0; i < collected.count; ++i)
            if (rd64(rd64(out) + 8ull * (uint64_t)(int64_t)(before + (int32_t)i)) != collected.items[i]) { differ = (int32_t)i; break; }
        int registers = again.rsp != done.rsp || again.rbx != done.rbx || again.rbp != done.rbp || again.rsi != done.rsi ||
            again.rdi != done.rdi || again.r12 != done.r12 || again.r13 != done.r13 || again.r14 != done.r14 || again.r15 != done.r15 ||
            memcmp(&again.xmm[6], &done.xmm[6], 10 * sizeof(XMM));
        if (differ >= 0 || registers) {
            uint64_t m = atomic_fetch_add_explicit(&mismatches, 1, memory_order_relaxed) + 1;
            if (m <= 20)
                fprintf(stderr, "VISIBILITY_VERIFY_MISMATCH call=%llu original=%d native=%u first_difference=%d registers=%d overflow=%u\n",
                        (unsigned long long)n, after - before, collected.count, differ, registers, collected.overflow);
        }
    }
    *c = done;
    if (!(n % 600))
        fprintf(stderr, "VISIBILITY_VERIFY calls=%llu objects=%llu mismatches=%llu fallbacks=%llu\n", (unsigned long long)n,
                (unsigned long long)atomic_load(&objects), (unsigned long long)atomic_load(&mismatches),
                (unsigned long long)atomic_load(&fallbacks));
}

void L_000140F8F530(CPU *c) {
    int mode = visibility_mode();
    prefetch_settings();
    if (!mode) { fp_cpu_original_L_000140F8F530(c); return; }
    if (mode == 2) { verify(c); return; }
    if (mode == 3) {
        Stats s; memset(&s, 0, sizeof(s));
        if (!run(c, NULL, &s)) { s.fallback = 1; fp_cpu_original_L_000140F8F530(c); }
        add_stats(&s);
        return;
    }
    if (!run(c, NULL, NULL)) fp_cpu_original_L_000140F8F530(c);
}
