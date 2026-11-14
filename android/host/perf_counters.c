/* Diagnostic call counters, compiled only with -DFP_PERF_COUNTERS.
 * Lock-free: imports are keyed by their static name pointer. */
#include "perf_counters.h"
#ifdef FP_PERF_COUNTERS
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SLOTS 4096u
static struct { _Atomic(const char *) name; const char *dll; _Atomic uint64_t count; } table[SLOTS];
static _Atomic uint64_t kinds[FP_PERF_KINDS];
static _Atomic uint64_t last_dump_ns, ticks;
static const char *kind_names[FP_PERF_KINDS] = {"main", "import", "payload", "dependency", "dynamic",
    "d3d11", "steam", "openal", "shader", "dinput", "export", "miss"};

void fp_perf_count_import(const char *dll, const char *name) {
    size_t h = ((uintptr_t)name >> 3) * 0x9E3779B97F4A7C15ull >> 52;
    for (unsigned probe = 0; probe < SLOTS; ++probe) {
        size_t i = (h + probe) & (SLOTS - 1);
        const char *seen = atomic_load_explicit(&table[i].name, memory_order_acquire);
        if (seen == name) { atomic_fetch_add_explicit(&table[i].count, 1, memory_order_relaxed); return; }
        if (!seen) {
            const char *expected = NULL;
            if (atomic_compare_exchange_strong(&table[i].name, &expected, name) || expected == name) {
                table[i].dll = dll;
                atomic_fetch_add_explicit(&table[i].count, 1, memory_order_relaxed);
                return;
            }
        }
    }
}

void fp_perf_count_kind(unsigned kind) { atomic_fetch_add_explicit(&kinds[kind], 1, memory_order_relaxed); }

static int by_count(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

void fp_perf_tick(void) {
    if ((atomic_fetch_add_explicit(&ticks, 1, memory_order_relaxed) & 0xffff) != 0) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    uint64_t last = atomic_load(&last_dump_ns);
    if (last && now - last < 5000000000ull) return;
    if (!atomic_compare_exchange_strong(&last_dump_ns, &last, now)) return;
    char line[512]; int at = snprintf(line, sizeof line, "PERF_KINDS t=%" PRIu64, now);
    for (unsigned k = 0; k < FP_PERF_KINDS && at < (int)sizeof line - 40; ++k)
        at += snprintf(line + at, sizeof line - (size_t)at, " %s=%" PRIu64, kind_names[k],
                       atomic_exchange(&kinds[k], 0));
    fprintf(stderr, "%s\n", line);
    static uint64_t rows[SLOTS][2]; unsigned n = 0;
    for (unsigned i = 0; i < SLOTS; ++i) {
        uint64_t c = atomic_exchange(&table[i].count, 0);
        if (c) { rows[n][0] = c; rows[n][1] = i; ++n; }
    }
    qsort(rows, n, sizeof rows[0], by_count);
    for (unsigned r = 0; r < n && r < 60; ++r)
        fprintf(stderr, "PERF_IMPORT t=%" PRIu64 " count=%" PRIu64 " %s!%s\n", now, rows[r][0],
                table[rows[r][1]].dll, atomic_load(&table[rows[r][1]].name));
}
#endif
