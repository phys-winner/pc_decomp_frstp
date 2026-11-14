#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
int win64_path_call(CPU *c,const char *name);
void win64_path_self_test(void);
#ifdef __cplusplus
}
#endif
