#pragma once
#include "cpu64.h"

void fp_windows_input_init(uint64_t guest_image_base);
int fp_windows_input_call(CPU *cpu, uint64_t target, const char *import_name);
