#pragma once
#include "cpu64.h"
int frostpunk_mobile_submit(unsigned action,float dx,float dy,float zoom,float angle);
unsigned frostpunk_mobile_context(float x,float y);
uint64_t frostpunk_mobile_swipe_target(float x,float y,int direction);
void fp_mobile_camera_before(CPU *cpu);
void fp_mobile_camera_destroy(void);
void fp_mobile_ui_scale(void);
void fp_mobile_ui_before(CPU *cpu);
void fp_mobile_ui_position(CPU *cpu,void (*original)(CPU*));
void fp_mobile_ui_template(CPU *cpu,void (*original)(CPU*));
void fp_mobile_law_camera_init(CPU *cpu,void (*original)(CPU*));
void fp_mobile_law_camera(CPU *cpu,void (*original)(CPU*));
void fp_mobile_painters_created(CPU *cpu,void (*original)(CPU*));
void fp_mobile_panels_created(CPU *cpu,void (*original)(CPU*));
void fp_mobile_debug_menu_request(void);
int frostpunk_dxvk_scene_active(void);
