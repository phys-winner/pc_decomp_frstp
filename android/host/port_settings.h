#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Android-only rows in the original General and Graphics menus (SettingsConfig ids 21-30).
 * The translated settings getter/setter in recomp_funcs_0959 calls into these; values live
 * in <private>/port-settings.txt, independent of the game's own config.bin.
 * Vulkan driver selection belongs to the shared launcher. */
enum { FP_SETTING_OUTPUT_SCALE = 21, FP_SETTING_MOBILE_EFFECTS = 22,
       FP_SETTING_OBJECT_HIGHLIGHT = 23, FP_SETTING_UI_SCALE = 24, FP_SETTING_DEBUG_MENU = 25,
       FP_SETTING_TOUCH_OFFSET = 26, FP_SETTING_UPSCALER = 27, FP_SETTING_FRAME_LIMIT = 28,
       FP_SETTING_GESTURE_HELP = 29, FP_SETTING_SWITCH_UI = 30 };

void fp_port_settings_load(const char *directory);
/* Always returns a value that is one of the row's options. */
uint32_t fp_port_setting_get(uint32_t id);
void fp_port_setting_set(uint32_t id, uint32_t value);
/* UI recipes are cached by the engine; the selected archive is latched at startup. */
int frostpunk_switch_ui(void);
/* Launcher graphics groups (android/port.json options, PORT_OPTION_<KEY> = pc | switch).  With
 * "Mobile post-processing" on, each group uses either the reduced Switch-like profile or the PC
 * settings: 1 = PC for that group.  Defaults: snow PC, everything else Switch-like. */
enum { FP_GFX_SNOW, FP_GFX_BUILDINGS, FP_GFX_TEXTURES, FP_GFX_SHADOWS, FP_GFX_EFFECTS, FP_GFX_COUNT };
int frostpunk_mobile_graphics_pc(unsigned group);
/* Mobile post-processing on and the group not switched to PC. */
int frostpunk_mobile_graphics_reduced(unsigned group);
void fp_port_frame_rates_configure(const uint32_t *rates,uint32_t count);
uint32_t fp_port_frame_rates(uint32_t rates[32]);
uint32_t frostpunk_android_frame_limit(void);
void frostpunk_android_frame_pace(void);
/* Exported to DXVK (weak import, polled once per frame): 1 = mobile post-processing. */
int frostpunk_dxvk_mobile_effects(void);
int frostpunk_dxvk_object_highlight(void);
int frostpunk_dxvk_upscaler(void);
void frostpunk_dxvk_upscaler_failed(uint32_t error);

#ifdef __cplusplus
}
#endif
