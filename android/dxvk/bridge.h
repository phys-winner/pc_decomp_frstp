#pragma once
#include "cpu64.h"
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Configure before the first D3D/DXGI import. Window may be NULL for offscreen.
 * The caller retains provider/window for the entire DXVK lifetime. */
void win64_d3d11_configure(PFN_vkGetInstanceProcAddr provider, void *native_window);
/* Optional production HWND resolver. A successful acquire transfers one native
 * window reference; release runs only after the actual native swapchain dies.
 * Callbacks must remain loaded for the process lifetime. Hidden/minimized
 * registered HWNDs are valid. A NULL resolver retains the direct probe mode. */
typedef void *(*win64_d3d11_window_acquire_fn)(uint64_t guest_hwnd,uint64_t *generation);
typedef void (*win64_d3d11_window_release_fn)(void *native_window);
void win64_d3d11_set_window_resolver(win64_d3d11_window_acquire_fn acquire,win64_d3d11_window_release_fn release);
/* Explicit guest virtual desktop; Android surface/input keep physical extents. */
void win64_d3d11_set_display_mode(uint32_t width,uint32_t height,uint32_t refresh);
int frostpunk_dxvk_get_display_mode(uint32_t *width,uint32_t *height,uint32_t *refresh);
int win64_d3d11_import(CPU *cpu, const char *dll, const char *name);
int win64_d3d11_dispatch(CPU *cpu, uint64_t target);
/* Original DirectX screenshot entry: context, texture, unused, UTF16 filename. */
void win64_d3d11_save_photo(CPU *cpu);
#ifdef __cplusplus
}
#endif
