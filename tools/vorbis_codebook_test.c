/* Device test for the native libvorbis codebook decoders in android/host/vorbis_native.c.
 *
 * tools/test_vorbis_codebook.py extracts the translated originals (vorbis_book_decodev_set
 * 0x141C54E60, vorbis_book_decodevv_add 0x141C54AF0, vorbis_book_decodev_add
 * 0x141C551D0, oggpack_look 0x140DFE110 and oggpack_adv 0x140DFE1F0) from
 * generated/frostpunk-x64, builds them with this file for arm64 Android and runs it on a
 * device. Each case runs the translated original, the native decoder and the guest-facing
 * wrapper (with and without its self-check) on identical copies of a random codebook (valid
 * Huffman trees built like libvorbis sharedbook.c, skewed trees with 32-bit codes,
 * single-entry and unused books), a random bitstream cut at a random place (end-of-packet
 * overflow), a random start position and special float values, and compares the return
 * value, the oggpack_buffer, the callee-saved registers and every word of the padded output
 * arrays.
 */
#include "vorbis_native.c"

#include <time.h>
#include <sys/mman.h>
#include "tables.h"
void frostpunk_unsupported(uint64_t va,const char *instruction){fprintf(stderr,"UNREACHABLE_ORACLE %llx %s\n",(unsigned long long)va,instruction);abort();}

int64_t g_image_delta;
void L_000140DFE110(CPU *c);
void L_000140DFE1F0(CPU *c);
void dispatch(CPU *c, uint64_t target) {
    if(target==UINT64_C(0x140DFE1F0)){c->rip=target;L_000140DFE1F0(c);return;}
    if (target != UINT64_C(0x140DFE110)) {
        fprintf(stderr, "unexpected dispatch 0x%" PRIx64 "\n", target);
        abort();
    }
    c->rip = target;
    L_000140DFE110(c);
}
void dispatch_jmp(CPU *c, uint64_t target) { dispatch(c, target); }
void L_000141C5C490(CPU *c) { (void)c; abort(); }

