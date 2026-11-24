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
/* ETC2 of the converted archive: "ETC2" = RGB8 (8 bytes per 4x4 block), "ETCA" = RGBA8 with EAC
 * alpha (16 bytes).  Read by the native Vulkan device (android/vulkan/lib/formats.cpp) only. */
#define FP_ETC2_RGB_D3DFMT UINT32_C(0x32435445)
#define FP_ETC2_RGBA_D3DFMT UINT32_C(0x41435445)
#define FP_ETC2_RGB_DXGI_TYPELESS UINT32_C(0x7f00e200)
#define FP_ETC2_RGB_DXGI_UNORM UINT32_C(0x7f00e201)
#define FP_ETC2_RGB_DXGI_SRGB UINT32_C(0x7f00e202)
#define FP_ETC2_RGBA_DXGI_TYPELESS UINT32_C(0x7f00ea00)
#define FP_ETC2_RGBA_DXGI_UNORM UINT32_C(0x7f00ea01)
#define FP_ETC2_RGBA_DXGI_SRGB UINT32_C(0x7f00ea02)

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
