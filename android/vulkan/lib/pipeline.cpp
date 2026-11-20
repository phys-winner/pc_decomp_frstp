#include "pipeline.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include "context.h"
#include "rdvk.h"
#include "stability.h"

namespace rdvk {
namespace {
struct KeyHash {
    size_t operator()(const GraphicsKey& k) const {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k); ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return size_t(h);
    }
};
std::mutex mutex;
std::unordered_map<uint64_t, PipelineLayout*> layouts;
std::unordered_map<GraphicsKey, VkPipeline, KeyHash> graphics;
std::unordered_map<const Shader*, VkPipeline> computes;
VkPipelineCache cache = VK_NULL_HANDLE;

Stage engine_stage(VkShaderStageFlagBits s) {
    switch (s) {
    case VK_SHADER_STAGE_VERTEX_BIT: return VS; case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return HS;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return DS; case VK_SHADER_STAGE_GEOMETRY_BIT: return GS;
    case VK_SHADER_STAGE_FRAGMENT_BIT: return PS; default: return CS;
    }
}
std::string cache_path() {
    return std::string(host->files_dir) +
        (gpu.caps.driver_id == VK_DRIVER_ID_MESA_PANVK ? "/rdvk-panvk-pipeline-cache.bin" : "/rdvk-pipeline-cache.bin");
}
}  // namespace

void pipeline_cache_init() {
    std::vector<uint8_t> data;
    if (FILE* f = fopen(cache_path().c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (256l << 20)) { data.resize(size_t(n)); if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); }
        fclose(f);
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vk.vkCreatePipelineCache(gpu.device, &info, nullptr, &cache) != VK_SUCCESS) {
        info.initialDataSize = 0; info.pInitialData = nullptr;
        vk_check(vk.vkCreatePipelineCache(gpu.device, &info, nullptr, &cache), "vkCreatePipelineCache");
    }
    log("RDVK_PIPELINE_CACHE loaded_bytes=%zu", data.size());
}

void pipeline_cache_save() {
    if (!cache) return;
    size_t size = 0;
    if (vk.vkGetPipelineCacheData(gpu.device, cache, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<uint8_t> data(size);
    if (vk.vkGetPipelineCacheData(gpu.device, cache, &size, data.data()) != VK_SUCCESS) return;
    std::string path = cache_path(), part = path + ".part";
    if (FILE* f = fopen(part.c_str(), "wb")) {
        bool ok = fwrite(data.data(), 1, size, f) == size;
        ok = fclose(f) == 0 && ok;
        if (ok) rename(part.c_str(), path.c_str());
    }
}

const PipelineLayout* pipeline_layout(const Shader* const* shaders, uint32_t count, bool compute) {
    std::vector<LayoutEntry> entries;
    for (uint32_t i = 0; i < count; ++i) {
        const Shader* s = shaders[i];
        if (!s) continue;
        for (const Shader::BindingInfo& b : s->bindings) {
            auto it = std::find_if(entries.begin(), entries.end(), [&](const LayoutEntry& e) { return e.binding == b.binding; });
            if (it != entries.end()) { it->stages |= s->stage; continue; }
            LayoutEntry e{b.binding, b.type, VkShaderStageFlags(s->stage), uint8_t(engine_stage(s->stage)), b.space, uint8_t(b.slot), b.view, nullptr};
            if (b.space == 0 && b.slot == 14) e.icb = s;
            entries.push_back(e);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const LayoutEntry& a, const LayoutEntry& b) { return a.binding < b.binding; });
    /* The entries carry the slot and the declared view type that bind_descriptors uses: two shader
     * sets with the same bindings but another view type (t0 as CUBE in one shader, 2D in the other)
     * must not share a layout, or the slot gets the stand-in view and reads zeros (the snow base
     * heightmap of a new game sampled as 0: no snow on the ground). */
    uint64_t key = compute ? 7 : 3;
    for (const LayoutEntry& e : entries) {
        uint64_t words[5] = {e.binding, uint64_t(e.type), e.stages, uint64_t(uintptr_t(e.icb)),
                             uint64_t(e.view) | uint64_t(e.slot) << 8 | uint64_t(e.space) << 16 | uint64_t(e.stage) << 24};
        for (uint64_t w : words) { key ^= w; key *= 1099511628211ull; }
    }
    std::lock_guard lock(mutex);
    auto found = layouts.find(key);
    if (found != layouts.end()) return found->second;
    auto* layout = new PipelineLayout();
    layout->entries = entries;
    layout->key = key;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const LayoutEntry& e : entries) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = e.binding; b.descriptorType = e.type; b.descriptorCount = 1; b.stageFlags = e.stages;
        bindings.push_back(b);
    }
    layout->push = gpu.caps.push_descriptor && bindings.size() <= gpu.caps.max_push_descriptors;
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.flags = layout->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
    set.bindingCount = uint32_t(bindings.size());
    set.pBindings = bindings.data();
    vk_check(vk.vkCreateDescriptorSetLayout(gpu.device, &set, nullptr, &layout->set_layout), "vkCreateDescriptorSetLayout");
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &layout->set_layout;
    info.pushConstantRangeCount = compute ? 0 : 1;
    info.pPushConstantRanges = &push;
    vk_check(vk.vkCreatePipelineLayout(gpu.device, &info, nullptr, &layout->layout), "vkCreatePipelineLayout");
    layouts.emplace(key, layout);
    return layout;
}

