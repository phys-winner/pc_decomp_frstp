#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* KERNEL32/KERNELBASE handler; its caller alone pops the guest return address. */
int win64_system_call(CPU *cpu,const char *name);
/* Validates actual measurements and Win64 output bounds after win64_init. */
void win64_system_self_test(void);
#ifdef __cplusplus
}
#endif
