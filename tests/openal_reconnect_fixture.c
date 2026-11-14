/* Output reconnection of the OpenAL bridge (android/host/win64_openal.c) against a stand-in
 * OpenAL library whose connection state the test controls (tests/openal_reconnect_fake.c):
 * sources are held on a disconnect, a disconnected device is reopened on the default output with
 * the attributes of its context, a failed reopen is repeated no more than once a second and
 * leaves no error behind for the guest, and nothing is reopened while the application is paused.
 *
 *     openal_reconnect_fixture FAKE_LIBRARY PROFILE_DIRECTORY
 */
#include "win64_openal.h"
#include "win64.h"
#include "openal_original_entries.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { FAKE_CONNECTED, FAKE_REOPEN_ALLOWED, FAKE_EXTENSION, FAKE_REOPENS, FAKE_DISABLES, FAKE_OTHER_DISABLES,
       FAKE_QUERIES, FAKE_ERROR_READS, FAKE_ATTRIBUTE_COUNT, FAKE_NAMED_REOPENS, FAKE_PAUSES, FAKE_RESUMES };
typedef struct { const char *name; uint64_t va; } route;
static const route routes[] = {
#define ROW(name, type, va) {#name, va},
FROSTPUNK_OPENAL_ROUTES(ROW)
#undef ROW
};
static unsigned checks;
static void check(int condition, const char *what) {
    ++checks;
    if (!condition) { printf("{\"success\":false,\"failed\":\"%s\",\"checks\":%u}\n", what, checks); fflush(stdout); _Exit(1); }
}

/* Host services of the bridge: the fixture's own memory is the guest memory. */
void win64_region_add(uint64_t address, size_t bytes, int writable) { (void)address; (void)bytes; (void)writable; }
void win64_region_remove(uint64_t address) { (void)address; }
void *win64_pointer(uint64_t address, size_t bytes, int writable) { (void)bytes; (void)writable; return (void *)(uintptr_t)address; }
const char *win64_string_a(uint64_t address) { return (const char *)(uintptr_t)address; }
uint64_t win64_argument(CPU *c, unsigned index) {
    uint64_t registers[] = {c->rcx, c->rdx, c->r8, c->r9};
    check(index < 4, "register arguments only");
    return registers[index];
}

static uint64_t call(const char *name, uint64_t a, uint64_t b) {
    static uint64_t stack[16];
    CPU c;
    memset(&c, 0, sizeof(c));
    c.rcx = a; c.rdx = b; c.rsp = (uintptr_t)stack;
    for (unsigned i = 0; i < sizeof(routes) / sizeof(*routes); ++i)
        if (!strcmp(routes[i].name, name)) { check(win64_openal_dispatch(&c, routes[i].va), name); return c.rax; }
    check(0, "typed route name");
    return 0;
}
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)(t.tv_nsec / 1000000); }
static void delay(unsigned ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; while (nanosleep(&t, &t)); }

