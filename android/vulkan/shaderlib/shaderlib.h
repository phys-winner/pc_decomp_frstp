/* Layout of the build-time shader library (tools/build_shaderlib.py, rdvk_shaderc.cpp), linked
 * into libfrostpunk_rdvk.so as read-only data.  Little endian; every offset is from the start of
 * the library and 4-byte aligned, so SPIR-V words are used in place.
 *
 *   Header | Shader[shader_count] sorted by (hash, dxbc_size) | ICB data, VariantRecord[], SPIR-V,
 *   Binding[] ...
 *
 * A shader is found by the FNV-1a 64 of its whole DXBC container and its size, and confirmed
 * with the container's own 16-byte checksum.  Each shader has one variant per range of
 * VkPhysicalDeviceLimits::minStorageBufferOffsetAlignment that changes the SPIR-V (raw and
 * structured buffers become SSBOs only when the device's alignment allows it).  rdvk_shaderc writes
 * the SPIR-V raw; tools/build_shaderlib.py packs every variant as its own zlib stream for the APK,
 * so the device inflates only the shaders the game creates. */
#pragma once
#include <cstddef>
#include <cstdint>

namespace rdvk {
namespace shaderlib {

constexpr char kMagic[8] = {'R', 'D', 'V', 'K', 'S', 'H', 'L', '1'};
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMinAlignment = 4, kMaxAlignment = 256;

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t shader_count;
    uint32_t shaders;          /* offset of Shader[shader_count] */
    uint32_t total_size;
    uint32_t reserved[2];
};
static_assert(sizeof(Header) == 32, "header layout");

struct Shader {
    uint64_t hash;             /* FNV-1a 64 of the DXBC container */
    uint32_t dxbc_size;
    uint8_t checksum[16];      /* bytes 4..19 of the container */
    uint32_t stage;            /* VkShaderStageFlagBits */
    uint32_t input_mask, output_mask, patch_vertices;
    uint8_t role;              /* mobile pass policy role (objects.h ROLE_*) */
    uint8_t variant_count;
    uint16_t reserved;
    uint32_t uniform, uniform_size;   /* immediate constant buffer */
    uint32_t variants;         /* offset of VariantRecord[variant_count] */
    uint32_t pad;
};
static_assert(sizeof(Shader) == 64, "shader layout");

struct VariantRecord {
    uint32_t min_alignment, max_alignment;   /* inclusive range of minStorageBufferOffsetAlignment */
    uint32_t spirv, spirv_words;
    uint32_t bindings, binding_count;
    uint32_t packed_bytes;     /* 0: spirv_words raw words at `spirv`; else a zlib stream of that size */
    uint32_t reserved;
};
static_assert(sizeof(VariantRecord) == 32, "variant layout");

struct Binding {
    uint32_t descriptor_type;  /* VkDescriptorType */
    uint32_t binding;          /* DXVK binding number */
    uint32_t view_type;        /* VkImageViewType */
};
static_assert(sizeof(Binding) == 12, "binding layout");

inline uint64_t fnv1a64(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

}  // namespace shaderlib
}  // namespace rdvk
