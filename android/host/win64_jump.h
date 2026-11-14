#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Initial Win64 context capture only; caller performs the common RSP pop. */
int win64_jump_call(CPU *cpu,const char *name);
void win64_jump_set_round(unsigned x86_rounding);
void win64_jump_self_test(void);
#ifdef __cplusplus
}
#endif
