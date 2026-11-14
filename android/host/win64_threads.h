#pragma once
#include "win64.h"
typedef struct {
    int (*attach)(CPU *cpu,uint64_t stack_low,size_t stack_bytes);
    void (*detach)(CPU *cpu);
    uint64_t (*call)(CPU *cpu,uint64_t target,uint64_t parameter);
    void *(*teb_current)(void);
} win64_thread_hooks;
/* Configure before any guest TLS API. process_ready is called only after the
 * real process/DLL attachment callbacks have completed. */
void win64_threads_configure(const win64_thread_hooks *hooks,size_t default_stack);
/* Adopt an already attached main guest context; does not declare readiness. */
void win64_threads_adopt_current(CPU *cpu);
void win64_threads_process_ready(void);
int win64_threads_call(CPU *c,const char *name);
/* Direct handler for a hot critical-section import, or NULL. */
void *win64_threads_fast_import(const char *name);
/* Kernel-only MSVC thread-name exception adapter; unrelated codes are unhandled. */
int win64_threads_raise_exception(CPU *cpu);
void win64_threads_self_test(void);
