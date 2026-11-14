#pragma once
#include "cpu64.h"

typedef void (*frostpunk_dependency_fn)(CPU *c);
void frostpunk_dependencies_init(CPU *c);
/* Called under the bootstrap loader lock, only after DLL_PROCESS_ATTACH TRUE.
 * Only DLL_THREAD_ATTACH(2)/DLL_THREAD_DETACH(3) are accepted. */
void frostpunk_dependencies_thread_notify(CPU *c,unsigned reason);
frostpunk_dependency_fn frostpunk_dependencies_lookup(uint64_t target);
int frostpunk_dependencies_import(CPU *c, uint64_t target);
uint64_t frostpunk_dependencies_export(const char *name);
