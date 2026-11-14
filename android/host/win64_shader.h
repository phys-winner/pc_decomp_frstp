#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
#define WIN64_SHADER_SENTINEL_BASE UINT64_C(0xe5ea0000)
#define WIN64_SHADER_SENTINEL_COUNT 22u
int win64_shader_call(CPU *cpu,const char *name);
int win64_shader_import(CPU *cpu,const char *dll,const char *name);
int win64_shader_dispatch(CPU *cpu,uint64_t target);
void win64_shader_self_test(void);
#ifdef __cplusplus
}
#endif
