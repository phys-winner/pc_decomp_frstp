#include "frame_profile.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define NS UINT64_C(1000000000)
#define SUBOPTIMAL 1000001003
static void event(uintptr_t pointer, uint64_t id, uint64_t now, int32_t result) {
    errno = EDOM;
    frostpunk_frame_profile_present((const void *)pointer, id, now, result);
    assert(errno == EDOM);
}
static void retire(uintptr_t pointer, uint64_t now) { event(pointer, 0, now, 1); }

int main(void) {
    /* Exact boundary: the event at t=1 belongs to the second fixed bin. */
    event(0x100, 1, NS, 0);
    event(0x100, 1, NS+1, 0); /* repeated ID */
    event(0x100, 0, NS+2, 0); /* repeated SyncInterval submission */
    event(0x100, 2, NS+3, -4); /* rejected queue result */
    event(0x100, 2, NS+500000000, SUBOPTIMAL);
    event(0x100, 3, 2*NS, 0);
    event(0x100, 4, 4500000000ULL, 0); /* full empty bin + 2.5s gap */
    retire(0x100, 5*NS);
    /* Pointer reuse is a fresh generation, and starts accepting ID one. */
    event(0x100, 1, 10*NS, 0);
    retire(0x100, 10*NS+1);

    /* Independent presenters retain independent clocks/counts. */
    event(0x200, 1, 20*NS, 0); event(0x300, 1, 20*NS, 0);
    event(0x200, 2, 20*NS+500000000, 0);
    event(0x300, 2, 20*NS+250000000, 0);
    retire(0x200, 21*NS); retire(0x300, 21*NS);

    /* Clock and ID regressions fail closed until explicit retirement. */
    event(0x400, 1, 30*NS, 0); event(0x400, 2, 30*NS-1, 0);
    event(0x400, 3, 31*NS, 0); retire(0x400, 32*NS);
    event(0x500, 9, 40*NS, 0); event(0x500, 8, 40*NS+1, 0);
    event(0x500, 10, 41*NS, 0); retire(0x500, 42*NS);
    event(0x600, 1, 50*NS, 0); event(0x600, 2, 0, 0); retire(0x600, 51*NS);
    event(0x700, 1, 60*NS, 0); event(0x700, 2, 5000*NS, 0); retire(0x700, 5000*NS);

    /* Explicit observation flush exposes an idle tail including zero bins. */
    event(0x800, 1, 70*NS, 0);
    errno = ERANGE; fp_frame_profile_flush(73*NS+500000000); assert(errno == ERANGE);
    retire(0x800, 73*NS+500000000);

    /* Bounded records never evict an active unrelated presenter. */
    for (unsigned i=0; i<16; ++i) event(0x1000+i, 1, 80*NS, 0);
    event(0x2000, 1, 80*NS, 0);
    for (unsigned i=0; i<16; ++i) retire(0x1000+i, 80*NS+1);
    event(0x2000, 1, 90*NS, 0); retire(0x2000, 90*NS+1);
    for (unsigned i=0; i<=30; ++i) event(0x3000, i+1, 100*NS+(uint64_t)i*NS/30, 0);
    retire(0x3000, 102*NS);
    for (unsigned i=0; i<=60; ++i) event(0x4000, i+1, 110*NS+(uint64_t)i*NS/60, 0);
    retire(0x4000, 112*NS);
    event(0, 1, NS, 0);
    puts("FRAME_PROFILE_FIXTURE_PASS deterministic_clock=1 errno_preserved=1 bounded_presenters=16");
    return 0;
}
