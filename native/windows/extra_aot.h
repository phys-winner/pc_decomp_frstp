#pragma once
#include "cpu64.h"
#include "loader.h"

/* Exact static entries absent from the sealed main/supplemental catalogs. */
int fp_extra_aot_init(const fp_image *image);
void (*fp_extra_aot_lookup(uint64_t original))(CPU *);
