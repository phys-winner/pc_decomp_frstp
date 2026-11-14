#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* OLE32 apartment lifecycle and one scoped DirectInput8W class provider;
 * the common import router alone pops RSP. No general class activation,
 * object marshaling, OLE, or COM message dispatch. */
int win64_com_call(CPU *cpu,const char *name);
void win64_com_self_test(void);
#ifdef __cplusplus
}
#endif
