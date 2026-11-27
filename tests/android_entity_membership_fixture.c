#define _GNU_SOURCE
#include "cpu64.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define ES3_EH_ENTER(...)
#define ES3_EH_LEAVE()
int64_t g_image_delta;

void dispatch(CPU *c, uint64_t target) {
    assert(target == UINT64_C(0x141AE0960));
    c->rsp += 8;
}
void dispatch_jmp(CPU *c, uint64_t target) {
    (void)c; (void)target;
    abort();
}

#include "membership_baseline.inc"
#include "membership_candidate.inc"

static _Alignas(16) unsigned char stack[2048], initial_stack[2048], reference_stack[2048];
static _Alignas(16) unsigned char entity[160];
static uintptr_t fault_address;
static unsigned checks;

static void expected_fault(int signal_number, siginfo_t *info, void *context) {
    (void)context;
    _exit(signal_number == SIGSEGV && (uintptr_t)info->si_addr == fault_address ? 101 : 102);
}

static CPU initial_cpu(uint64_t system, uint64_t ent, uint32_t id) {
    CPU cpu;
    for (unsigned i = 0; i < sizeof(cpu); ++i)
        ((unsigned char *)&cpu)[i] = (unsigned char)(i * 37 + id);
    cpu.cf = cpu.zf = cpu.sf = cpu.of = cpu.pf = cpu.af = 0;
    cpu.mxcsr = 0x1f80;
    cpu.rcx = system;
    cpu.rdx = ent;
    cpu.rsp = (uintptr_t)(stack + 1024);
    for (unsigned i = 0; i < sizeof(stack); ++i) initial_stack[i] = (unsigned char)(i + id);
    wr64(cpu.rsp, UINT64_C(0xe5ee0010));
    memcpy(initial_stack + 1024, stack + 1024, 8);
    return cpu;
}

static void compare(uint64_t system, uint64_t ent, uint32_t id, unsigned expected) {
    CPU before = initial_cpu(system, ent, id), reference = before, candidate = before;
    memcpy(stack, initial_stack, sizeof(stack));
    membership_baseline(&reference);
    memcpy(reference_stack, stack, sizeof(stack));
    memcpy(stack, initial_stack, sizeof(stack));
    membership_candidate(&candidate);
    assert(!memcmp(&reference, &candidate, sizeof(reference)));
    assert(!memcmp(reference_stack, stack, sizeof(stack)));
    assert((candidate.rax & 255) == expected && candidate.rsp == before.rsp + 8);
    ++checks;
}

int main(void) {
    unsigned char *image = calloc(1, 0x3000000);
    assert(image);
    g_image_delta = (int64_t)(uintptr_t)image - INT64_C(0x140000000);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *arena = mmap(NULL, page * 8, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(arena != MAP_FAILED && !mprotect(arena, page * 7, PROT_READ | PROT_WRITE));
    uint64_t system = (uintptr_t)(arena + page * 7 - 0x2060);
    fault_address = (uintptr_t)(arena + page * 7);

    for (unsigned debug = 0; debug < 2; ++debug) {
        wr32(GVA(0x14296FAE0), debug);
        for (uint32_t id = 0; id < 65536; ++id) {
            wr32((uintptr_t)entity + 0x50, id);
            uint64_t word = system + 0x60 + 4 * (id >> 5);
            for (unsigned present = 0; present < 2; ++present) {
                uint32_t mask = UINT32_C(1) << (id & 31);
                wr32(word, (UINT32_C(0xa5a55a5a) & ~mask) | (present ? mask : 0));
                compare(system, (uintptr_t)entity, id, present);
            }
        }
    }
    wr32(GVA(0x14296FAE0), 0);
    compare(fault_address, 0, 0, 0);

    const uint32_t invalid[] = {65536, 65567, 65568, 0x100000, 0x3f800000, 0x80000000, UINT32_MAX};
    for (unsigned debug = 0; debug < 2; ++debug) {
        wr32(GVA(0x14296FAE0), debug);
        for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
            wr32((uintptr_t)entity + 0x50, invalid[i]);
            CPU before = initial_cpu(system, (uintptr_t)entity, invalid[i]), candidate = before;
            memcpy(stack, initial_stack, sizeof(stack));
            membership_candidate(&candidate);
            assert(candidate.rax == 0 && candidate.rsp == before.rsp + 8);
            assert(candidate.rbx == before.rbx && candidate.rbp == before.rbp && candidate.rsi == before.rsi);
            assert(candidate.rdi == before.rdi && candidate.r12 == before.r12 && candidate.r13 == before.r13);
            assert(candidate.r14 == before.r14 && candidate.r15 == before.r15);
            ++checks;
        }
    }

    wr32(GVA(0x14296FAE0), 0);
    wr32((uintptr_t)entity + 0x50, 65536);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct sigaction handler = {.sa_sigaction = expected_fault, .sa_flags = SA_SIGINFO};
        sigemptyset(&handler.sa_mask);
        assert(!sigaction(SIGSEGV, &handler, NULL));
        CPU cpu = initial_cpu(system, (uintptr_t)entity, 65536);
        memcpy(stack, initial_stack, sizeof(stack));
        membership_baseline(&cpu);
        _exit(103);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 101);
    assert(!munmap(arena, page * 8));
    free(image);
    printf("{\"success\":true,\"checks\":%u,\"valid_full_cpu_and_stack\":262144,"
           "\"invalid_ids\":14,\"baseline_guard_page_fault\":true,\"page_bytes\":%zu}\n", checks, page);
    return 0;
}
