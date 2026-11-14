#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host dispatch owns the return-address pop. Unknown names return0. */
int win64_stdio_call(CPU *cpu,const char *name);
/* Implemented by the CRT host: writes this guest thread's CRT errno. */
void win64_stdio_set_errno(int value);
/* Called after Win64 pointer/allocator initialization. No game/FILE access. */
void win64_stdio_self_test(void);
#ifdef __cplusplus
}
#endif
