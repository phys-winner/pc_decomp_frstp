#pragma once
#include <stddef.h>
#include "cpu64.h"

enum { FP_AOT_CMPPS_EQ, FP_AOT_CMPPS_LT, FP_AOT_CMPPD_EQ, FP_AOT_CMPPD_LT, FP_AOT_CMPPD_NLE };
/* Call once after mapping the pinned PE as NX data and before guest threads. */
int fp_aot_cpu_init(const void *mapped_pe,size_t image_size);
/* False means no CPU change: retain the existing unsupported-instruction STOP. */
int fp_aot_cpu_instruction(CPU *c,uint64_t original_va,const char *instruction);
int fp_aot_packed_compare(CPU *c,unsigned destination,const XMM *source,unsigned operation);
int fp_aot_packed_compare_memory(CPU *c,unsigned destination,uint64_t address,unsigned operation);
