/* Native Vulkan rendering device for the Liquid Engine (Frostpunk): ABI between the runtime
 * (libfrostpunk_runtime.so, android/vulkan/rdvk_runtime.c) and the backend library
 * (libfrostpunk_rdvk.so, android/vulkan/lib).
 *
 * The engine talks to the GPU only through its RenderingDeviceDirectX11 backend
 * (Frostpunk.exe 0x140D85F60-0x140D9C000).  Every backend function that touches D3D11 is a
 * hook: its lifted body stays in the runtime under fp_d3d11_L_<VA> and the symbol L_<VA> is a
 * stub that calls the library's implementation when the Vulkan device is active.  The table
 * index of a hook is its line in android/vulkan/rdvk_hooks.txt.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "cpu64.h"

#define RDVK_ABI 3
#define RDVK_HOOK_MAX 256
/* Guest COM proxies of the library (D3D11-shaped objects the engine stores in its RD objects):
 * vtable slots are sentinel addresses RDVK_SENTINEL + (kind * 256 + slot) * 8, served by
 * frostpunk_rdvk_dispatch through win64_dispatch_dynamic. */
#define RDVK_SENTINEL UINT64_C(0xE6100000)
#define RDVK_SENTINEL_BYTES UINT64_C(0x100000)

typedef void (*rdvk_fn)(CPU *);

typedef struct rdvk_host {
    uint32_t abi, size;
    /* Original lifted bodies, indexed like the hook table (for diagnostics and pass-through). */
    rdvk_fn original[RDVK_HOOK_MAX];
    uint64_t hook_va[RDVK_HOOK_MAX];
    uint32_t hook_count;
    /* Services of the runtime. */
    void (*log)(const char *line);
    void (*render_log)(const char *line);
    void *(*vk_get_instance_proc_addr)(void);           /* PFN_vkGetInstanceProcAddr */
    void *(*window_acquire)(uint64_t hwnd, uint64_t *generation); /* ANativeWindow* */
    void (*window_release)(void *window);
    void *(*window_native)(void);                        /* the activity's current window */
    rdvk_fn (*static_target)(uint64_t va);               /* lifted function of a guest VA */
    void (*dispatch)(CPU *c, uint64_t va);               /* full guest call dispatch */
    uint64_t (*guest_alloc)(size_t bytes, int zero);
    void (*guest_free)(uint64_t address);
    const char *files_dir;
    int (*display_mode)(uint32_t *width, uint32_t *height, uint32_t *refresh);
    void (*frame_pace)(void);
    void (*present_result)(int32_t result);
    void (*present_accepted)(void);
    /* Port graphics policy (port_settings.c, android/dxvk/mobile_geometry.inc); may be NULL. */
    int (*mobile_effects)(void);                         /* Graphics row "Mobile post-processing" */
    int (*mobile_graphics_pc)(unsigned group);           /* launcher group FP_GFX_* is "PC" */
    int (*geometry_detail)(void);                        /* "Model detail": 0 auto, 1 full, 2..5 coarser */
    int (*object_highlight)(void);                       /* Graphics row "Object highlight" */
} rdvk_host;

typedef struct rdvk_library {
    uint32_t abi, size;
    /* Fills table[i] for every hook it implements; returns 0 to stay on D3D11 (DXVK). */
    int (*init)(const rdvk_host *host, rdvk_fn table[RDVK_HOOK_MAX]);
    /* COM call on a library proxy (target in the sentinel range); returns 1 when handled. */
    int (*com_dispatch)(CPU *c, uint64_t target);
    /* RGBA8 pixels (malloc'ed, rows of width*4 bytes) of a 2D texture of the device, read on the
     * render thread for the game's photo; 0 or an HRESULT. */
    uint32_t (*read_texture)(uint64_t texture, uint32_t *width, uint32_t *height, uint32_t *format, uint8_t **rgba);
} rdvk_library;

#ifdef __cplusplus
extern "C" {
#endif
/* Exported by libfrostpunk_rdvk.so. */
const rdvk_library *frostpunk_rdvk_library(void);
/* Runtime side. */
int frostpunk_rdvk_active(void);
int frostpunk_rdvk_dispatch(CPU *c, uint64_t target);
uint32_t frostpunk_rdvk_read_texture(uint64_t texture, uint32_t *width, uint32_t *height, uint32_t *format, uint8_t **rgba);
#ifdef __cplusplus
}
#endif
