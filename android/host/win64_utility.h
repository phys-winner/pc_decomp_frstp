#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* UCRT utility handler. The parent dispatcher alone pops the guest return. */
int win64_utility_call(CPU *cpu,const char *name);
/* The handler of atoi, qsort or bsearch for the import cache (win64.c), else NULL. */
int (*win64_utility_fast_import(const char *name))(CPU *);
/* Preserves this native thread's guest random state. */
void win64_utility_self_test(void);
#ifdef __cplusplus
}
#endif
