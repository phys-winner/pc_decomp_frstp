/* Win64 Virtual* emulation with 4 KB guest pages on any host page size (16 KB on Pixel 10a).
 *
 * Build against the APK's runtime and run on the device next to all APK libraries:
 *   clang --target=aarch64-linux-android30 -O1 -Inative/runtime64 -Wl,-z,max-page-size=16384
 *     -Wl,--allow-shlib-undefined tests/win64_virtual_pages_test.c libfrostpunk_runtime.so -o vm-pages-test
 *   adb push vm-pages-test lib/arm64-v8a/*.so /data/local/tmp/fp-vm/
 *   adb shell 'cd /data/local/tmp/fp-vm && LD_LIBRARY_PATH=. ./vm-pages-test'
 * Expected: "VM16K_TEST passed failures=0". Runtimes before 2026-10-28 crash with SIGSEGV on 16 KB hosts
 * (VirtualProtect of one 4 KB page made the whole 16 KB host page read-only).
 */
#include "cpu64.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void win64_init(uint64_t image, size_t image_bytes, uint64_t stack, size_t stack_bytes);
int win64_dispatch_import(CPU *c, const char *dll, const char *name);
void frostpunk_teb_write(uint64_t off, uint64_t value, unsigned bytes);

static uint64_t stack[64];
static int failures;

static uint64_t call(const char *name, uint64_t a, uint64_t b, uint64_t d, uint64_t e) {
    CPU c;
    memset(&c, 0, sizeof(c));
    c.rsp = (uint64_t)(uintptr_t)&stack[40]; c.rcx = a; c.rdx = b; c.r8 = d; c.r9 = e;
    win64_dispatch_import(&c, "kernel32.dll", name);
    return c.rax;
}

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    failures += !ok;
}

int main(void) {
    frostpunk_teb_write(0x68, 0, 4);
    win64_init(0x140000000, 0x1000, (uint64_t)(uintptr_t)stack, sizeof(stack));
    printf("host page %ld\n", sysconf(_SC_PAGESIZE));
    uint32_t *old = (uint32_t *)&stack[2];
    uint64_t *info = &stack[8];

    uint64_t vm = call("VirtualAlloc", 0, 32768, 0x3000, 4);
    check(vm && !(vm & 65535), "VirtualAlloc 32 KB RW");
    check(call("VirtualProtect", vm, 4096, 2, (uint64_t)(uintptr_t)old) == 1 && *old == 4, "VirtualProtect first 4 KB read-only");
    volatile unsigned char *bytes = (unsigned char *)(uintptr_t)vm;
    bytes[4096] = 0x5a;
    check(bytes[4096] == 0x5a, "neighbour 4 KB still writable");
    check(call("VirtualQuery", vm + 4096 + 100, (uint64_t)(uintptr_t)info, 48, 0) == 48 && info[0] == vm + 4096,
          "VirtualQuery base rounds to 4 KB");
    check((uint32_t)(info[4] >> 32) == 4, "VirtualQuery neighbour protection RW");

    uint64_t rv = call("VirtualAlloc", 0, 32768, 0x2000, 4);
    check(rv && call("VirtualAlloc", rv + 4096, 4096, 0x1000, 4) == rv + 4096, "commit returns the 4 KB address");
    check(call("VirtualAlloc", rv, 4096, 0x1000, 4) == rv, "commit first 4 KB");
    memset((void *)(uintptr_t)rv, 0x11, 4096);
    memset((void *)(uintptr_t)(rv + 4096), 0xa5, 4096);
    check(call("VirtualFree", rv + 4096, 4096, 0x4000, 0) == 1, "decommit second 4 KB");
    int kept = 1;
    for (int i = 0; i < 4096; ++i) kept &= ((volatile unsigned char *)(uintptr_t)rv)[i] == 0x11;
    check(kept, "neighbour data kept after decommit");
    check(call("VirtualAlloc", rv + 4096, 4096, 0x1000, 4) == rv + 4096, "recommit");
    int zero = 1;
    for (int i = 0; i < 4096; ++i) zero &= ((volatile unsigned char *)(uintptr_t)(rv + 4096))[i] == 0;
    check(zero, "recommit is zero-filled");
    check(call("VirtualFree", vm, 0, 0x8000, 0) == 1 && call("VirtualFree", rv, 0, 0x8000, 0) == 1, "release");
    printf("VM16K_TEST %s failures=%d\n", failures ? "FAIL" : "passed", failures);
    return failures != 0;
}
