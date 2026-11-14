#pragma once
#include "win64.h"
int win64_sync_call(CPU *cpu,const char *name);
void win64_sync_self_test(void);
/* Fail closed before unwinding a live synchronous INIT_ONCE callback. */
void win64_sync_thread_exit(void);
