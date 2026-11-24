#include "formats.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "device.h"
#include "rdvk.h"

namespace rdvk {
namespace {
struct Row { uint32_t dxgi; VkFormat image, view; uint32_t block, bytes; uint8_t kind; };
/* kind: 0 color, 1 typeless color, 2 depth, 3 depth+stencil, 4 depth-typeless, 5 srgb, 6 compressed, 7 compressed srgb,
 * 8 compressed typeless, 9 depth-typeless read as depth (view) */
constexpr Row ROWS[] = {
    {1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 1},
    {2, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 0},
    {3, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_UINT, 1, 16, 0},
    {4, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SINT, 1, 16, 0},
    {5, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, 1, 12, 1},
    {6, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, 1, 12, 0},
    {7, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_UINT, 1, 12, 0},
    {8, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_SINT, 1, 12, 0},
    {9, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 1},
    {10, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 0},
    {11, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 0},
    {12, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT, 1, 8, 0},
    {13, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 1, 8, 0},
    {14, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT, 1, 8, 0},
    {15, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, 1, 8, 1},
    {16, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, 1, 8, 0},
    {17, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT, 1, 8, 0},
    {18, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SINT, 1, 8, 0},
    {19, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 8, 4},
    {20, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 8, 3},
    {21, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT, 1, 8, 9},
    {23, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 1},
    {24, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 0},
    {25, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, 1, 4, 0},
    {26, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 1, 4, 0},
    {27, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 1},
    {28, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 0},
    {29, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, 1, 4, 5},
    {30, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT, 1, 4, 0},
    {31, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 1, 4, 0},
    {32, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT, 1, 4, 0},
    {33, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, 1, 4, 1},
    {34, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, 1, 4, 0},
    {35, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 1, 4, 0},
    {36, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_UINT, 1, 4, 0},
    {37, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SNORM, 1, 4, 0},
    {38, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16_SINT, 1, 4, 0},
    {39, VK_FORMAT_D32_SFLOAT, VK_FORMAT_R32_SFLOAT, 1, 4, 4},
    {40, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT, 1, 4, 2},
    {41, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT, 1, 4, 0},
    {42, VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, 1, 4, 0},
    {43, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SINT, 1, 4, 0},
    {44, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 1, 4, 4},
    {45, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 1, 4, 3},
    {46, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 1, 4, 9},
    {48, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 1, 2, 1},
    {49, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 1, 2, 0},
    {50, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_UINT, 1, 2, 0},
    {51, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_SNORM, 1, 2, 0},
    {52, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8_SINT, 1, 2, 0},
    {53, VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UNORM, 1, 2, 4},
    {54, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_SFLOAT, 1, 2, 0},
    {55, VK_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM, 1, 2, 2},
    {56, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, 1, 2, 0},
    {57, VK_FORMAT_R16_UINT, VK_FORMAT_R16_UINT, 1, 2, 0},
    {58, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SNORM, 1, 2, 0},
    {59, VK_FORMAT_R16_SINT, VK_FORMAT_R16_SINT, 1, 2, 0},
    {60, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1, 1, 1},
    {61, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1, 1, 0},
    {62, VK_FORMAT_R8_UINT, VK_FORMAT_R8_UINT, 1, 1, 0},
    {63, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_SNORM, 1, 1, 0},
    {64, VK_FORMAT_R8_SINT, VK_FORMAT_R8_SINT, 1, 1, 0},
    {65, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1, 1, 0},   /* A8: swizzled in the view */
    {67, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 1, 4, 0},
    {70, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 8},
    {71, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 6},
    {72, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 8, 7},
    {73, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 8},
    {74, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 6},
    {75, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, 4, 16, 7},
    {76, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 8},
    {77, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 6},
    {78, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, 4, 16, 7},
    {79, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 8},
    {80, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 6},
    {81, VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, 4, 8, 6},
    {82, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 8},
    {83, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 6},
    {84, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK, 4, 16, 6},
    {85, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, 1, 2, 0},
    {86, VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 1, 2, 0},
    {87, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 1, 4, 0},
    {88, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 1, 4, 0},
    {90, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 1, 4, 1},
    {91, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 1, 4, 5},
    {92, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 1, 4, 1},
    {93, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 1, 4, 5},
    {94, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 16, 8},
    {95, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 16, 6},
    {96, VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK, 4, 16, 6},
    {97, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, 4, 16, 8},
    {98, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, 4, 16, 6},
    {99, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, 4, 16, 7},
    {115, VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, 1, 2, 0},
};
std::array<Format, 128> table;
Format astc[6];
Format etc2[6];
bool ready = false;
void build() {
    for (const Row& r : ROWS) {
        Format& f = table[r.dxgi];
        f.image = r.image; f.view = r.view; f.block = r.block; f.bytes = r.bytes;
        f.typeless = r.kind == 1 || r.kind == 4 || r.kind == 8;
        f.depth = r.kind == 2 || r.kind == 3 || r.kind == 4 || r.kind == 9;
        f.stencil = r.image == VK_FORMAT_D24_UNORM_S8_UINT || r.image == VK_FORMAT_D32_SFLOAT_S8_UINT;
        f.srgb = r.kind == 5 || r.kind == 7;
        f.compressed = r.kind >= 6 && r.kind <= 8;
        f.aspect = f.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    }
    const VkFormat a[6] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
                           VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    for (int i = 0; i < 6; ++i) {
        Format& f = astc[i];
        f.image = i < 3 ? VK_FORMAT_ASTC_4x4_UNORM_BLOCK : VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
        f.view = a[i]; f.block = i < 3 ? 4 : 6; f.bytes = 16; f.compressed = true;
        f.typeless = i % 3 == 0; f.srgb = i % 3 == 2;
    }
    const VkFormat e[6] = {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
                           VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK};
    for (int i = 0; i < 6; ++i) {
        Format& f = etc2[i];
        f.image = i < 3 ? VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK : VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        f.view = e[i]; f.block = 4; f.bytes = i < 3 ? 8 : 16; f.compressed = true;
        f.typeless = i % 3 == 0; f.srgb = i % 3 == 2;
    }
    ready = true;
}
struct DataFormat { VkFormat format; const char* name; };
constexpr DataFormat DATA_FORMATS[] = {
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1_SRGB"},
    {VK_FORMAT_BC2_UNORM_BLOCK, "BC2"}, {VK_FORMAT_BC2_SRGB_BLOCK, "BC2_SRGB"},
    {VK_FORMAT_BC3_UNORM_BLOCK, "BC3"}, {VK_FORMAT_BC3_SRGB_BLOCK, "BC3_SRGB"},
    {VK_FORMAT_ASTC_6x6_UNORM_BLOCK, "ASTC_6x6"}, {VK_FORMAT_ASTC_6x6_SRGB_BLOCK, "ASTC_6x6_SRGB"},
    {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, "ASTC_4x4"}, {VK_FORMAT_ASTC_4x4_SRGB_BLOCK, "ASTC_4x4_SRGB"},
    {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "ETC2_RGB"}, {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, "ETC2_RGB_SRGB"},
    {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, "ETC2_RGBA"}, {VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, "ETC2_RGBA_SRGB"},
};
}  // namespace

const char* data_format_name(VkFormat format) {
    for (const DataFormat& d : DATA_FORMATS) if (d.format == format) return d.name;
    return nullptr;
}

bool format_sampled(VkFormat format) {
    /* The feature bits textureCompressionBC/ETC2/ASTC_LDR promise whole families; a driver may still
     * sample single formats without the bit (emulated BC on Mali) or lack one with it, so ask per format. */
    VkFormatProperties p{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &p);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (p.optimalTilingFeatures & need) == need;
}

void data_format_use(VkFormat image /* the view format of the DXGI code */, uint32_t width, uint32_t height, uint32_t mips, uint32_t layers) {
    static std::mutex m;
    static bool reported = false;
    static VkFormat seen[16];
    static unsigned count = 0;
    const char* name = data_format_name(image);
    if (!name) return;
    std::lock_guard<std::mutex> lock(m);
    if (!reported) { reported = true; formats_report(); }
    for (unsigned i = 0; i < count; ++i) if (seen[i] == image) return;
    if (count < 16) seen[count++] = image;
    const bool sampled = format_sampled(image);
    log("RDVK_TEXTURE_FORMAT %s first=%ux%u mips=%u layers=%u sampled=%d source=%s", name, width, height, mips, layers, int(sampled),
        image >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && image <= VK_FORMAT_BC7_SRGB_BLOCK ? "original" : "converted");
    if (!sampled)
        log("RDVK_TEXTURE_UNSUPPORTED %s is not sampled by this Vulkan driver: convert the textures in the launcher "
            "(ASTC or ETC2), or restore the originals for a driver that samples DXT", name);
}

void formats_report() {
    char present[256] = "", missing[256] = "";
    for (const DataFormat& d : DATA_FORMATS) {
        char* list = format_sampled(d.format) ? present : missing;
        size_t used = strlen(list);
        snprintf(list + used, sizeof present - used, "%s%s", used ? "," : "", d.name);
    }
    log("RDVK_TEXTURE_FORMATS sampled=%s missing=%s", present[0] ? present : "-", missing[0] ? missing : "-");
}

const Format& format(uint32_t dxgi) {
    if (!ready) build();
    if (dxgi >= FP_ASTC4_TYPELESS && dxgi <= FP_ASTC4_SRGB) return astc[dxgi - FP_ASTC4_TYPELESS];
    if (dxgi >= FP_ASTC6_TYPELESS && dxgi <= FP_ASTC6_SRGB) return astc[3 + dxgi - FP_ASTC6_TYPELESS];
    if (dxgi >= FP_ETC2_RGB_TYPELESS && dxgi <= FP_ETC2_RGB_SRGB) return etc2[dxgi - FP_ETC2_RGB_TYPELESS];
    if (dxgi >= FP_ETC2_RGBA_TYPELESS && dxgi <= FP_ETC2_RGBA_SRGB) return etc2[3 + dxgi - FP_ETC2_RGBA_TYPELESS];
    if (dxgi < table.size() && table[dxgi].image != VK_FORMAT_UNDEFINED) return table[dxgi];
    stop("unsupported DXGI format %u", dxgi);
}

uint32_t view_format_list(uint32_t dxgi, VkFormat image, VkFormat out[8]) {
    if (!ready) build();
    uint32_t n = 0;
    auto add = [&](VkFormat f) {
        if (f == VK_FORMAT_UNDEFINED || n == 8) return;
        for (uint32_t i = 0; i < n; ++i) if (out[i] == f) return;
        out[n++] = f;
    };
    add(image);
    /* The private ASTC formats: typeless and sRGB images are viewed as UNORM or sRGB. */
    if (dxgi >= FP_ASTC4_TYPELESS && dxgi <= FP_ASTC4_SRGB) {
        if (dxgi != FP_ASTC4_UNORM) { add(VK_FORMAT_ASTC_4x4_UNORM_BLOCK); add(VK_FORMAT_ASTC_4x4_SRGB_BLOCK); }
        return n;
    }
    if (dxgi >= FP_ASTC6_TYPELESS && dxgi <= FP_ASTC6_SRGB) {
        if (dxgi != FP_ASTC6_UNORM) { add(VK_FORMAT_ASTC_6x6_UNORM_BLOCK); add(VK_FORMAT_ASTC_6x6_SRGB_BLOCK); }
        return n;
    }
    if (dxgi >= FP_ETC2_RGB_TYPELESS && dxgi <= FP_ETC2_RGB_SRGB) {
        if (dxgi != FP_ETC2_RGB_UNORM) { add(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK); add(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK); }
        return n;
    }
    if (dxgi >= FP_ETC2_RGBA_TYPELESS && dxgi <= FP_ETC2_RGBA_SRGB) {
        if (dxgi != FP_ETC2_RGBA_UNORM) { add(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK); add(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK); }
        return n;
    }
    if (dxgi >= table.size()) return n;
    const Format& f = table[dxgi];
    /* A typeless format: every member of its DXGI family (as DXVK lists them). */
    static const struct { uint8_t typeless, first, last; } families[] = {
        {1, 2, 4}, {5, 6, 8}, {9, 10, 14}, {15, 16, 18}, {23, 24, 25}, {27, 28, 32}, {33, 34, 38}, {39, 41, 43},
        {48, 49, 52}, {53, 54, 59}, {60, 61, 64}, {70, 71, 72}, {73, 74, 75}, {76, 77, 78}, {79, 80, 81}, {82, 83, 84},
        {94, 95, 96}, {97, 98, 99}};
    if (f.typeless) {
        for (const auto& fam : families) {
            if (fam.typeless != dxgi) continue;
            for (uint32_t m = fam.first; m <= fam.last; ++m)
                if (!table[m].depth && table[m].view != VK_FORMAT_UNDEFINED) add(table[m].view);
        }
        if (dxgi == 90) { add(VK_FORMAT_B8G8R8A8_UNORM); add(VK_FORMAT_B8G8R8A8_SRGB); }
        if (dxgi == 92) { add(VK_FORMAT_B8G8R8A8_UNORM); add(VK_FORMAT_B8G8R8A8_SRGB); }
        return n;
    }
    /* sRGB formats keep a UNORM image (copies) and an sRGB view. */
    if (f.srgb || f.view != f.image) add(f.view);
    return n;
}

void formats_fallbacks() {
    if (!ready) build();
    /* D24S8 is optional in Vulkan (at least one of D24S8/D32S8 exists); DXVK substitutes D32S8. */
    VkFormatProperties p{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu.physical, VK_FORMAT_D24_UNORM_S8_UINT, &p);
    if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        unsigned n = 0;
        for (Format& f : table) {
            if (f.image == VK_FORMAT_D24_UNORM_S8_UINT) { f.image = VK_FORMAT_D32_SFLOAT_S8_UINT; ++n; }
            if (f.view == VK_FORMAT_D24_UNORM_S8_UINT) f.view = VK_FORMAT_D32_SFLOAT_S8_UINT;
        }
        log("RDVK_FALLBACK D24_UNORM_S8_UINT is no depth attachment on this driver: %u DXGI formats use D32_SFLOAT_S8_UINT", n);
    }
}

VkFormat view_format(uint32_t dxgi, VkImageAspectFlags* aspect) {
    const Format& f = format(dxgi);
    if (aspect) {
        *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (f.depth) *aspect = (dxgi == 47 || dxgi == 22) ? VK_IMAGE_ASPECT_STENCIL_BIT
                              : (dxgi == 21 || dxgi == 46 || dxgi == 39 || dxgi == 53) ? VK_IMAGE_ASPECT_DEPTH_BIT : f.aspect;
    }
    /* A sampled view of a depth format reads the depth aspect of the image format. */
    if (dxgi == 39) return VK_FORMAT_D32_SFLOAT;
    if (dxgi == 53) return VK_FORMAT_D16_UNORM;
    return f.view;
}
}  // namespace rdvk
