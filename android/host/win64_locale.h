#pragma once
#include "cpu64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* This module owns CRT locale/thread mode. Dispatch owns the return-address pop. */
int win64_locale_call(CPU *cpu,const char *name);
/* Called from win64_thread_crt_attach before guest code on every new thread. */
void win64_locale_thread_attach(void);
/* Called after the host's Win64 pointer registry and allocator are initialized. */
void win64_locale_self_test(void);
#ifdef __cplusplus
}
#endif
