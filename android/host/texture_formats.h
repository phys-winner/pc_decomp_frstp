#pragma once
#include <stddef.h>
#include "cpu64.h"

/* Private resource ABI shared explicitly with tools/dxvk_astc_overlay.py. */
#define FP_ASTC4_D3DFMT UINT32_C(0x34545341)
#define FP_ASTC6_D3DFMT UINT32_C(0x36545341)
#define FP_ASTC4_DXGI_TYPELESS UINT32_C(0x7f004400)
#define FP_ASTC4_DXGI_UNORM UINT32_C(0x7f004401)
#define FP_ASTC4_DXGI_SRGB UINT32_C(0x7f004402)
#define FP_ASTC6_DXGI_TYPELESS UINT32_C(0x7f006600)
#define FP_ASTC6_DXGI_UNORM UINT32_C(0x7f006601)
#define FP_ASTC6_DXGI_SRGB UINT32_C(0x7f006602)

/* Initialize from the checked mapped PE before starting guest threads.
 * Overrides only the two exact entry targets; old formats call the unchanged
 * canonical static function. Each wrapper owns the original function's RET. */
#ifdef __cplusplus
extern "C" {
#endif
int fp_android_texture_formats_init(const void *mapped_pe, size_t image_size);
void (*fp_android_texture_formats_lookup(uint64_t original_va))(CPU *);
#ifdef __cplusplus
}
#endif
