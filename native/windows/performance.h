#pragma once
#include "cpu64.h"
int fp_performance_init(void);
void fp_performance_native_begin(const CPU *cpu,uint64_t target);
void fp_performance_native_return(const CPU *cpu,uint64_t target);
