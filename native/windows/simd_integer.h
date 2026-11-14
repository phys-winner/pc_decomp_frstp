#pragma once
#include "cpu64.h"
#include "loader.h"

/* Exact original sites only. Returns zero without changing CPU on a mismatch. */
int fp_simd_integer(CPU *cpu, const fp_image *image, uint64_t original_va,
                    const char *instruction);
