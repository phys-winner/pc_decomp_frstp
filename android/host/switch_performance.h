#pragma once
#include <stdint.h>
uint32_t fp_switch_texture_skip(const char *path,const uint32_t header[4],uint32_t original);
void fp_switch_gfx_config(uint64_t config);
void fp_graphics_first_run_defaults(void);
