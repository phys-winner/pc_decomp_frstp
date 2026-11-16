#pragma once
#include <stdint.h>
uint32_t fp_switch_texture_skip(const char *path,const uint32_t header[4],uint32_t original);
void fp_switch_gfx_config(uint64_t config);
void fp_graphics_first_run_defaults(void);
void fp_graphics_budget_note(uint64_t self, uint64_t target, int after, uint64_t result);
void fp_graphics_budget_tick(void);
void fp_fx_apply(void);
