#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
int win64_scan_call(CPU *cpu,const char *name);
void win64_scan_self_test(void);
#ifdef __cplusplus
}
#endif