VkPipeline graphics_pipeline(const GraphicsKey& key, const PipelineLayout* layout, const InputLayout* input, const BlendState* blend) {
    {
        std::lock_guard lock(mutex);
        auto it = graphics.find(key);
        if (it != graphics.end()) return it->second;
    }
    /* Optional stages (external-theories T02 b): without the feature such draws are skipped. */
    for (const Shader* s : key.shader)
        if (s && !s->module) {   /* a shader the driver cannot run (Shader::missing_features): the draw is skipped */
            std::lock_guard lock(mutex);
            graphics.emplace(key, VK_NULL_HANDLE);
            return VK_NULL_HANDLE;
        }
    bool uses_gs = key.shader[GS] != nullptr, uses_tess = key.shader[HS] || key.shader[DS];
    if ((uses_gs && !gpu.caps.geometry) || (uses_tess && !gpu.caps.tessellation)) {
        if (first_time(uses_gs ? "pipeline_gs" : "pipeline_tess"))
            log("RDVK_FALLBACK %s shaders unsupported by this driver: their draws are skipped (vs=%s)", uses_gs ? "geometry" : "tessellation",
                key.shader[VS] ? key.shader[VS]->name : "-");
        std::lock_guard lock(mutex);
        graphics.emplace(key, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[5];
    uint32_t stage_count = 0;
    for (const Shader* s : key.shader) {
        if (!s) continue;
        VkPipelineShaderStageCreateInfo& st = stages[stage_count++];
        st = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage = s->stage; st.module = s->module; st.pName = "main";
    }
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[32];
    uint32_t vb_count = 0, va_count = 0;
    if (input) {
        for (const auto& b : input->bindings) {
            if (vb_count >= 16) break;
            vb[vb_count++] = VkVertexInputBindingDescription{b.binding, 0, b.instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        }
        for (const auto& a : input->attributes) {
            if (va_count >= 32) break;
            va[va_count++] = VkVertexInputAttributeDescription{a.location, a.binding, a.format, a.offset};
        }
    }
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = vb_count; vertex.pVertexBindingDescriptions = vb;
    vertex.vertexAttributeDescriptionCount = va_count; vertex.pVertexAttributeDescriptions = va;
    static const VkPrimitiveTopology classes[] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = classes[std::min(key.topology_class, 3u)];
    VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    tess.patchControlPoints = std::max(1u, key.patch_points);
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationDepthClipStateCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT};
    clip.depthClipEnable = key.depth_clip;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  gpu.caps.depth_clip ? &clip : nullptr};
    raster.depthClampEnable = key.depth_clamp && gpu.caps.depth_clamp;
    if (!gpu.caps.depth_clip) {
        /* Without VK_EXT_depth_clip_enable clamping is what disables clipping (as in DXVK). */
        raster.depthClampEnable = !key.depth_clip && gpu.caps.depth_clamp;
        if (!key.depth_clip && first_time("depth_clip"))
            log("RDVK_FALLBACK no VK_EXT_depth_clip_enable: DepthClipEnable=FALSE drawn with depth clamp=%d", int(gpu.caps.depth_clamp));
    }
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(key.samples);
    VkSampleMask mask = key.sample_mask;
    ms.pSampleMask = &mask;
    ms.alphaToCoverageEnable = blend ? blend->desc.AlphaToCoverageEnable : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.minDepthBounds = 0; ds.maxDepthBounds = 1;
    VkPipelineColorBlendAttachmentState attachments[8];
    for (uint32_t i = 0; i < key.color_count; ++i) {
        if (blend) attachments[i] = blend->attachments[i];
        else { attachments[i] = VkPipelineColorBlendAttachmentState{}; attachments[i].colorWriteMask = 0xF; }
    }
    if (!gpu.caps.independent_blend && key.color_count > 1) {
        bool differ = false;
        for (uint32_t i = 1; i < key.color_count; ++i) differ |= memcmp(&attachments[i], &attachments[0], sizeof(attachments[0])) != 0;
        if (differ) {
            if (first_time("independent_blend")) log("RDVK_FALLBACK independentBlend unsupported: every target uses the blend state of target 0");
            for (uint32_t i = 1; i < key.color_count; ++i) attachments[i] = attachments[0];
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = key.color_count; cb.pAttachments = attachments;
    std::vector<VkDynamicState> dynamic = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = uint32_t(dynamic.size()); dyn.pDynamicStates = dynamic.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = key.color_count;
    rendering.pColorAttachmentFormats = key.color;
    rendering.depthAttachmentFormat = key.depth;
    rendering.stencilAttachmentFormat = key.stencil ? key.depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    info.stageCount = stage_count; info.pStages = stages;
    info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly;
    info.pTessellationState = key.topology_class == 3 ? &tess : nullptr;
    info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds; info.pColorBlendState = &cb; info.pDynamicState = &dyn;
    info.layout = layout->layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateGraphicsPipelines(gpu.device, cache, 1, &info, nullptr, &pipeline);
    /* Mali can return VK_INCOMPLETE with a valid pipeline (external-theories T05 b): accepted. */
    if (r > VK_SUCCESS && pipeline && first_time("pipeline_result")) log("RDVK_PIPELINE_RESULT %s with a valid pipeline: accepted", vk_result(r));
    if (r < VK_SUCCESS || !pipeline) {
        log("RDVK_PIPELINE_FAILED %s vs=%s ps=%s colors=%u depth=%d", vk_result(r), key.shader[0] ? key.shader[0]->name : "-",
            key.shader[4] ? key.shader[4]->name : "-", key.color_count, int(key.depth));
        pipeline = VK_NULL_HANDLE;
    }
    std::lock_guard lock(mutex);
    graphics.emplace(key, pipeline);
    ++g_ctx.pipelines_created;
    return pipeline;
}

VkPipeline compute_pipeline(const Shader* cs, const PipelineLayout* layout) {
    {
        std::lock_guard lock(mutex);
        auto it = computes.find(cs);
        if (it != computes.end()) return it->second;
    }
    if (!cs->module) {   /* Shader::missing_features: the dispatch is skipped */
        std::lock_guard lock(mutex);
        computes.emplace(cs, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module = cs->module; info.stage.pName = "main";
    info.layout = layout->layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateComputePipelines(gpu.device, cache, 1, &info, nullptr, &pipeline);
    if (r < VK_SUCCESS || !pipeline) { log("RDVK_COMPUTE_PIPELINE_FAILED %s %s", vk_result(r), cs->name); pipeline = VK_NULL_HANDLE; }
    std::lock_guard lock(mutex);
    computes.emplace(cs, pipeline);
    return pipeline;
}

void pipelines_forget_shader(Shader* s) {
    std::lock_guard lock(mutex);
    for (auto it = graphics.begin(); it != graphics.end();) {
        bool uses = false;
        for (const Shader* k : it->first.shader) uses |= k == s;
        if (uses) {
            VkPipeline p = it->second;
            if (p) gpu.defer([p] { vk.vkDestroyPipeline(gpu.device, p, nullptr); });
            it = graphics.erase(it);
        } else ++it;
    }
    auto c = computes.find(s);
    if (c != computes.end()) {
        VkPipeline p = c->second;
        if (p) gpu.defer([p] { vk.vkDestroyPipeline(gpu.device, p, nullptr); });
        computes.erase(c);
    }
}
}  // namespace rdvk
