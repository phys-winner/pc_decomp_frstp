/* DXGI -> Vulkan formats of the native rendering device. */
#pragma once
#include <cstdint>
#include "vk.h"

namespace rdvk {
struct Format {
    VkFormat image = VK_FORMAT_UNDEFINED;  /* format of a VkImage created with this DXGI format */
    VkFormat view = VK_FORMAT_UNDEFINED;   /* format of views (typeless -> representative) */
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t block = 1;                    /* block width/height in texels */
    uint32_t bytes = 0;                    /* bytes per block (or per texel) */
    bool typeless = false, depth = false, stencil = false, srgb = false, compressed = false;
};
/* FP_ASTC*_DXGI_* of android/host/texture_formats.h */
constexpr uint32_t FP_ASTC4_TYPELESS = 0x7f004400u, FP_ASTC4_UNORM = 0x7f004401u, FP_ASTC4_SRGB = 0x7f004402u;
constexpr uint32_t FP_ASTC6_TYPELESS = 0x7f006600u, FP_ASTC6_UNORM = 0x7f006601u, FP_ASTC6_SRGB = 0x7f006602u;
/* FP_ETC2_*_DXGI_*: RGB8 (8 bytes per 4x4 block) and RGBA8 with EAC alpha (16 bytes). */
constexpr uint32_t FP_ETC2_RGB_TYPELESS = 0x7f00e200u, FP_ETC2_RGB_UNORM = 0x7f00e201u, FP_ETC2_RGB_SRGB = 0x7f00e202u;
constexpr uint32_t FP_ETC2_RGBA_TYPELESS = 0x7f00ea00u, FP_ETC2_RGBA_UNORM = 0x7f00ea01u, FP_ETC2_RGBA_SRGB = 0x7f00ea02u;

const Format& format(uint32_t dxgi);
/* After device creation: substitutes formats the driver lacks (D24S8 -> D32S8). */
void formats_fallbacks();
/* The texture formats of the game data on this driver (sampled with linear filtering, optimal
 * tiling), one RDVK_TEXTURE_FORMATS log line: BC1-3 originals and the converted ASTC and ETC2. */
void formats_report();
/* "BC3_SRGB" for the formats of the game data and of the converted archive; nullptr otherwise. */
const char* data_format_name(VkFormat format);
/* Whether the driver samples this format with linear filtering in optimal tiling. */
bool format_sampled(VkFormat format);
/* A compressed texture of the game data is created: the first one logs the driver's list, the first
 * of each format logs RDVK_TEXTURE_FORMAT, and a format the driver cannot sample names the remedy. */
void data_format_use(VkFormat image, uint32_t width, uint32_t height, uint32_t mips, uint32_t layers);
/* The formats views of a colour image of `dxgi` (created as `image`) may use, for
 * VkImageFormatListCreateInfo.  0 or 1: the image needs no mutable format. */
uint32_t view_format_list(uint32_t dxgi, VkFormat image, VkFormat out[8]);
/* Format of a view: a depth-typeless resource read as a texture maps to its depth aspect. */
VkFormat view_format(uint32_t dxgi, VkImageAspectFlags* aspect = nullptr);
/* Row pitch / slice size of a mip for uploads and maps. */
inline uint32_t row_bytes(const Format& f, uint32_t width) { return (width + f.block - 1) / f.block * f.bytes; }
inline uint32_t row_count(const Format& f, uint32_t height) { return (height + f.block - 1) / f.block; }
}  // namespace rdvk
