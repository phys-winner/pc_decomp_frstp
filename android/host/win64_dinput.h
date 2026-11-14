#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Keyboard-only DirectInput8A adaptation. Both handlers leave guest RSP intact;
 * the common import/dynamic dispatcher removes the single return address. */
#define WIN64_DINPUT_SENTINEL_BASE UINT64_C(0xe7000000)
int win64_dinput_call(CPU *cpu,const char *name);
int win64_dinput_dispatch(CPU *cpu,uint64_t target);
void win64_dinput_self_test(void);
/* Staged class factory only; caller must pass real current COM apartment state. */
int win64_dinput_com_call(CPU *cpu,const char *name,int apartment_initialized);
#ifdef __cplusplus
}
#endif
