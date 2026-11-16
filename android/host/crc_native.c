#include "cpu64.h"
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#endif
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

/* zlib crc32 (ISO-HDLC / gzip), guest L_000141C04BB0.
 * The load-window profile puts this software table CRC on ResourceManager.
 * ARM crc32b/crc32w/crc32x are that polynomial. crc32c* is Castagnoli and is not used.
 * Callers of L_000141C04BA0 read only eax. This entry still restores the
 * callee-saved registers and the ret slot, and leaves the flags the original
 * tail leaves: TEST of a zero remainder, or the last ADD r11d,-1 of a 1..3 byte tail.
 */
void fp_original_L_000141C04BB0(CPU *c);

static int crc_enabled(void) {
#ifdef __ANDROID__
    static _Atomic int enabled = -1;
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        int saved = errno;
        char setting[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.fp.native_crc", setting);
        errno = saved;
        value = setting[0] != '0';
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
#else
    return 1;
#endif
}

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_table_init(void) {
    if (crc_table_ready)
        return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

static uint32_t crc_bytes(uint32_t crc, const uint8_t *buf, uint32_t len) {
    crc = ~crc;
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    while (len && ((uintptr_t)buf & 7u)) {
        crc = __crc32b(crc, *buf++);
        --len;
    }
    while (len >= 8) {
        uint64_t word;
        memcpy(&word, buf, 8);
        crc = __crc32d(crc, word);
        buf += 8;
        len -= 8;
    }
    while (len) {
        crc = __crc32b(crc, *buf++);
        --len;
    }
#else
    crc_table_init();
    while (len--)
        crc = crc_table[(crc ^ *buf++) & 0xffu] ^ (crc >> 8);
#endif
    return ~crc;
}

/* Head bytes until a 4-byte boundary, then multiples of 4, then a 0..3 byte tail.
 * That is the original's alignment split. The tail's last flag write is what ret sees:
 * NOT and MOV do not change flags. */
static void crc_exit_flags(CPU *c, uint64_t address, uint32_t len) {
    uint32_t head = 0;
    if (len && (address & 3u)) {
        head = 4u - (uint32_t)(address & 3u);
        if (head > len)
            head = len;
    }
    if (((len - head) & 3u) == 0)
        flags_logicz(c, 0, 4);
    else
        flags_add(c, 1, 0xffffffffu, 4);
}

void L_000141C04BB0(CPU *c) {
    uint32_t crc = (uint32_t)c->rcx;
    uint64_t address = c->rdx;
    uint32_t len = (uint32_t)c->r8;
    if (!crc_enabled() || (len && !address) || (len && address > UINT64_MAX - len)) {
        fp_original_L_000141C04BB0(c);
        return;
    }
    uint64_t rdi = c->rdi, rbx = c->rbx;
    uint64_t rbp = c->rbp, rsi = c->rsi;
    uint64_t r12 = c->r12, r13 = c->r13, r14 = c->r14, r15 = c->r15;
    uint32_t result = len ? crc_bytes(crc, (const uint8_t *)(uintptr_t)address, len) : ~(~crc);
    SET32(c->rax, result);
    SET32(c->rcx, result);
    c->rdi = rdi;
    c->rbx = rbx;
    c->rbp = rbp;
    c->rsi = rsi;
    c->r12 = r12;
    c->r13 = r13;
    c->r14 = r14;
    c->r15 = r15;
    crc_exit_flags(c, address, len);
    c->rip = UINT64_C(0x141C04EA0);
    c->rsp += 8;
}
