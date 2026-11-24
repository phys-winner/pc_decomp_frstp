#pragma once
#include "cpu64.h"
int win64_string_call(CPU *cpu,const char *name);
/* Direct handler of a hot string import for the import cache (win64.c), else NULL. */
int (*win64_string_fast_import(const char *name))(CPU *);
void win64_string_self_test(void);
