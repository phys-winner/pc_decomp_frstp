#pragma once
#include <stddef.h>
#include "cpu64.h"

/* Initialize from the checked mapped PE before starting guest threads. */
int fp_android_extra_aot_init(const void *mapped_pe,size_t image_size);
void (*fp_android_extra_aot_lookup(uint64_t original_va))(CPU *);
