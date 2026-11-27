/* Pipeline layouts (one descriptor set, DXVK binding numbers) and graphics/compute pipelines. */
#pragma once
#include <cstdint>
#include <vector>
#include "objects.h"

namespace rdvk {
struct LayoutEntry {
    uint32_t binding;
    VkDescriptorType type;
    VkShaderStageFlags stages;
    uint8_t stage;   /* engine stage (rdvk::Stage) whose bindings feed it */
    uint8_t space;   /* 0 CB, 1 sampler, 2 SRV, 3 UAV, 4 UAV counter */
    uint8_t slot;
    VkImageViewType view;
    const Shader* icb = nullptr;  /* immediate constant buffer of this shader (CB slot 14) */
};

struct PipelineLayout {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<LayoutEntry> entries;
    bool push = false;
    uint64_t key = 0;
};

struct GraphicsKey {
    const Shader* shader[5] = {};   /* VS HS DS GS PS */
    uint64_t input = 0, blend = 0;
    VkFormat color[8] = {};
    VkFormat depth = VK_FORMAT_UNDEFINED;
    uint32_t color_count = 0, samples = 1, topology_class = 0, patch_points = 0, sample_mask = ~0u;
    uint32_t depth_clamp = 0, depth_clip = 1, stencil = 0;
    bool operator==(const GraphicsKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
};

const PipelineLayout* pipeline_layout(const Shader* const* shaders, uint32_t count, bool compute);
VkPipeline graphics_pipeline(const GraphicsKey& key, const PipelineLayout* layout, const InputLayout* input, const BlendState* blend);
VkPipeline compute_pipeline(const Shader* cs, const PipelineLayout* layout);
void pipeline_cache_init();
void pipeline_cache_save(const char* reason = "exit");
bool pipeline_cache_save_async(const char* reason);
void pipeline_cache_wait_and_save();
void pipeline_cache_drop_device_lost();
}  // namespace rdvk
