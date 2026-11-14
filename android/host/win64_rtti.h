#pragma once
#include "cpu64.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Main-image relative RTTI only; initialize before guest threads start.
 * Admitted profile: public, nonvirtual single inheritance, zero PMDs,
 * COL revision1/offset0/cdOffset0 and pointer casts (isReference==0).
 * Unsupported multiple/virtual/construction/reference forms fail closed.
 * __std_type_info_name additionally supports the original ShapeRenderNode
 * diagnostic-call closure with native-oracle names and Win64 cache/SLIST ABI. */
int win64_rtti_init(const void *mapped_image,size_t image_bytes);
int win64_rtti_call(CPU *cpu,const char *name);
#ifdef __cplusplus
}
#endif
