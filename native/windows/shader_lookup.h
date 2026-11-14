#pragma once
#include "cpu64.h"
#include "loader.h"
void frostpunk_shader_lookup_1410596a9(CPU *c);
void frostpunk_shader_lookup_1410597e9(CPU *c);
int fp_shader_lookup_init(const fp_image *image);
void (*fp_shader_lookup(uint64_t original))(CPU *);
