/* Shaders and input layouts of the native rendering device.
 *
 * No shader is translated on the device: the game's DXBC containers were translated to SPIR-V at
 * build time (tools/build_shaderlib.py, DXVK 2.4.1's compiler on the PC) and the APK carries the
 * result (shaderlib.cpp).  create_shader looks the container up by its FNV-1a 64 and size, takes
 * the variant for this device's minStorageBufferOffsetAlignment and creates the module straight
 * from the mapped APK range.  The SPIR-V keeps DXVK's binding numbers (one descriptor set, sparse
 * bindings): computeStageBindingOffset(stage) + {CB 0..15, sampler 16..31, SRV 32..159},
 * UAVs from 960 (graphics) / 1088 (compute), UAV counters +64.  Input layouts read the vertex
 * shader's input signature (ISGN) directly. */
#include <atomic>
#include <cstring>
#include <strings.h>
#include "geometry.h"
#include "objects.h"
#include "rdvk.h"
#include "shaderlib_device.h"
#include "stability.h"

namespace rdvk {
namespace {
/* DXVK's binding layout (dxbc_util.h of DXVK 2.4.1). */
constexpr uint32_t kStageBindingCount = 160, kSamplerBindingIndex = 16, kResourceBindingIndex = 32;
constexpr uint32_t kUavBindingIndex = kStageBindingCount * 6, kUavBindingCount = 64;

std::atomic<uint32_t> missing{0};

uint32_t u32(const uint8_t* data, size_t size, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= size) memcpy(&v, data + at, 4);
    return v;
}

/* SPIR-V capabilities that need an optional device feature (external-theories T02: a missing
 * feature turns into a logged fallback, never into an invalid module handed to the driver). */
uint32_t capability_feature(uint32_t capability) {
    switch (capability) {
    case 2: case 24: case 54: return SHADER_FEATURE_GEOMETRY;          /* Geometry, GeometryPointSize, GeometryStreams */
    case 3: case 23: return SHADER_FEATURE_TESSELLATION;               /* Tessellation, TessellationPointSize */
    case 9: return SHADER_FEATURE_FLOAT16;
    case 10: return SHADER_FEATURE_FLOAT64;
    case 11: return SHADER_FEATURE_INT64;
    case 22: return SHADER_FEATURE_INT16;
    case 25: return SHADER_FEATURE_GATHER_EXTENDED;
    case 32: return SHADER_FEATURE_CLIP_DISTANCE;
    case 33: return SHADER_FEATURE_CULL_DISTANCE;
    case 34: case 45: return SHADER_FEATURE_CUBE_ARRAY;               /* ImageCubeArray, SampledCubeArray */
    case 35: return SHADER_FEATURE_SAMPLE_RATE;
    case 55: return SHADER_FEATURE_READ_WITHOUT_FORMAT;
    case 56: return SHADER_FEATURE_WRITE_WITHOUT_FORMAT;
    case 57: return SHADER_FEATURE_MULTI_VIEWPORT;
    case 4427: return SHADER_FEATURE_DRAW_PARAMETERS;
    case 5345: return SHADER_FEATURE_MEMORY_MODEL;
    case 5379: return SHADER_FEATURE_DEMOTE;
    default: return 0;
    }
}

uint32_t missing_shader_features(const uint32_t* code, uint32_t words) {
    uint32_t need = 0;
    for (uint32_t at = 5; at < words;) {
        uint32_t op = code[at] & 0xffff, len = code[at] >> 16;
        if (!len) break;
        if (op == 17 && len >= 2) need |= capability_feature(code[at + 1]);   /* OpCapability */
        else if (op != 17 && op != 10 && op != 11 && op != 14) break;           /* past the capabilities and extensions */
        at += len;
    }
    return need & ~gpu.caps.shader_features;
}

const char* const kShaderFeatureNames[SHADER_FEATURE_COUNT] = {
    "tessellationShader", "geometryShader", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderDrawParameters", "vulkanMemoryModel", "shaderDemoteToHelperInvocation", "imageCubeArray", "shaderImageGatherExtended",
    "shaderInt16", "shaderFloat16", "sampleRateShading", "shaderClipDistance", "shaderCullDistance", "multiViewport", "shaderInt64",
    "shaderFloat64"};

uint32_t ssbo_alignment() {
    uint32_t a = uint32_t(gpu.caps.min_storage_offset ? gpu.caps.min_storage_offset : 16);
    return a < shaderlib::kMinAlignment ? shaderlib::kMinAlignment : a;
}
}  // namespace

