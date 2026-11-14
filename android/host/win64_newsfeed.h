#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit local-mode policy. Diagnostic/online entry must not enable this. */
void win64_newsfeed_enable_offline(void);
int win64_newsfeed_call(CPU *cpu,const char *name);
void win64_newsfeed_self_test(void);
#ifdef __cplusplus
}
#endif
