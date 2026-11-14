#pragma once
#include <stddef.h>
#include "cpu64.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Verify the unchanged original before exposing this single static entry. */
int fp_android_string_resize_init(const void *mapped_pe,size_t image_size);
void (*fp_android_string_resize_lookup(uint64_t original_va))(CPU *);
#ifdef __cplusplus
}
#endif
