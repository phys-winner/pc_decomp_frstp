#pragma once
#include "cpu64.h"
#include "loader.h"

/* Initialize before starting guest threads. */
int fp_simd_float_init(void);
int fp_simd_float(CPU *cpu, const fp_image *image, uint64_t original_va,
                  const char *instruction);

enum { FP_CMPPS_EQ, FP_CMPPS_LT, FP_CMPPD_EQ, FP_CMPPD_LT, FP_CMPPD_NLE };
/* Called only by byte-proven Windows source overlays; source may alias CPU. */
int fp_simd_packed_compare(CPU *cpu,unsigned destination,const XMM *source,unsigned operation);
int fp_simd_packed_compare_memory(CPU *cpu,unsigned destination,uint64_t address,unsigned operation);
