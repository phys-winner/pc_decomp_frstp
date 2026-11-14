#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Explicitly unavailable symbol-handler capability. No fake symbol session.
 * The common import router alone pops the guest return address. */
int win64_dbghelp_call(CPU *cpu,const char *name);
void win64_dbghelp_self_test(void);
#ifdef __cplusplus
}
#endif