static uint64_t rng_state = UINT64_C(0x9E3779B97F4A7C15);
static uint64_t rnd(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(0x2545F4914F6CDD1D);
}
static uint32_t below(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

static uint32_t random_word(void) {
    static const uint32_t special[] = {0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
                                       0xffc00000u, 0x7f800001u, 0x7fa5a5a5u, 0xffbfffffu, 0x00000001u,
                                       0x807fffffu, 0x3f800000u, 0x7f7fffffu, 0xff7fffffu, 0x00800000u};
    if (!below(8)) return special[below(sizeof(special) / sizeof(*special))];
    float f = (float)((int32_t)(uint32_t)(rnd() >> 40) - (1 << 23)) / 4096.0f;
    uint32_t w;
    memcpy(&w, &f, 4);
    return w;
}

/* ---- codebooks, built like vorbis_book_init_decode ---- */

#define MAX_USED 640
#define MAX_DIM 12
typedef struct {
    codebook book;
    uint32_t codelist[MAX_USED];
    int8_t length_storage[MAX_USED + 1];
    uint32_t firsttable[256];
    uint32_t values[MAX_USED * MAX_DIM];
} test_book;

/* _make_words for a list without unused entries: MSb-first code words; 0 for a bad tree. */
static int make_words(const int8_t *l, int n, uint32_t *r) {
    uint32_t marker[33] = {0};
    int count = 0;
    for (int i = 0; i < n; i++) {
        int length = l[i];
        uint32_t entry = marker[length];
        if (length < 32 && (entry >> length)) return 0;
        r[count++] = entry;
        for (int j = length; j > 0; j--) {
            if (marker[j] & 1) {
                if (j == 1) marker[1]++;
                else marker[j] = marker[j - 1] << 1;
                break;
            }
            marker[j]++;
        }
        for (int j = length + 1; j < 33; j++)
            if ((marker[j] >> 1) == entry) {
                entry = marker[j];
                marker[j] = marker[j - 1] << 1;
            } else
                break;
    }
    if (!(count == 1 && marker[2] == 2))
        for (int i = 1; i < 33; i++)
            if (marker[i] & (0xffffffffu >> (32 - i))) return 0;
    return 1;
}

static int ilog(unsigned v) {
    int r = 0;
    while (v) { r++; v >>= 1; }
    return r;
}

static void make_book(test_book *t) {
    memset(t, 0, sizeof(*t));
    codebook *b = &t->book;
    b->dim = 1 + (int32_t)below(MAX_DIM);
    int used = below(20) ? (below(3) ? 2 + (int)below(40) : 2 + (int)below(MAX_USED - 2)) : (below(2) ? 1 : 0);
    b->entries = used;
    b->used_entries = used;
    if (!used) return;
    int8_t depth[MAX_USED];
    int count = 1;
    if (used == 1) {
        depth[0] = 1;
    } else {
        depth[0] = 0;
        int maxlen = ilog((unsigned)used - 1) + (int)below(33u - (unsigned)ilog((unsigned)used - 1));
        if (maxlen > 32) maxlen = 32;
        int skew = !below(4);
        while (count < used) {
            int pick = skew && depth[count - 1] < maxlen ? count - 1 : (int)below((uint32_t)count);
            for (int tries = 0; depth[pick] >= maxlen; ++tries) pick = tries < 8 ? (int)below((uint32_t)count) : (pick + 1) % count;
            depth[pick]++;
            depth[count++] = depth[pick];
        }
        for (int i = used - 1; i > 0; --i) {
            int j = (int)below((uint32_t)i + 1);
            int8_t s = depth[i]; depth[i] = depth[j]; depth[j] = s;
        }
    }
    uint32_t words[MAX_USED], aligned[MAX_USED];
    int order[MAX_USED];
    if (!make_words(depth, used, words)) {
        fprintf(stderr, "bad tree used=%d\n", used);
        abort();
    }
    for (int i = 0; i < used; i++) {
        aligned[i] = depth[i] == 32 ? words[i] : words[i] << (32 - depth[i]);
        order[i] = i;
    }
    for (int i = 1; i < used; i++) { /* insertion sort by aligned word */
        int k = order[i], j = i - 1;
        while (j >= 0 && aligned[order[j]] > aligned[k]) { order[j + 1] = order[j]; --j; }
        order[j + 1] = k;
    }
    int8_t *lengths = &t->length_storage[1];
    int maxlength = 0;
    for (int i = 0; i < used; i++) {
        t->codelist[i] = aligned[order[i]];
        lengths[i] = depth[order[i]];
        if (lengths[i] > maxlength) maxlength = lengths[i];
    }
    b->codelist = (uint64_t)(uintptr_t)t->codelist;
    b->dec_codelengths = (uint64_t)(uintptr_t)lengths;
    b->dec_firsttable = (uint64_t)(uintptr_t)t->firsttable;
    b->dec_maxlength = maxlength;
    b->valuelist = (uint64_t)(uintptr_t)t->values;
    for (int i = 0; i < used * b->dim; i++) t->values[i] = random_word();
    if (used == 1 && maxlength == 1) {
        b->dec_firsttablen = 1;
        t->firsttable[0] = t->firsttable[1] = 1;
        return;
    }
    int tablen = ilog((unsigned)used) - 4;
    if (tablen < 5) tablen = 5;
    if (tablen > 8) tablen = 8;
    b->dec_firsttablen = tablen;
    int tabn = 1 << tablen;
    for (int i = 0; i < used; i++)
        if (lengths[i] <= tablen) {
            uint32_t orig = __builtin_bitreverse32(t->codelist[i]);
            for (int j = 0; j < (1 << (tablen - lengths[i])); j++) t->firsttable[orig | ((uint32_t)j << lengths[i])] = (uint32_t)i + 1;
        }
    uint32_t mask = 0xfffffffeu << (31 - tablen);
    long lo = 0, hi = 0;
    for (int i = 0; i < tabn; i++) {
        uint32_t word = (uint32_t)i << (32 - tablen);
        if (t->firsttable[__builtin_bitreverse32(word)] == 0) {
            while ((lo + 1) < used && t->codelist[lo + 1] <= word) lo++;
            while (hi < used && word >= (t->codelist[hi] & mask)) hi++;
            unsigned long loval = (unsigned long)lo, hival = (unsigned long)(used - hi);
            if (loval > 0x7fff) loval = 0x7fff;
            if (hival > 0x7fff) hival = 0x7fff;
            t->firsttable[__builtin_bitreverse32(word)] = 0x80000000u | (uint32_t)(loval << 15) | (uint32_t)hival;
        }
    }
}

/* ---- bitstreams ---- */

#define MAX_BYTES 2048
static uint8_t stream[MAX_BYTES + 16];

static ogg_bits make_bits(void) {
    uint32_t pick = below(3);
    int32_t storage = (int32_t)(pick == 0 ? below(17) : pick == 1 ? 16 + below(241) : 256 + below(MAX_BYTES - 255));
    for (int32_t i = 0; i < storage + 16; i++) stream[i] = (uint8_t)rnd();
    ogg_bits b = {0};
    b.buffer = (uint64_t)(uintptr_t)stream;
    b.storage = storage;
    if (!below(32)) {
        b.ptr = 0;
        b.endbyte = storage;
        b.endbit = 1;
        return b;
    }
    b.endbyte = below(2) ? (int32_t)below(4) : (int32_t)below((uint32_t)storage + 1);
    if (b.endbyte > storage) b.endbyte = storage;
    b.endbit = (int32_t)below(8);
    b.ptr = b.buffer + (uint64_t)b.endbyte;
    return b;
}

/* ---- guest calls ---- */

static uint8_t guest_stack[1 << 16];
static CPU cpu;
static uint64_t saved[8];

static void guest_setup(CPU *c, uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9, uint64_t arg5, uint64_t arg6) {
    memset(c, 0, sizeof(*c));
    saved[0] = c->rbx = rnd(); saved[1] = c->rbp = rnd(); saved[2] = c->rsi = rnd(); saved[3] = c->rdi = rnd();
    saved[4] = c->r12 = rnd(); saved[5] = c->r13 = rnd(); saved[6] = c->r14 = rnd(); saved[7] = c->r15 = rnd();
    c->rsp = (((uint64_t)(uintptr_t)(guest_stack + sizeof(guest_stack) - 512)) & ~UINT64_C(15)) - 8;
    wr64(c->rsp, UINT64_C(0x00000001deadbeef));
    wr64(c->rsp + 0x28, arg5);
    wr64(c->rsp + 0x30, arg6);
    c->rcx = rcx; c->rdx = rdx; c->r8 = r8; c->r9 = r9;
}

static int guest_finish(const CPU *c, uint64_t entry_rsp, const char *what, long long test) {
    uint64_t now[8] = {c->rbx, c->rbp, c->rsi, c->rdi, c->r12, c->r13, c->r14, c->r15};
    if (c->rsp != entry_rsp + 8 || memcmp(now, saved, sizeof(now)) || (c->rax >> 32)) {
        fprintf(stderr, "FAIL %s test=%lld: rsp/callee-saved/rax upper\n", what, test);
        return 0;
    }
    return 1;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
static double time_lifted[3], time_native[3];
static long long calls_timed[3];

/* ---- decodev_set ---- */

#define SET_PAD 16
#define SET_MAX 420
static uint32_t set_out[3][SET_PAD + SET_MAX + SET_PAD];

static int test_decodev_set(test_book *t, long long test) {
    int32_t n = (int32_t)below(SET_MAX + 3) - 2;
    ogg_bits start = make_bits(), bits[3] = {start, start, start};
    for (int k = 0; k < SET_PAD + SET_MAX + SET_PAD; k++) set_out[0][k] = set_out[1][k] = set_out[2][k] = random_word();
    uint64_t a[3];
    for (int k = 0; k < 3; k++) a[k] = (uint64_t)(uintptr_t)&set_out[k][SET_PAD];
    uint64_t r9 = (rnd() << 32) | (uint32_t)n;

    guest_setup(&cpu, (uint64_t)(uintptr_t)&t->book, a[0], (uint64_t)(uintptr_t)&bits[0], r9, rnd(), rnd());
    uint64_t entry_rsp = cpu.rsp;
    double t0 = now_ns();
    L_000141C54E60(&cpu);
    double t1 = now_ns();
    if (!guest_finish(&cpu, entry_rsp, "decodev_set lifted", test)) return 0;
    int32_t want = (int32_t)(uint32_t)cpu.rax;

    double t2 = now_ns();
    int32_t got = book_decodev_set(&t->book, a[1], &bits[1], n);
    double t3 = now_ns();
    time_lifted[0] += t1 - t0;
    time_native[0] += t3 - t2;
    calls_timed[0]++;

    guest_setup(&cpu, (uint64_t)(uintptr_t)&t->book, a[2], (uint64_t)(uintptr_t)&bits[2], r9, rnd(), rnd());
    entry_rsp = cpu.rsp;
    native_decodev_set(&cpu);
    if (!guest_finish(&cpu, entry_rsp, "decodev_set wrapper", test)) return 0;
    int32_t wrapped = (int32_t)(uint32_t)cpu.rax;

    if (want != got || want != wrapped || memcmp(&bits[0], &bits[1], sizeof(ogg_bits)) ||
        memcmp(&bits[0], &bits[2], sizeof(ogg_bits)) || memcmp(set_out[0], set_out[1], sizeof(set_out[0])) ||
        memcmp(set_out[0], set_out[2], sizeof(set_out[0]))) {
        fprintf(stderr,
                "FAIL decodev_set test=%lld n=%d dim=%d used=%d tablen=%d maxlen=%d storage=%d start=%d/%d "
                "ret=%d/%d/%d endbyte=%d/%d/%d endbit=%d/%d/%d\n",
                test, n, t->book.dim, t->book.used_entries, t->book.dec_firsttablen, t->book.dec_maxlength,
                start.storage, start.endbyte, start.endbit, want, got, wrapped, bits[0].endbyte, bits[1].endbyte,
                bits[2].endbyte, bits[0].endbit, bits[1].endbit, bits[2].endbit);
        return 0;
    }
    return 1;
}

static int test_decodev_add(test_book *t, long long test) {
    int32_t n = (int32_t)below(SET_MAX + 3) - 2;
    ogg_bits start = make_bits(), bits[3] = {start, start, start};
    for (int k = 0; k < SET_PAD + SET_MAX + SET_PAD; k++) set_out[0][k] = set_out[1][k] = set_out[2][k] = random_word();
    uint64_t a[3];
    for (int k = 0; k < 3; k++) a[k] = (uint64_t)(uintptr_t)&set_out[k][SET_PAD];
    uint64_t r9 = (rnd() << 32) | (uint32_t)n;

    guest_setup(&cpu, (uint64_t)(uintptr_t)&t->book, a[0], (uint64_t)(uintptr_t)&bits[0], r9, rnd(), rnd());
    uint64_t entry_rsp = cpu.rsp;
    double t0 = now_ns();
    L_000141C551D0(&cpu);
    double t1 = now_ns();
    if (!guest_finish(&cpu, entry_rsp, "decodev_add lifted", test)) return 0;
    int32_t want = (int32_t)(uint32_t)cpu.rax;

    double t2 = now_ns();
    int32_t got = book_decodev_add(&t->book, a[1], &bits[1], n);
    double t3 = now_ns();
    time_lifted[2] += t1 - t0;
    time_native[2] += t3 - t2;
    calls_timed[2]++;

    guest_setup(&cpu, (uint64_t)(uintptr_t)&t->book, a[2], (uint64_t)(uintptr_t)&bits[2], r9, rnd(), rnd());
    entry_rsp = cpu.rsp;
    native_decodev_add(&cpu);
    if (!guest_finish(&cpu, entry_rsp, "decodev_add wrapper", test)) return 0;
    int32_t wrapped = (int32_t)(uint32_t)cpu.rax;

    if (want != got || want != wrapped || memcmp(&bits[0], &bits[1], sizeof(ogg_bits)) ||
        memcmp(&bits[0], &bits[2], sizeof(ogg_bits)) || memcmp(set_out[0], set_out[1], sizeof(set_out[0])) ||
        memcmp(set_out[0], set_out[2], sizeof(set_out[0]))) {
        fprintf(stderr,
                "FAIL decodev_add test=%lld n=%d dim=%d used=%d tablen=%d maxlen=%d storage=%d start=%d/%d "
                "ret=%d/%d/%d endbyte=%d/%d/%d endbit=%d/%d/%d\n",
                test, n, t->book.dim, t->book.used_entries, t->book.dec_firsttablen, t->book.dec_maxlength,
                start.storage, start.endbyte, start.endbit, want, got, wrapped, bits[0].endbyte, bits[1].endbyte,
                bits[2].endbyte, bits[0].endbit, bits[1].endbit, bits[2].endbit);
        return 0;
    }
    return 1;
}

/* ---- decodevv_add ---- */

#define VV_PAD 80
#define VV_ROWS 512
#define VV_MAX_CH 6
static uint32_t vv_out[3][VV_MAX_CH][VV_PAD + VV_ROWS + VV_PAD];

static int test_decodevv_add(test_book *t, long long test) {
    int32_t ch = 1 + (int32_t)below(VV_MAX_CH);
    int32_t offset = (int32_t)below(365) - 64;
    int32_t n = (int32_t)below(101) - 4;
    if (!below(4)) n = (int32_t)(t->book.dim * (1 + below(8))) * ch; /* res2-like whole partitions */
    if (offset / ch + VV_PAD < 0 || (offset + n) / ch + MAX_DIM + 2 > VV_ROWS) n = 0;
    ogg_bits start = make_bits(), bits[3] = {start, start, start};
    uint64_t pointers[3][VV_MAX_CH];
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < VV_MAX_CH; k++) pointers[s][k] = (uint64_t)(uintptr_t)&vv_out[s][k][VV_PAD];
    for (int k = 0; k < VV_MAX_CH; k++)
        for (int r = 0; r < VV_PAD + VV_ROWS + VV_PAD; r++) vv_out[0][k][r] = vv_out[1][k][r] = vv_out[2][k][r] = random_word();
    uint64_t r8 = (rnd() << 32) | (uint32_t)offset, r9 = (rnd() << 32) | (uint32_t)ch;
    uint64_t arg6 = (rnd() << 32) | (uint32_t)n;

    guest_setup(&cpu, (uint64_t)(uintptr_t)&t->book, (uint64_t)(uintptr_t)pointers[0], r8, r9,
                (uint64_t)(uintptr_t)&bits[0], arg6);
    uint64_t entry_rsp = cpu.rsp;
    double t0 = now_ns();
    L_000141C54AF0(&cpu);
    double t1 = now_ns();
    if (!guest_finish(&cpu, entry_rsp, "decodevv_add lifted", test)) return 0;
    int32_t want = (int32_t)(uint32_t)cpu.rax;

    int32_t got = 0;
    double t2 = now_ns();
    if (t->book.used_entries > 0) got = book_decodevv_add(&t->book, (uint64_t)(uintptr_t)pointers[1], offset, ch, &bits[1], n);
    double t3 = now_ns();
    time_lifted[1] += t1 - t0;
    time_native[1] += t3 - t2;
    calls_timed[1]++;

    guest_setup(&cpu, (uint64_t)(uintptr_t)&t->book, (uint64_t)(uintptr_t)pointers[2], r8, r9,
                (uint64_t)(uintptr_t)&bits[2], arg6);
    entry_rsp = cpu.rsp;
    native_decodevv_add(&cpu);
    if (!guest_finish(&cpu, entry_rsp, "decodevv_add wrapper", test)) return 0;
    int32_t wrapped = (int32_t)(uint32_t)cpu.rax;

    if (want != got || want != wrapped || memcmp(&bits[0], &bits[1], sizeof(ogg_bits)) ||
        memcmp(&bits[0], &bits[2], sizeof(ogg_bits)) || memcmp(vv_out[0], vv_out[1], sizeof(vv_out[0])) ||
        memcmp(vv_out[0], vv_out[2], sizeof(vv_out[0]))) {
        fprintf(stderr,
                "FAIL decodevv_add test=%lld ch=%d offset=%d n=%d dim=%d used=%d tablen=%d maxlen=%d storage=%d "
                "start=%d/%d ret=%d/%d/%d endbyte=%d/%d/%d endbit=%d/%d/%d\n",
                test, ch, offset, n, t->book.dim, t->book.used_entries, t->book.dec_firsttablen,
                t->book.dec_maxlength, start.storage, start.endbyte, start.endbit, want, got, wrapped,
                bits[0].endbyte, bits[1].endbyte, bits[2].endbyte, bits[0].endbit, bits[1].endbit, bits[2].endbit);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    long long cases = argc > 1 ? atoll(argv[1]) : 100000;
    if (argc > 2) rng_state = (uint64_t)strtoull(argv[2], NULL, 0) | 1u;
    static uint32_t mask_table[33];
    for (int i = 0; i < 33; i++) mask_table[i] = i == 32 ? 0xffffffffu : (1u << i) - 1u;
    void *tables=mmap((void*)(uintptr_t)0x141c50000ull,0x5a0000u,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    if(tables==MAP_FAILED || (uintptr_t)tables!=0x141c50000ull){fprintf(stderr,"Oracle table mapping failed\n");return 2;}
    memcpy((void*)(uintptr_t)0x1421E56C0ull,mask_table,sizeof(mask_table));
    memcpy((void*)(uintptr_t)0x141C556B0ull,original_jump_table,sizeof(original_jump_table));
    if(mprotect(tables,0x5a0000u,PROT_READ))return 2;
    g_image_delta=0;
    vorbis_configure();
    atomic_store(&codebook_state, 1); /* ignore device properties */
    atomic_store(&verify_always, 0);
    static test_book book;
    long long failures = 0, minus_one[3] = {0, 0, 0};
    for (long long test = 0; test < cases && failures < 10; test++) {
        if (!(test % 16)) make_book(&book);
        int ok = test%3==2?test_decodev_add(&book,test):test%3==1?test_decodevv_add(&book,test):test_decodev_set(&book,test);
        if (!ok) failures++;
        else if ((int32_t)(uint32_t)cpu.rax == -1) minus_one[test % 3]++;
    }
    int state = atomic_load(&codebook_state);
    printf("VORBIS_CODEBOOK_TEST cases=%lld failures=%lld self_check_state=%d verified=%u/%u "
           "returned_minus_one=%lld/%lld lifted_ns=%.0f/%.0f native_ns=%.0f/%.0f\n",
           cases, failures, state, atomic_load(&codebook_verified[0]), atomic_load(&codebook_verified[1]),
           minus_one[0], minus_one[1], time_lifted[0] / (double)(calls_timed[0] ? calls_timed[0] : 1),
           time_lifted[1] / (double)(calls_timed[1] ? calls_timed[1] : 1),
           time_native[0] / (double)(calls_timed[0] ? calls_timed[0] : 1),
           time_native[1] / (double)(calls_timed[1] ? calls_timed[1] : 1));
    printf("VORBIS_ADD_TEST verified=%u returned_minus_one=%lld lifted_ns=%.0f native_ns=%.0f\n",atomic_load(&codebook_verified[2]),minus_one[2],time_lifted[2]/(calls_timed[2]?calls_timed[2]:1),time_native[2]/(calls_timed[2]?calls_timed[2]:1));
    return failures || state <= 0 ? 1 : 0;
}
