#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
int win64_error_message_call(CPU *cpu,const char *name);
void win64_error_message_self_test(void);
#ifdef __cplusplus
}
#endif