int main(int argc, char **argv) {
    check(argc == 3, "arguments");
    check(win64_openal_configure(argv[1], argv[2]), "bridge accepts the stand-in library");
    void *handle = dlopen(argv[1], RTLD_NOW);
    check(handle != NULL, "stand-in library handle");
    void (*fake_set)(int, int) = (void (*)(int, int))dlsym(handle, "fake_set");
    int (*fake_get)(int) = (int (*)(int))dlsym(handle, "fake_get");
    const ALCint *(*fake_attributes)(void) = (const ALCint *(*)(void))dlsym(handle, "fake_attributes");
    check(fake_set && fake_get && fake_attributes, "stand-in controls");

    uint64_t device = call("alcOpenDevice", 0, 0);
    check(device != 0, "device");
    static const ALCint wanted[] = {ALC_FREQUENCY, 48000, ALC_MONO_SOURCES, 64, 0};
    uint64_t context = call("alcCreateContext", device, (uintptr_t)wanted);
    check(context != 0, "context with attributes");
    check(call("alcMakeContextCurrent", context, 0) == 1 && call("alcMakeContextCurrent", context, 0) == 1, "context current twice");
    check(fake_get(FAKE_DISABLES) == 1 && !fake_get(FAKE_OTHER_DISABLES), "sources held once per context, nothing else disabled");

    /* Connected output: polled, never reopened, and no more often than five times a second. */
    uint64_t started = now_ms();
    for (unsigned i = 0; i < 60; ++i) { call("alGetError", 0, 0); delay(10); }
    int allowed = (int)((now_ms() - started) / 200) + 2;
    check(fake_get(FAKE_REOPENS) == 0, "connected output is not reopened");
    check(fake_get(FAKE_QUERIES) >= 2 && fake_get(FAKE_QUERIES) <= allowed, "connection polled at a bounded rate");

    /* Disconnect: one reopen on the default output with the context's attributes. */
    fake_set(FAKE_CONNECTED, 0);
    delay(250);
    call("alGetError", 0, 0);
    check(fake_get(FAKE_REOPENS) == 1 && !fake_get(FAKE_NAMED_REOPENS), "disconnected output reopened on the default output");
    check(fake_get(FAKE_ATTRIBUTE_COUNT) == 5 && !memcmp(fake_attributes(), wanted, sizeof(wanted)), "reopened with the context attributes");
    check(fake_get(FAKE_CONNECTED) == 1, "stand-in reconnected");
    for (unsigned i = 0; i < 30; ++i) { call("alGetError", 0, 0); delay(10); }
    check(fake_get(FAKE_REOPENS) == 1, "no reopen once connected again");

    /* A reopen that fails: its error is not left for the guest, and it is repeated once a second. */
    delay(1000);
    fake_set(FAKE_REOPEN_ALLOWED, 0);
    fake_set(FAKE_CONNECTED, 0);
    delay(250);
    int reads = fake_get(FAKE_ERROR_READS);
    call("alGetError", 0, 0);
    check(fake_get(FAKE_REOPENS) == 2, "failed attempt made");
    check(fake_get(FAKE_ERROR_READS) == reads + 1 && call("alcGetError", device, 0) == ALC_NO_ERROR, "the attempt's error is not visible to the guest");
    for (unsigned i = 0; i < 6; ++i) { delay(100); call("alGetError", 0, 0); }
    check(fake_get(FAKE_REOPENS) == 2, "failed reopen not repeated within a second");
    delay(600);
    call("alGetError", 0, 0);
    check(fake_get(FAKE_REOPENS) == 3, "failed reopen repeated after a second");
    fake_set(FAKE_REOPEN_ALLOWED, 1);

    /* Paused application: nothing is reopened until it resumes. */
    win64_openal_pause(1);
    delay(1100);
    call("alGetError", 0, 0);
    check(fake_get(FAKE_REOPENS) == 3 && fake_get(FAKE_PAUSES) == 1, "no reopen while paused");
    win64_openal_pause(0);
    delay(250);
    call("alGetError", 0, 0);
    check(fake_get(FAKE_REOPENS) == 4 && fake_get(FAKE_CONNECTED) == 1 && fake_get(FAKE_RESUMES) == 1, "reopened after resume");

    /* A context without an attribute list reopens without one; a library without the hold
     * extension is left alone. */
    fake_set(FAKE_EXTENSION, 0);
    uint64_t plain = call("alcCreateContext", device, 0);
    check(plain != 0 && call("alcMakeContextCurrent", plain, 0) == 1, "context without attributes");
    check(fake_get(FAKE_DISABLES) == 1, "no hold request without the extension");
    delay(1000);
    fake_set(FAKE_CONNECTED, 0);
    delay(250);
    call("alGetError", 0, 0);
    check(fake_get(FAKE_REOPENS) == 5 && fake_get(FAKE_ATTRIBUTE_COUNT) == 0, "reopened without attributes");

    win64_openal_shutdown();
    printf("{\"success\":true,\"checks\":%u,\"reopens\":%d,\"connection_queries\":%d,\"hold_requests\":%d}\n",
           checks, fake_get(FAKE_REOPENS), fake_get(FAKE_QUERIES), fake_get(FAKE_DISABLES));
    return 0;
}
