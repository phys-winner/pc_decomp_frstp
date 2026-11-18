/* The OpenAL Soft that Frostpunk.exe links statically (0x141BABD20-0x141C02DD0) is not part of
 * the runtime: every public AL/ALC entry goes to the native OpenAL through win64_openal.c (30
 * routes, the other entries stop), so the static library's mixer, backends and context code can
 * never run.  tools/dead_code.py drops its lifted bodies from the dispatch table and the link.
 *
 * Three of its functions are still called by the image itself, outside any AL entry, and only
 * touch OpenAL Soft's own state:
 *   0x141BB0D70  alc_constructor (a .CRT$XCU initializer): atexit(alc_destructor), LogFile =
 *                stderr, two __ALSOFT_* environment switches, the thread-context TSS key, the
 *                list lock, ThunkInit;
 *   0x141BD2780  TLS callback: DLL_THREAD_DETACH runs the destructors of OpenAL Soft's TSS keys,
 *                DLL_PROCESS_DETACH frees them;
 *   0x141BD2760  TLS callback: DLL_PROCESS_DETACH frees the thunk table.
 * With no OpenAL Soft context ever created, their only effect is on state nothing reads, so
 * they return at once here. */
#include "cpu64.h"

static void guest_return(CPU *c) {
    c->rax = 0;
    c->rip = pop64(c);
}

void fp_openal_static_constructor(CPU *c) { guest_return(c); }
void fp_openal_static_tls_callback(CPU *c) { guest_return(c); }
