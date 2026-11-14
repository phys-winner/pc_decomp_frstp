#pragma once
#include "cpu64.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Gate the original no-op matherr before admitting its underflow return0. */
int win64_math_init(const void *mapped_pe,size_t image_size);
int win64_math_call(CPU *cpu,const char *name);
/* powf after its cached import step 0 (stdio decline already done): the UCRT-exact handler only. */
int win64_math_fast_powf(CPU *cpu);
void win64_math_self_test(void);
#ifdef __cplusplus
}
#endif
