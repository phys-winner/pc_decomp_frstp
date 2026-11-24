/* Stress test of cpu64.h's locked read-modify-write helpers (lock_<op><W>, lock_xchg<W>, lock_neg<W>,
 * lock_not<W>): several threads hammer the same words, aligned and misaligned; the final values must be
 * exact.  Build and run with tools/test_atomic_helpers.py --serial SERIAL. */
#include "cpu64.h"
#include <pthread.h>
#include <stdio.h>

int guest_lock_unaligned;   /* the host's spinlock (android/host/win64_sync.c in the runtime) */

#ifdef STRESS_PLAIN
/* Negative control: the old plain load/op/store lowering must lose updates under this load. */
static inline uint32_t plain_add32(uint64_t a, uint32_t v) { uint32_t o = *(volatile uint32_t *)(uintptr_t)a; *(volatile uint32_t *)(uintptr_t)a = o + v; return o; }
#define lock_add32 plain_add32
#endif

enum { THREADS = 8, ROUNDS = 200000 };
static _Alignas(64) unsigned char arena[256];
static uint64_t base_aligned, base_misaligned32, base_misaligned64, base_cross16, base_byte, base_word, base_neg, base_xchg;
static volatile int start_flag;
static uint64_t xchg_sum[THREADS];

static void *worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    while (!__atomic_load_n(&start_flag, __ATOMIC_ACQUIRE)) {}
    uint64_t token = (uint64_t)(id + 1) << 32, got = 0;
    for (unsigned i = 0; i < ROUNDS; ++i) {
        lock_add32(base_aligned, 3); lock_sub32(base_aligned, 1);           /* +2 per round */
        lock_add64(base_aligned + 8, 1);                                     /* +1 */
        lock_add32(base_misaligned32, 1);                                    /* misaligned 32-bit: +1 */
        lock_add64(base_misaligned64, 2); lock_sub64(base_misaligned64, 1);  /* misaligned 64-bit: +1 */
        lock_add64(base_cross16, 1);                                         /* crosses a 16-byte line: +1 */
        lock_add8(base_byte, 1);                                             /* wraps mod 256 */
        lock_add16(base_word, 1);                                            /* misaligned 16-bit, wraps */
        lock_or32(base_aligned + 16, 1u << id); lock_and32(base_aligned + 20, ~(1u << id));
        lock_xor32(base_aligned + 24, 1u << id);                             /* ROUNDS is even: back to 0 */
        lock_not32(base_aligned + 28);                                       /* THREADS*ROUNDS even: unchanged */
        lock_neg32(base_neg);                                                /* even count: unchanged */
        /* xchg ring: every value handed in is taken out exactly once. */
        got += lock_xchg64(base_xchg, token + i) & 0xFFFFFFFFu;
    }
    xchg_sum[id] = got;
    return NULL;
}

int main(void) {
    uint64_t a = (uint64_t)(uintptr_t)arena;
    base_aligned = a; base_misaligned32 = a + 66; base_misaligned64 = a + 99; base_cross16 = a + 124;
    base_byte = a + 140; base_word = a + 147; base_neg = a + 152; base_xchg = a + 160;
    wr32(a + 20, 0xFFFFFFFFu); wr32(a + 28, 0x12345678u); wr32(base_neg, 77u);
    pthread_t t[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i);
    __atomic_store_n(&start_flag, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < THREADS; ++i) pthread_join(t[i], NULL);
    uint64_t total = (uint64_t)THREADS * ROUNDS, m32, m64, c16, last;
    memcpy(&m32, (void *)(uintptr_t)base_misaligned32, 4); m32 &= 0xFFFFFFFFu;
    memcpy(&m64, (void *)(uintptr_t)base_misaligned64, 8);
    memcpy(&c16, (void *)(uintptr_t)base_cross16, 8);
    uint16_t w; memcpy(&w, (void *)(uintptr_t)base_word, 2);
    memcpy(&last, (void *)(uintptr_t)base_xchg, 8);
    /* Sum of the low halves of all tokens put in, minus the one still in the word, plus the initial 0. */
    uint64_t put = (uint64_t)THREADS * ((uint64_t)ROUNDS * (ROUNDS - 1) / 2), taken = 0;
    for (unsigned i = 0; i < THREADS; ++i) taken += xchg_sum[i];
    int ok = 1;
#define CHECK(name, have, want) do { uint64_t h_ = (have), w_ = (want); \
    printf("%-14s %llu (want %llu)%s\n", name, (unsigned long long)h_, (unsigned long long)w_, h_ == w_ ? "" : "  FAIL"); ok &= h_ == w_; } while (0)
    CHECK("add/sub32", rd32(a), 2 * total & 0xFFFFFFFFu);
    CHECK("add64", rd64(a + 8), total);
    CHECK("misaligned32", m32, total & 0xFFFFFFFFu);
    CHECK("misaligned64", m64, total);
    CHECK("cross16", c16, total);
    CHECK("add8", rd8(base_byte), total & 0xFFu);
    CHECK("add16", w, total & 0xFFFFu);
    CHECK("or32", rd32(a + 16), (1u << THREADS) - 1);
    CHECK("and32", rd32(a + 20), 0xFFFFFFFFu & ~((1u << THREADS) - 1));
    CHECK("xor32", rd32(a + 24), 0);
    CHECK("not32", rd32(a + 28), 0x12345678u);
    CHECK("neg32", rd32(base_neg), 77u);
    CHECK("xchg64", taken + (last & 0xFFFFFFFFu), put);
    printf("ATOMIC_STRESS %s threads=%u rounds=%u\n", ok ? "PASS" : "FAIL", THREADS, ROUNDS);
    return ok ? 0 : 1;
}
