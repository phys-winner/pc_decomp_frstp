#pragma once
#include "cpu64.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { const char *name; const char *value; } win64_environment_entry;
/* Copies explicit guest variables only. No HLE/region/kernel call occurs here.
 * Configure before first getenv/_wgetenv; published values live for process life.
 * Names are printable ASCII without '='; values are validated UTF-8. */
int win64_environment_configure(const win64_environment_entry *entries,size_t count);
int win64_environment_call(CPU *cpu,const char *name);
/* GetEnvironmentVariableA only; handled/no guest RSP pop. */
int win64_environment_windows_call(CPU *cpu,const char *name);
/* GetUserNameA for the fixed private Android guest identity; no RSP pop.
 * The caller must restrict this provider to ADVAPI32.dll. */
int win64_identity_call(CPU *cpu,const char *name);
void win64_environment_self_test(void);
#ifdef __cplusplus
}
#endif