uint32_t create_shader(Kind kind, const uint8_t* bytecode, size_t size, Shader** out) {
    *out = nullptr;
    if (!bytecode || size < 32) return d3d::HR_E_INVALIDARG;
    uint64_t hash = shaderlib::fnv1a64(bytecode, size);
    ShaderlibEntry entry{};
    if (!shaderlib_find(bytecode, size, hash, ssbo_alignment(), &entry)) {
        uint32_t n = missing.fetch_add(1) + 1;
        log("RDVK_SHADER_MISSING %s_%016llx bytes=%zu alignment=%u missing=%u (not in the packaged shader library)",
            kind_name(kind), (unsigned long long)hash, size, ssbo_alignment(), n);
        return d3d::HR_E_INVALIDARG;
    }
    auto* s = new Shader(kind);
    snprintf(s->name, sizeof(s->name), "%s_%016llx", kind_name(kind), (unsigned long long)hash);
    s->hash = hash;
    s->role = entry.shader.role;
    if (s->role != ROLE_NONE) log("RDVK_PASS_ROLE %s role=%u", s->name, unsigned(s->role));
    s->stage = VkShaderStageFlagBits(entry.shader.stage);
    s->input_mask = entry.shader.input_mask;
    s->output_mask = entry.shader.output_mask;
    s->patch_vertices = entry.shader.patch_vertices;
    {   /* Declared constant buffer sizes: dcl_constantbuffer cbN[size] in the SHDR/SHEX chunk. */
        uint32_t chunks = u32(bytecode, size, 28);
        for (uint32_t c = 0; c < chunks && c < 32; ++c) {
            uint32_t at = u32(bytecode, size, 32 + 4 * c);
            uint32_t fourcc = u32(bytecode, size, at), length = u32(bytecode, size, at + 4);
            if (fourcc != 0x58454853u && fourcc != 0x52444853u) continue;   /* "SHEX" / "SHDR" */
            size_t pos = at + 8 + 8, end = std::min<size_t>(size, at + 8 + length);
            while (pos + 4 <= end) {
                uint32_t token = u32(bytecode, size, pos), opcode = token & 0x7ff, len = (token >> 24) & 0x7f;
                if (opcode == 0x35) len = u32(bytecode, size, pos + 4);   /* customdata: length in the next token */
                if (!len) break;
                if (opcode == 0x59 && len >= 4) {          /* dcl_constantbuffer */
                    uint32_t operand = u32(bytecode, size, pos + 4);
                    size_t idx = pos + 8 + ((operand >> 31) ? 4 : 0);
                    uint32_t slot = u32(bytecode, size, idx), vec4 = u32(bytecode, size, idx + 4);
                    if (slot < 16) s->cb_vec4[slot] = uint16_t(std::min<uint32_t>(vec4, 4096));
                }
                pos += size_t(len) * 4;
            }
        }
    }
    s->bindings.reserve(entry.binding_count);
    for (uint32_t i = 0; i < entry.binding_count; ++i) {
        shaderlib::Binding b;
        memcpy(&b, entry.bindings + i * sizeof(b), sizeof(b));
        Shader::BindingInfo bi{VkDescriptorType(b.descriptor_type), b.binding, VkImageViewType(b.view_type), 0, 0};
        if (b.binding >= kUavBindingIndex) {
            uint32_t u = b.binding - kUavBindingIndex;
            if (u >= 2 * kUavBindingCount) u -= 2 * kUavBindingCount;  /* compute UAV range */
            bi.space = u >= kUavBindingCount ? 4 : 3;  /* 3 UAV, 4 UAV counter */
            bi.slot = u % kUavBindingCount;
        } else {
            uint32_t r = b.binding % kStageBindingCount;
            if (r < kSamplerBindingIndex) { bi.space = 0; bi.slot = r; }                       /* CB (14 = icb) */
            else if (r < kResourceBindingIndex) { bi.space = 1; bi.slot = r - kSamplerBindingIndex; }
            else { bi.space = 2; bi.slot = r - kResourceBindingIndex; }
        }
        s->bindings.push_back(bi);
    }
    if (entry.uniform) s->uniform_data.assign(entry.uniform, entry.uniform + entry.shader.uniform_size);
    std::vector<uint32_t> scratch;
    const uint32_t* code = shaderlib_spirv(entry, scratch);
    if (!code) { release(s); return d3d::HR_E_INVALIDARG; }
    s->missing_features = missing_shader_features(code, entry.spirv_words);
    if (s->missing_features) {
        /* No module: pipelines with this shader are not created and their draws/dispatches are skipped. */
        for (uint32_t f = 0; f < SHADER_FEATURE_COUNT; ++f)
            if ((s->missing_features & (1u << f)) && first_time(kShaderFeatureNames[f]))
                log("RDVK_FALLBACK %s unsupported by this driver: draws of shaders that need it are skipped (first: %s)",
                    kShaderFeatureNames[f], s->name);
    } else {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size_t(entry.spirv_words) * 4;
        mi.pCode = code;
        vk_check(vk.vkCreateShaderModule(gpu.device, &mi, nullptr, &s->module), "vkCreateShaderModule");
        if (gpu.caps.qualcomm_shader_interfaces) {
            if (kind == Kind::PixelShader) {
                if (scratch.empty()) s->fragment_spirv.assign(code, code + entry.spirv_words);
                else s->fragment_spirv = std::move(scratch);
            } else if (kind == Kind::VertexShader || kind == Kind::DomainShader || kind == Kind::GeometryShader) {
                s->output_widths = shader_output_widths(code, entry.spirv_words);
            }
        }
    }
    if (!s->uniform_data.empty()) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (s->uniform_data.size() + 255) & ~size_t(255);
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        vk_check(vk.vkCreateBuffer(gpu.device, &bi, nullptr, &s->uniform), "vkCreateBuffer(icb)");
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(gpu.device, s->uniform, &req);
        s->uniform_memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        vk_check(vk.vkBindBufferMemory(gpu.device, s->uniform, s->uniform_memory.memory, s->uniform_memory.offset), "bind icb");
        memcpy(s->uniform_memory.mapped, s->uniform_data.data(), s->uniform_data.size());
    }
    *out = s;
    return d3d::HR_S_OK;
}

