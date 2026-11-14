#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* After kernel roots are configured, before guest entry: provision this
 * Android guest's redirected AppData/Pictures directories under its real
 * private profile anchor. No Windows host user directories are exposed. */
int win64_shell_provision(void);
/* SHELL32: known-folder HRESULT; ExtractIconExW explicit UINT_MAX/error50.
 * DragAcceptFiles registers guest window state only, with no Android drops.
 * The parent alone pops guest RSP. No icon handles are fabricated. */
int win64_shell_call(CPU *cpu,const char *name);
/* ABI/error checks only; performs no filesystem operation. */
void win64_shell_self_test(void);
#ifdef __cplusplus
}
#endif
