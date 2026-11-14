#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "cpu64.h"
/* The caller owns a configured native local-services DSO for process lifetime.
 * No Steam account/session authentication is represented by this backend. */
int win64_steam_configure(void *native_dso);
int win64_steam_import(CPU *cpu,const char *dll,const char *name);
int win64_steam_dispatch(CPU *cpu,uint64_t target);
#ifdef __cplusplus
}
#endif