namespace {
/* The register of an input-signature element (ISGN / ISG1 chunk), as DXVK's DxbcIsgn::find:
 * semantic index and stream 0 equal, the semantic name compared without case. */
bool isgn_register(const uint8_t* b, size_t size, const char* semantic, uint32_t index, uint32_t* reg) {
    if (size < 32 || memcmp(b, "DXBC", 4)) return false;
    uint32_t chunks = u32(b, size, 28);
    for (uint32_t c = 0; c < chunks && c < 64; ++c) {
        uint32_t at = u32(b, size, 32 + 4 * c);
        uint32_t fourcc = u32(b, size, at), length = u32(b, size, at + 4);
        bool isg1 = fourcc == 0x31475349u;                  /* "ISG1": stream + precision words */
        if (fourcc != 0x4E475349u && !isg1) continue;       /* "ISGN" */
        size_t base = at + 8, end = std::min<size_t>(size, base + length);
        uint32_t count = u32(b, end, base);
        size_t stride = isg1 ? 32 : 24;
        for (uint32_t i = 0; i < count && base + 8 + (i + 1) * stride <= end; ++i) {
            size_t e = base + 8 + i * stride;
            uint32_t stream = isg1 ? u32(b, end, e) : 0;
            if (isg1) e += 4;
            uint32_t name = u32(b, end, e), sem_index = u32(b, end, e + 4), reg_id = u32(b, end, e + 16);
            if (stream || sem_index != index || base + name >= end) continue;
            const char* text = reinterpret_cast<const char*>(b + base + name);
            if (!memchr(text, 0, end - (base + name)) || strcasecmp(text, semantic)) continue;
            *reg = reg_id;
            return true;
        }
        return false;
    }
    return false;
}
}  // namespace

uint32_t create_input_layout(const d3d::INPUT_ELEMENT_DESC* elements, uint32_t count, const uint8_t* vs, size_t vs_size, InputLayout** out) {
    *out = nullptr;
    if (!vs || vs_size < 32 || memcmp(vs, "DXBC", 4)) {
        log("RDVK_INPUT_LAYOUT_FAILED no DXBC signature");
        return d3d::HR_E_INVALIDARG;
    }
    auto* layout = new InputLayout();
    uint32_t next_offset[16] = {};   /* D3D11_APPEND_ALIGNED_ELEMENT: after the previous element, 4-byte aligned */
    uint64_t key = 1469598103934665603ull;
    for (uint32_t i = 0; i < count; ++i) {
        const d3d::INPUT_ELEMENT_DESC& e = elements[i];
        const char* semantic = reinterpret_cast<const char*>(e.SemanticName);
        uint32_t offset = e.AlignedByteOffset == 0xFFFFFFFFu ? (next_offset[e.InputSlot & 15] + 3u) & ~3u : e.AlignedByteOffset;
        const Format& f = format(e.Format);
        next_offset[e.InputSlot & 15] = offset + f.bytes;
        uint32_t reg = 0;
        if (!isgn_register(vs, vs_size, semantic ? semantic : "", e.SemanticIndex, &reg)) continue;  /* not consumed by the shader */
        layout->attributes.push_back(InputLayout::Attribute{reg, e.InputSlot, e.Format, offset, f.view});
        check_vertex_format(f.view);
        bool found = false;
        for (auto& b : layout->bindings) if (b.binding == e.InputSlot) found = true;
        if (!found) layout->bindings.push_back(InputLayout::Binding{e.InputSlot, e.InputSlotClass == 1, e.InstanceDataStepRate});
        uint32_t words[4] = {reg, e.InputSlot, e.Format, offset};
        for (uint32_t w : words) { key ^= w; key *= 1099511628211ull; }
        key ^= e.InputSlotClass; key *= 1099511628211ull;
    }
    layout->key = key;
    geometry::layout_created(layout, elements, count);
    *out = layout;
    return d3d::HR_S_OK;
}
}  // namespace rdvk
