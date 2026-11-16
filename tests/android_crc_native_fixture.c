/* Host check of the zlib CRC wrapper. The ARM crc32* path is the same polynomial
 * and is selected only when the translation unit is built with +crc. */
#include "cpu64.h"
#include <stdio.h>
#include <string.h>

int64_t g_image_delta;

void L_000141C04BB0(CPU *c);

void fp_original_L_000141C04BB0(CPU *c) {
    fprintf(stderr, "original called\n");
    c->rax = 0xdead;
}

static uint32_t ref_crc(uint32_t crc, const uint8_t *buf, uint32_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return ~crc;
}

static int check(const char *name, const uint8_t *buf, uint32_t len, uint32_t init, int align) {
    uint8_t raw[256];
    uint8_t stack[64];
    memset(raw, 0x5a, sizeof raw);
    memcpy(raw + align, buf, len);
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.rcx = init;
    cpu.rdx = (uint64_t)(uintptr_t)(raw + align);
    cpu.r8 = len;
    cpu.rdi = 0x1111;
    cpu.rbx = 0x2222;
    cpu.rbp = 0x3333;
    cpu.rsi = 0x4444;
    cpu.r12 = 0x5555;
    cpu.r13 = 0x6666;
    cpu.r14 = 0x7777;
    cpu.r15 = 0x8888;
    cpu.rsp = (uint64_t)(uintptr_t)(stack + 32);
    uint32_t expect = ref_crc(init, buf, len);
    L_000141C04BB0(&cpu);
    int bad = 0;
    if ((uint32_t)cpu.rax != expect || (uint32_t)cpu.rcx != expect)
        bad = 1;
    if (cpu.rdi != 0x1111 || cpu.rbx != 0x2222 || cpu.rbp != 0x3333 || cpu.rsi != 0x4444 ||
        cpu.r12 != 0x5555 || cpu.r13 != 0x6666 || cpu.r14 != 0x7777 || cpu.r15 != 0x8888)
        bad = 1;
    if (cpu.rsp != (uint64_t)(uintptr_t)(stack + 40) || cpu.rip != UINT64_C(0x141C04EA0))
        bad = 1;
    uint32_t head = 0;
    uint64_t address = (uint64_t)(uintptr_t)(raw + align);
    if (len && (address & 3u)) {
        head = 4u - (uint32_t)(address & 3u);
        if (head > len)
            head = len;
    }
    int tail = ((len - head) & 3u) != 0;
    if (cpu.zf != 1 || cpu.cf != (tail ? 1u : 0u))
        bad = 1;
    printf("%s expect 0x%08x got 0x%08x cf %u zf %u %s\n", name, expect, (uint32_t)cpu.rax,
           (unsigned)cpu.cf, (unsigned)cpu.zf, bad ? "FAIL" : "ok");
    return bad;
}

int main(void) {
    int bad = 0;
    const uint8_t digits[] = "123456789";
    uint8_t seq[64];
    for (int i = 0; i < 64; ++i)
        seq[i] = (uint8_t)i;
    bad |= check("empty", digits, 0, 0, 0);
    bad |= check("digits", digits, 9, 0, 0);
    bad |= check("digits-init", digits, 9, 0x12345678u, 0);
    bad |= check("digits-unaligned", digits, 9, 0, 1);
    bad |= check("seq", seq, 64, 1, 0);
    bad |= check("seq-unaligned", seq, 64, 1, 3);
    uint8_t zeros[31];
    memset(zeros, 0, sizeof zeros);
    bad |= check("zeros31", zeros, 31, 0, 0);
    uint8_t aaa[7];
    memset(aaa, 'a', sizeof aaa);
    bad |= check("a7", aaa, 7, 0xffffffffu, 0);
    bad |= check("one", digits, 1, 0, 0);
    bad |= check("four", digits, 4, 0, 0);
    printf("%s\n", bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}
