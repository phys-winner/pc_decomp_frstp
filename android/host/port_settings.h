#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Android-only rows in the original General and Graphics menus (SettingsConfig ids 21-33).
 * The translated settings getter/setter in recomp_funcs_0959 calls into these; values live
 * in <private>/port-settings.txt, independent of the game's own config.bin.
 * Vulkan driver selection belongs to the shared launcher. */
enum { FP_SETTING_OUTPUT_SCALE = 21, FP_SETTING_MOBILE_EFFECTS = 22,
       FP_SETTING_OBJECT_HIGHLIGHT = 23, FP_SETTING_UI_SCALE = 24, FP_SETTING_DEBUG_MENU = 25,
       FP_SETTING_TOUCH_OFFSET = 26, FP_SETTING_UPSCALER = 27, FP_SETTING_FRAME_LIMIT = 28,
       FP_SETTING_GESTURE_HELP = 29, FP_SETTING_SWITCH_UI = 30, FP_SETTING_MODEL_DETAIL = 31,
       FP_SETTING_BUILDING_MODELS = 32, FP_SETTING_FREE_BUILDING = 33 };
/* Native landscape size, before output scale. Option labels are "WIDTHxHEIGHT (PERCENT%)". */
void fp_output_display_size(uint32_t width, uint32_t height);
int fp_output_resolution_label(uint32_t percent, char *text, size_t text_n);
/* 0 = automatic (mobile post-processing), 1 = full meshes, then coarser steps. The bridge
 * rewrites index buffers on the next presents; a missing bridge keeps the value only. */
void frostpunk_geometry_detail(int level);

void fp_port_settings_load(const char *directory);
/* Always returns a value that is one of the row's options. */
uint32_t fp_port_setting_get(uint32_t id);
void fp_port_setting_set(uint32_t id, uint32_t value);
/* UI recipes are cached by the engine; the selected archive is latched at startup. */
int frostpunk_switch_ui(void);
/* Row 33 mirrors the profile's own claim of the free Monument to the Builders of the Generator
 * (the main menu's "Follow Publisher Page" button).  Until the player decides, the first start
 * claims it once; afterwards the stored choice wins. */
int fp_port_free_building_pending(void);
/* Launcher graphics groups (android/port.json options, PORT_OPTION_<KEY> = pc | switch).  With
 * "Mobile post-processing" on, each group uses either the reduced Switch-like profile or the PC
 * settings: 1 = PC for that group.  Defaults: snow PC, everything else Switch-like. */
enum { FP_GFX_SNOW, FP_GFX_BUILDINGS, FP_GFX_TEXTURES, FP_GFX_SHADOWS, FP_GFX_EFFECTS, FP_GFX_COUNT };
int frostpunk_mobile_graphics_pc(unsigned group);
/* Mobile post-processing on and the group not switched to PC. */
int frostpunk_mobile_graphics_reduced(unsigned group);
void fp_port_frame_rates_configure(const uint32_t *rates,uint32_t count);
enum { FP_MAX_FRAME_LIMITS = 64 };
uint32_t fp_port_frame_rates(uint32_t rates[FP_MAX_FRAME_LIMITS]);
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
