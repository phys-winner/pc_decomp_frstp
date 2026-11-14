#include "cpu64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void L_000140E1E6C0(CPU *);
void fp_original_L_000140E1E6C0(CPU *);
static _Alignas(4096) unsigned char arena[16384], before[16384], expected[16384];
static CPU current, initial, reference;
static uint64_t seed = UINT64_C(0x677569642d6e6174), cases;
static uint32_t random32(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (uint32_t)seed; }

static void compare(void) {
    initial = current; memcpy(before, arena, sizeof(arena));
    fp_original_L_000140E1E6C0(&current);
    reference = current; memcpy(expected, arena, sizeof(arena));
    current = initial; memcpy(arena, before, sizeof(arena));
    L_000140E1E6C0(&current);
    if (memcmp(&current, &reference, sizeof(current)) || memcmp(arena, expected, sizeof(arena))) {
        size_t at = 0;
        while (at < sizeof(current) && ((unsigned char *)&current)[at] == ((unsigned char *)&reference)[at]) ++at;
        fprintf(stderr, "GUID_MISMATCH case=%llu cpu_offset=%zu expected=%02x actual=%02x\n", (unsigned long long)cases,
                at, at < sizeof(current) ? ((unsigned char *)&reference)[at] : 0,
                at < sizeof(current) ? ((unsigned char *)&current)[at] : 0);
        exit(1);
    }
    ++cases;
}

static void initialize(unsigned offset) {
    for (size_t i = 0; i < sizeof(current); ++i) ((unsigned char *)&current)[i] = (unsigned char)random32();
    current.rcx = (uintptr_t)arena + 128 + offset;
    current.rdx = (uintptr_t)arena + 512 + offset;
    current.rsp = random32() | ((uint64_t)random32() << 32);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char value = (unsigned char)random32();
        arena[128 + offset + i] = arena[512 + offset + i] = value;
    }
}

static uint64_t cpu_ns(void) {
#ifdef _WIN32
    return (uint64_t)clock() * UINT64_C(1000000000) / CLOCKS_PER_SEC;
#else
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + t.tv_nsec;
#endif
}

static double measure(void (*fn)(CPU *), unsigned count, unsigned kind) {
    initialize(0);
    if (kind == 1) arena[512 + 15] ^= 128;
    if (kind == 2) arena[512] ^= 128;
    uint64_t start = cpu_ns();
    for (unsigned i = 0; i < count; ++i) fn(&current);
    return (double)(cpu_ns() - start) / count;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--benchmark")) {
        printf("{\"success\":true,\"pairs\":[");
        for (unsigned i = 0; i < 8; ++i) for (unsigned kind = 0; kind < 3; ++kind) {
            double old_time, new_time;
            if (i & 1) { new_time = measure(L_000140E1E6C0, 500000, kind); old_time = measure(fp_original_L_000140E1E6C0, 500000, kind); }
            else { old_time = measure(fp_original_L_000140E1E6C0, 500000, kind); new_time = measure(L_000140E1E6C0, 500000, kind); }
            printf("%s{\"kind\":%u,\"original_ns\":%.3f,\"native_ns\":%.3f}", i || kind ? "," : "", kind, old_time, new_time);
        }
        puts("]}"); return 0;
    }
    for (unsigned position = 0; position < 16; ++position) {
        for (unsigned a = 0; a < 256; ++a) for (unsigned b = 0; b < 256; ++b) {
            unsigned offset = (a + b) & 15;
            initialize(offset); arena[128 + offset + position] = a; arena[512 + offset + position] = b;
            compare();
        }
    }
    for (unsigned iteration = 0; iteration < 4096; ++iteration) {
        unsigned offset = iteration & 15; initialize(offset);
        current.rdx = current.rcx + (iteration >> 4) % 17 - 8;
        compare();
        initialize(offset); current.rcx = (uintptr_t)&current + iteration % (sizeof(current) - 16); compare();
        initialize(offset); current.rdx = (uintptr_t)&current + iteration % (sizeof(current) - 16); compare();
        initialize(offset); current.rcx = (uintptr_t)arena + 4081 + offset; compare();
        initialize(offset); current.rdx = (uintptr_t)arena + 4081 + offset; compare();
    }
    printf("{\"success\":true,\"cases\":%llu,\"byte_pair_positions\":1048576,\"full_cpu_and_memory\":true,\"page_edge_and_aliases\":true}\n", (unsigned long long)cases);
    return 0;
}
