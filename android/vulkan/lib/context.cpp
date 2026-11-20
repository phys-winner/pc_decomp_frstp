#include "stability.h"
#include "context.h"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <cstring>
#include "geometry.h"
#include "pipeline.h"
#include "rdvk.h"

namespace rdvk {
Context g_ctx;

namespace {
enum Dirty : uint64_t {
    D_PIPELINE = 1, D_VIEWPORT = 2, D_SCISSOR = 4, D_RASTER = 8, D_DEPTH = 16, D_BLEND = 32, D_VB = 64, D_IB = 128,
    D_TOPOLOGY = 256, D_DESCRIPTORS = 512, D_LAYOUT = 1024, D_ALL = ~0ull
};

/* Pending barriers, flushed as one vkCmdPipelineBarrier. */
struct Barriers {
    std::vector<VkImageMemoryBarrier> images;
    VkPipelineStageFlags src = 0, dst = 0;
    VkAccessFlags memory_src = 0, memory_dst = 0;
    void flush(VkCommandBuffer cmd) {
        if (images.empty() && !memory_src) return;
        VkMemoryBarrier mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mem.srcAccessMask = memory_src; mem.dstAccessMask = memory_dst;
        vk.vkCmdPipelineBarrier(cmd, src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst ? dst : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                memory_src ? 1 : 0, &mem, 0, nullptr, uint32_t(images.size()), images.data());
        images.clear(); src = dst = 0; memory_src = memory_dst = 0;
    }
};
Barriers barriers;

constexpr VkPipelineStageFlags SHADERS = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
    VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags DEPTH_STAGES = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

void layout_access(VkImageLayout layout, bool source, VkPipelineStageFlags& stage, VkAccessFlags& access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED: stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; access = 0; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = source ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : (VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stage = DEPTH_STAGES;
        access = source ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                        : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        stage = DEPTH_STAGES | SHADERS;
        access = source ? 0 : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: stage = SHADERS; access = source ? 0 : VK_ACCESS_SHADER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: stage = VK_PIPELINE_STAGE_TRANSFER_BIT; access = source ? 0 : VK_ACCESS_TRANSFER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: stage = VK_PIPELINE_STAGE_TRANSFER_BIT; access = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_GENERAL:
        stage = SHADERS | VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = source ? (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT) : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; access = 0; break;
    default: stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
}

VkPrimitiveTopology vk_topology(uint32_t t, uint32_t* klass, uint32_t* points) {
    *points = 0;
    switch (t) {
    case 1: *klass = 0; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: *klass = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: *klass = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: *klass = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 10: *klass = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: *klass = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: *klass = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: *klass = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default:
        if (t >= 33 && t <= 64) { *klass = 3; *points = t - 32; return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; }
        *klass = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
VkStencilOp stencil_op(uint32_t op) {
    switch (op) {
    case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP;
    }
}
bool is_depth_texture(const Texture* t) { return t->aspect != VK_IMAGE_ASPECT_COLOR_BIT; }

bool srv_layout_matches(const View& view, VkImageLayout layout) {
    const Texture* t = view.texture;
    const auto& range = view.range;
    if (t->layout(range.baseMipLevel, range.baseArrayLayer) != layout) return false;
    if ((range.levelCount == 1 && range.layerCount == 1) || !gpu.caps.defer_attachment_transitions ||
        !(t->bind & (d3d::BIND_RENDER_TARGET | d3d::BIND_DEPTH_STENCIL))) return true;
    /* A render pass may have written a later mip or array layer of this SRV. */
    for (uint32_t layer = range.baseArrayLayer; layer < range.baseArrayLayer + range.layerCount; ++layer)
        for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + range.levelCount; ++mip)
            if (t->layout(mip, layer) != layout) return false;
    return true;
}

/* Last values set in the current command buffer (dynamic state). */
struct DynamicCache {
    VkCullModeFlags cull = ~0u; VkFrontFace front = VK_FRONT_FACE_MAX_ENUM;
    VkBool32 bias_enable = 2; float bias[3] = {-1, -1, -1};
    VkBool32 depth_test = 2, depth_write = 2, stencil_test = 2; VkCompareOp depth_op = VK_COMPARE_OP_MAX_ENUM;
    uint32_t stencil_hash = ~0u, stencil_ref = ~0u;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    float blend[4] = {-1, -1, -1, -1};
    VkViewport viewport{}; VkRect2D scissor{};
    bool viewport_valid = false, scissor_valid = false;
};
DynamicCache dyn;

/* Descriptors pushed in the current command buffer (per layout entry). */
struct PushedEntry { uint64_t a = 0, b = 0, c = 0; };
const PipelineLayout* pushed_layout = nullptr;
VkPipelineBindPoint pushed_point = VK_PIPELINE_BIND_POINT_MAX_ENUM;
PushedEntry pushed[64];

/* Descriptor pools for layouts above the push limit, one per frame slot. */
struct PoolSlot { std::vector<VkDescriptorPool> pools; size_t next = 0; uint64_t serial = 0; };
PoolSlot pool_slots[Gpu::FRAMES];
VkDescriptorSet allocate_set(VkDescriptorSetLayout layout) {
    PoolSlot& slot = pool_slots[gpu.serial() % Gpu::FRAMES];
    if (slot.serial != gpu.serial()) {
        for (VkDescriptorPool p : slot.pools) vk.vkResetDescriptorPool(gpu.device, p, 0);
        slot.next = 0; slot.serial = gpu.serial();
    }
    for (;;) {
        if (slot.next == slot.pools.size()) {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                            {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = 1024; info.poolSizeCount = 7; info.pPoolSizes = sizes;
            VkDescriptorPool pool;
            vk_check(vk.vkCreateDescriptorPool(gpu.device, &info, nullptr, &pool), "vkCreateDescriptorPool");
            slot.pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = slot.pools[slot.next]; info.descriptorSetCount = 1; info.pSetLayouts = &layout;
        VkDescriptorSet set;
        if (vk.vkAllocateDescriptorSets(gpu.device, &info, &set) == VK_SUCCESS) return set;
        ++slot.next;
    }
}

VkBuffer dummy_buffer = VK_NULL_HANDLE;
Allocation dummy_memory;
/* Stand-ins for unbound SRV/UAV slots where the driver has no robustness2 nullDescriptor (the
 * Mali-G615 driver of the Lenovo tablet, Vulkan 1.3.247, crashed in vkUpdateDescriptorSets on a
 * VK_NULL_HANDLE view): zeroed 1x1 images of every view type in GENERAL layout and a texel view of
 * the zeroed buffer, as DXVK does. */
struct Dummies {
    VkImage image[3] = {};          /* 1D, 2D (6 layers, cube compatible), 3D */
    Allocation memory[3];
    VkImageView view[7] = {};       /* by VkImageViewType */
    VkBufferView texel = VK_NULL_HANDLE;
} dummies;

VkImageView dummy_view(VkImageViewType type) {
    if (gpu.caps.robustness2_null) return VK_NULL_HANDLE;
    VkImageView v = unsigned(type) < 7 ? dummies.view[type] : VK_NULL_HANDLE;
    return v ? v : dummies.view[VK_IMAGE_VIEW_TYPE_2D];
}
VkBufferView dummy_texel() { return gpu.caps.robustness2_null ? VK_NULL_HANDLE : dummies.texel; }

void create_dummies() {
    if (gpu.caps.robustness2_null) return;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties props{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &props);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageType types[3] = {VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D};
    for (int i = 0; i < 3; ++i) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = types[i];
        info.format = format;
        info.extent = VkExtent3D{1, 1, 1};
        info.mipLevels = 1;
        info.arrayLayers = i == 1 ? 6 : 1;
        if (i == 1) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vk_check(vk.vkCreateImage(gpu.device, &info, nullptr, &dummies.image[i]), "vkCreateImage(dummy)");
        VkMemoryRequirements req;
        vk.vkGetImageMemoryRequirements(gpu.device, dummies.image[i], &req);
        dummies.memory[i] = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        vk_check(vk.vkBindImageMemory(gpu.device, dummies.image[i], dummies.memory[i].memory, dummies.memory[i].offset), "bind dummy image");
    }
    struct { VkImageViewType type; int image; uint32_t layers; } views[] = {
        {VK_IMAGE_VIEW_TYPE_1D, 0, 1}, {VK_IMAGE_VIEW_TYPE_1D_ARRAY, 0, 1}, {VK_IMAGE_VIEW_TYPE_2D, 1, 1},
        {VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 1, 6}, {VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 1, 6},
        {VK_IMAGE_VIEW_TYPE_3D, 2, 1}};
    for (const auto& v : views) {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = dummies.image[v.image];
        info.viewType = v.type;
        info.format = format;
        info.subresourceRange = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, v.layers};
        if (vk.vkCreateImageView(gpu.device, &info, nullptr, &dummies.view[v.type]) != VK_SUCCESS) dummies.view[v.type] = VK_NULL_HANDLE;
    }
    VkBufferViewCreateInfo texel{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    texel.buffer = dummy_buffer;
    texel.format = VK_FORMAT_R32_UINT;
    texel.range = 65536;
    if (vk.vkCreateBufferView(gpu.device, &texel, nullptr, &dummies.texel) != VK_SUCCESS) dummies.texel = VK_NULL_HANDLE;
    /* Zero and GENERAL, before the first frame. */
    auto lock = gpu.upload_lock();
    gpu.upload([](VkCommandBuffer cmd) {
        for (int i = 0; i < 3; ++i) {
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 1 ? 6u : 1u};
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dummies.image[i]; b.subresourceRange = range;
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkClearColorValue zero{};
            vk.vkCmdClearColorImage(cmd, dummies.image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
    });
    log("RDVK_NULL_DESCRIPTORS stand-ins: no robustness2 nullDescriptor on this driver (storage=%d)", (usage & VK_IMAGE_USAGE_STORAGE_BIT) ? 1 : 0);
}
}  // namespace

void Context::init() {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = VK_LOD_CLAMP_NONE;
    vk_check(vk.vkCreateSampler(gpu.device, &s, nullptr, &default_sampler_), "vkCreateSampler(default)");
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = 65536;
    b.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    vk_check(vk.vkCreateBuffer(gpu.device, &b, nullptr, &dummy_buffer), "vkCreateBuffer(dummy)");
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(gpu.device, dummy_buffer, &req);
    dummy_memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    vk_check(vk.vkBindBufferMemory(gpu.device, dummy_buffer, dummy_memory.memory, dummy_memory.offset), "bind dummy");
    memset(dummy_memory.mapped, 0, 65536);
    create_dummies();
    viewport_ = d3d::VIEWPORT{0, 0, 1, 1, 0, 1};
}

Slice Context::stream_slice(VkDeviceSize bytes) { return gpu.stream.allocate((bytes + 15) & ~VkDeviceSize(15)); }

/* ------------------------------------------------------------------ state setters */
void Context::set_shader(Stage stage, Shader* shader) { if (shader_[stage] != shader) { shader_[stage] = shader; dirty_ |= D_PIPELINE | D_DESCRIPTORS | D_LAYOUT; } }

/* Pipeline layouts by shader combination, without building the binding list per draw. */
const PipelineLayout* Context::layout_for(const Shader* const* shaders, uint32_t count, bool compute) {
    LayoutKey key{};
    for (uint32_t i = 0; i < count && i < 5; ++i) key.shader[i] = shaders[i];
    key.compute = compute;
    auto found = layout_cache_.find(key);
    if (found != layout_cache_.end()) return found->second;
    const PipelineLayout* layout = pipeline_layout(shaders, count, compute);
    layout_cache_.emplace(key, layout);
    return layout;
}
void Context::set_constant_buffer(Stage stage, uint32_t slot, Buffer* b) { if (slot < MAX_CB) { stage_[stage].cb[slot] = b; dirty_ |= D_DESCRIPTORS; } }
void Context::set_srv(Stage stage, uint32_t slot, View* v) { if (slot < MAX_SRV) { stage_[stage].srv[slot] = v; dirty_ |= D_DESCRIPTORS; } }
void Context::set_sampler(Stage stage, uint32_t slot, Sampler* s) { if (slot < MAX_SAMPLER) { stage_[stage].sampler[slot] = s; dirty_ |= D_DESCRIPTORS; } }
void Context::set_uav(uint32_t slot, View* v) { if (slot < MAX_UAV) { uav_[slot] = v; dirty_ |= D_DESCRIPTORS; } }
void Context::set_vertex_buffer(uint32_t slot, Buffer* b, uint32_t offset, uint32_t stride) {
    if (slot >= MAX_VB) return;
    vb_[slot] = VertexBinding{b, offset, stride};
    dirty_ |= D_VB;
}
void Context::set_index_buffer(Buffer* b, uint32_t dxgi, uint32_t offset) { ib_ = b; ib_format_ = dxgi; ib_offset_ = offset; dirty_ |= D_IB; }
void Context::set_input_layout(InputLayout* l) { if (layout_ != l) { layout_ = l; dirty_ |= D_PIPELINE; } }
void Context::set_topology(uint32_t t) { if (topology_ != t) { topology_ = t; dirty_ |= D_TOPOLOGY | D_PIPELINE; } }
void Context::set_blend(BlendState* s, const float factor[4], uint32_t mask) {
    if (blend_ != s || sample_mask_ != mask) dirty_ |= D_PIPELINE;
    blend_ = s; sample_mask_ = mask;
    if (factor) memcpy(blend_factor_, factor, 16); else { for (float& f : blend_factor_) f = 1.0f; }
    dirty_ |= D_BLEND;
}
void Context::set_depth(DepthState* s, uint32_t ref) { depth_ = s; stencil_ref_ = ref; dirty_ |= D_DEPTH; }
void Context::set_raster(RasterState* s) {
    if (raster_ != s) {
        bool clip_changed = !raster_ || !s || raster_->desc.DepthClipEnable != s->desc.DepthClipEnable;
        raster_ = s;
        dirty_ |= D_RASTER | D_SCISSOR | (clip_changed ? D_PIPELINE : 0);
    }
}
void Context::set_viewport(const d3d::VIEWPORT& v) { viewport_ = v; viewport_set_ = true; dirty_ |= D_VIEWPORT; }
void Context::set_scissor(const d3d::RECT& r) { scissor_ = r; scissor_set_ = true; dirty_ |= D_SCISSOR; }

void Context::set_targets(uint32_t count, View* const* rtvs, View* dsv) {
    count = std::min(count, MAX_RT);
    View* next[MAX_RT] = {};
    for (uint32_t i = 0; i < count; ++i) next[i] = rtvs ? rtvs[i] : nullptr;
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; ++i) if (next[i]) used = i + 1;
    bool same = dsv == dsv_ && used == rt_count_;
    for (uint32_t i = 0; same && i < used; ++i) same = next[i] == rtv_[i];
    if (same) return;
    end_pass(PE_TARGETS);
    for (uint32_t i = 0; i < MAX_RT; ++i) rtv_[i] = i < used ? next[i] : nullptr;
    rt_count_ = used;
    dsv_ = dsv;
    dirty_ |= D_PIPELINE | D_SCISSOR | D_VIEWPORT;
}

/* ------------------------------------------------------------------ transitions */
void Context::transition(Texture* t, const VkImageSubresourceRange& range, VkImageLayout layout, bool rebind) {
    uint32_t mip_end = range.baseMipLevel + (range.levelCount == VK_REMAINING_MIP_LEVELS ? t->mips - range.baseMipLevel : range.levelCount);
    uint32_t layer_end = range.baseArrayLayer + (range.layerCount == VK_REMAINING_ARRAY_LAYERS ? t->layers - range.baseArrayLayer : range.layerCount);
    for (uint32_t layer = range.baseArrayLayer; layer < layer_end; ++layer)
        for (uint32_t mip = range.baseMipLevel; mip < mip_end; ++mip) {
            VkImageLayout& current = t->layouts[layer * t->mips + mip];
            if (current == layout && !rebind) continue;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            VkPipelineStageFlags src, dst;
            layout_access(current, true, src, b.srcAccessMask);
            layout_access(layout, false, dst, b.dstAccessMask);
            b.oldLayout = current; b.newLayout = layout;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->image;
            b.subresourceRange = VkImageSubresourceRange{t->aspect, mip, 1, layer, 1};
            barriers.images.push_back(b);
            barriers.src |= src; barriers.dst |= dst;
            current = layout;
            if (!gpu.caps.defer_attachment_transitions &&
                (layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)) {
                if (std::find(attachment_layouts_.begin(), attachment_layouts_.end(), t) == attachment_layouts_.end())
                    attachment_layouts_.push_back(t);
            }
        }
}

void Context::transition_view(View* v, VkImageLayout layout) { transition(v->texture, v->range, layout); }

VkImageLayout Context::srv_layout(const View* v) const {
    return is_depth_texture(v->texture) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* Textures left in an attachment layout by earlier passes go to their sampling layout now, so
 * later draws never have to split a render pass to read them. */
void Context::sweep_attachments() {
    if (gpu.caps.defer_attachment_transitions) return;
    for (size_t i = 0; i < attachment_layouts_.size();) {
        Texture* t = attachment_layouts_[i];
        bool current = false;
        for (uint32_t r = 0; r < rt_count_; ++r) current |= rtv_[r] && rtv_[r]->texture == t;
        if (dsv_ && dsv_->texture == t) current = true;
        if (current) { ++i; continue; }
        VkImageLayout target = is_depth_texture(t) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachment_layouts_.erase(attachment_layouts_.begin() + long(i));
        for (uint32_t layer = 0; layer < t->layers; ++layer)
            for (uint32_t mip = 0; mip < t->mips; ++mip) {
                VkImageLayout l = t->layout(mip, layer);
                if (l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
                    transition(t, VkImageSubresourceRange{t->aspect, mip, 1, layer, 1}, target);
            }
    }
}

/* ------------------------------------------------------------------ render passes */
void Context::begin_pass() {
    cmd_ = gpu.cmd();
    sweep_attachments();
    /* A subsequent pass still needs to see writes to an attachment it reuses in place. */
    bool rebind = gpu.caps.defer_attachment_transitions;
    for (uint32_t i = 0; i < rt_count_; ++i)
        if (rtv_[i]) {
            transition(rtv_[i]->texture, rtv_[i]->range, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rebind);
            use(rtv_[i]->texture);
        }
    if (dsv_) {
        transition(dsv_->texture, dsv_->range, dsv_->read_only_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   rebind && !dsv_->read_only_depth);
        use(dsv_->texture);
    }
    barriers.flush(cmd_);
    VkRenderingAttachmentInfo colors[MAX_RT];
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    for (uint32_t i = 0; i < rt_count_; ++i) {
        VkRenderingAttachmentInfo& a = colors[i];
        a = VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!rtv_[i]) { a.imageView = VK_NULL_HANDLE; continue; }
        a.imageView = rtv_[i]->view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = clear_[i].color ? VK_ATTACHMENT_LOAD_OP_CLEAR : clear_[i].discard ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue = clear_[i].value;
        width = std::min(width, rtv_[i]->width); height = std::min(height, rtv_[i]->height);
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (dsv_) {
        PendingClear& c = clear_[MAX_RT];
        depth.imageView = dsv_->view;
        depth.imageLayout = dsv_->read_only_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = c.depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue = c.value;
        stencil = depth;
        stencil.loadOp = c.stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        width = std::min(width, dsv_->width); height = std::min(height, dsv_->height);
    }
    if (width == UINT32_MAX) { width = uint32_t(std::max(1.0f, viewport_.Width)); height = uint32_t(std::max(1.0f, viewport_.Height)); }
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = VkRect2D{{0, 0}, {width, height}};
    info.layerCount = 1;
    info.colorAttachmentCount = rt_count_;
    info.pColorAttachments = colors;
    bool stencil_aspect = dsv_ && (dsv_->texture->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    info.pDepthAttachment = dsv_ ? &depth : nullptr;
    info.pStencilAttachment = stencil_aspect ? &stencil : nullptr;
    if (tracing)
        log("RDVK_TRACE pass_begin #%llu rt0=%p cload=%d ds=%p:%s dload=%d sload=%d satt=%d dlayout=%d %ux%u", (unsigned long long)passes + 1,
            rt_count_ && rtv_[0] ? static_cast<void*>(rtv_[0]->texture) : nullptr, rt_count_ ? int(colors[0].loadOp) : -1,
            dsv_ ? static_cast<void*>(dsv_->texture) : nullptr,
            dsv_ && dsv_->read_only_depth ? "ro" : "rw", int(depth.loadOp), int(stencil.loadOp), int(info.pStencilAttachment != nullptr),
            int(depth.imageLayout), width, height);
    vk.vkCmdBeginRendering(cmd_, &info);
    for (PendingClear& c : clear_) c = PendingClear{};
    in_pass_ = true;
    if (gpu.caps.reset_dynamic_per_pass) {
        dyn = DynamicCache{};
        dirty_ |= D_VIEWPORT | D_SCISSOR | D_RASTER | D_DEPTH | D_BLEND | D_TOPOLOGY | D_VB;
    }
    for (Query* q : active_queries_) open_query(q);
    ++passes;
}

/* Ends the current pass.  Outside a pass, clears still folded into the load ops of the current
 * targets are carried out by an empty pass first: whatever comes next (other targets, a copy, a
 * dispatch, a readback, the present) must see them. */
/* ------------------------------------------------------------------ occlusion queries */
namespace {
constexpr uint32_t QUERY_POOL_SIZE = 1024;
std::vector<VkQueryPool> query_pools;
std::vector<uint32_t> free_slots;                    /* reset, ready to begin */
std::deque<std::pair<uint64_t, uint32_t>> retired_slots;   /* (serial of the last use, slot) */

int occlusion_mode() {   /* 0 real queries, 1 old stub (0 samples), 2 always visible */
    static const int mode = [] {
        std::string m = property("debug.fp.rdvk_occlusion");
        int v = m == "stub" ? 1 : m == "visible" ? 2 : 0;
        if (v == 0 && !gpu.caps.host_query_reset) v = 2;
        log("RDVK_OCCLUSION mode=%s", v == 0 ? "vulkan queries" : v == 1 ? "stub (0 samples)" : "always visible");
        return v;
    }();
    return mode;
}
bool is_occlusion(const Query* q) { return q->desc.Query == d3d::QUERY_OCCLUSION || q->desc.Query == d3d::QUERY_OCCLUSION_PREDICATE; }
VkQueryPool pool_of(uint32_t slot) { return query_pools[slot / QUERY_POOL_SIZE]; }

uint32_t take_slot() {
    uint64_t done = gpu.completed();
    while (!retired_slots.empty() && retired_slots.front().first <= done) {
        uint32_t s = retired_slots.front().second;
        retired_slots.pop_front();
        vk.vkResetQueryPool(gpu.device, pool_of(s), s % QUERY_POOL_SIZE, 1);
        free_slots.push_back(s);
    }
    if (free_slots.empty()) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_OCCLUSION;
        info.queryCount = QUERY_POOL_SIZE;
        VkQueryPool pool = VK_NULL_HANDLE;
        if (vk.vkCreateQueryPool(gpu.device, &info, nullptr, &pool) != VK_SUCCESS) return UINT32_MAX;
        vk.vkResetQueryPool(gpu.device, pool, 0, QUERY_POOL_SIZE);
        uint32_t base = uint32_t(query_pools.size()) * QUERY_POOL_SIZE;
        query_pools.push_back(pool);
        for (uint32_t i = QUERY_POOL_SIZE; i-- > 0;) free_slots.push_back(base + i);
        log("RDVK_QUERY_POOL occlusion pools=%zu", query_pools.size());
    }
    uint32_t s = free_slots.back();
    free_slots.pop_back();
    return s;
}
void retire(std::vector<uint32_t>& slots, uint64_t serial) {
    for (uint32_t s : slots) retired_slots.emplace_back(serial, s);
    slots.clear();
}
}  // namespace

void Context::trace_poll() {
    static std::string last = property("debug.fp.rdvk_trace");
    if (tracing && trace_left_ > 1) { --trace_left_; log("RDVK_TRACE frame=%llu", (unsigned long long)frames); }
    else if (tracing) { tracing = false; trace_left_ = 0; log("RDVK_TRACE end frame=%llu", (unsigned long long)frames); }
    if (Texture* t = dump_pending) {
        dump_pending = nullptr;
        uint32_t w = 0, h = 0, f = 0;
        uint8_t* rgba = nullptr;
        uint32_t hr = 0;
        if (t->dxgi == 10 && t->kind == Kind::Texture2D) {
            /* RGBA16F: staging copy, halves to 8-bit RGBA (clamped). */
            w = t->width; h = t->height; f = 10;
            d3d::TEXTURE2D_DESC desc{w, h, 1, 1, 10, {1, 0}, d3d::USAGE_STAGING, 0, d3d::CPU_ACCESS_READ, 0};
            Texture* staging = nullptr;
            hr = create_texture(2, &desc, nullptr, &staging);
            if (hr == d3d::HR_S_OK) {
                copy_resource(staging, t);
                d3d::MAPPED_SUBRESOURCE mapped{};
                hr = map(staging, 0, d3d::MAP_READ, 0, &mapped);
                if (hr == d3d::HR_S_OK) {
                    rgba = static_cast<uint8_t*>(malloc(size_t(w) * h * 4));
                    auto half = [](uint16_t v) {
                        uint32_t e = (v >> 10) & 31, m = v & 1023;
                        float x = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * float(1u << e) / 32768.0f;
                        return (v & 0x8000) ? -x : x;
                    };
                    for (uint32_t y = 0; y < h; ++y) {
                        const uint16_t* row = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
                        for (uint32_t x = 0; x < w * 4; ++x) {
                            float v = half(row[x]);
                            rgba[size_t(y) * w * 4 + x] = uint8_t(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f);
                        }
                    }
                    unmap(staging, 0);
                }
                release(staging);
            }
        } else {
            hr = read_texture(guest_of(t), &w, &h, &f, &rgba);
        }
        size_t nonzero = 0, alpha = 0;
        if (rgba) {
            for (size_t i = 0; i < size_t(w) * h; ++i) { nonzero += (rgba[i * 4] | rgba[i * 4 + 1] | rgba[i * 4 + 2]) != 0; alpha += rgba[i * 4 + 3] != 0; }
            char path[512];
            snprintf(path, sizeof(path), "%s/rdvk-dump-%ux%u.rgba", host->files_dir, w, h);
            if (FILE* file = fopen(path, "wb")) { fwrite(rgba, 4, size_t(w) * h, file); fclose(file); }
            free(rgba);
        }
        log("RDVK_DUMP tex=%p %ux%u dxgi=%u hr=0x%x pixels_with_colour=%zu pixels_with_alpha=%zu", static_cast<void*>(t), w, h, f, hr, nonzero, alpha);
        release(t);
        if (t == capture_copy_) { release(capture_copy_); capture_copy_ = nullptr; }
    }
    frame_draw_ = 0;
    if (frames % 30 == 0) {
        std::string skip = property("debug.fp.rdvk_skip");
        unsigned long long a = 0, b = 0;
        if (skip.empty() || sscanf(skip.c_str(), "%llu-%llu", &a, &b) != 2) a = b = 0;
        if (a != skip_lo_ || b != skip_hi_) log("RDVK_SKIP draws %llu..%llu of each frame", a, b);
        skip_lo_ = a; skip_hi_ = b;
    }
    if (frames % 60) return;
    std::string now = property("debug.fp.rdvk_trace");
    if (now != last && !now.empty() && now != "0") { tracing = true; trace_left_ = uint32_t(std::max(1, atoi(now.c_str()))); log("RDVK_TRACE begin frame=%llu draws so far=%llu", (unsigned long long)frames + 1, (unsigned long long)draws); }
    last = now;
}

void Context::capture_after_draw() {
    static const long long want = property("debug.fp.rdvk_capture").empty() ? -1 : atoll(property("debug.fp.rdvk_capture").c_str());
    if (want < 0 || (long long)(frame_draw_ - 1) != want || capture_copy_ || !rt_count_ || !rtv_[0]) return;
    Texture* src = rtv_[0]->texture;
    d3d::TEXTURE2D_DESC desc{src->width, src->height, 1, 1, src->dxgi, {1, 0}, d3d::USAGE_DEFAULT, d3d::BIND_SHADER_RESOURCE, 0, 0};
    if (create_texture(2, &desc, nullptr, &capture_copy_) != d3d::HR_S_OK) { capture_copy_ = nullptr; return; }
    copy_resource(capture_copy_, src);   /* ends the pass; the next draw begins a new one with LOAD */
    dump_pending = capture_copy_;
    add_ref(capture_copy_);
    log("RDVK_CAPTURE rt0=%p dxgi=%u %ux%u after draw fi=%lld", static_cast<void*>(src), src->dxgi, src->width, src->height, want);
}

void Context::trace_draw(const char* kind, uint32_t count, uint32_t instances, bool skipped) {
    char line[1600];
    int at = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (at < int(sizeof(line)) - 1) at += snprintf(line + at, sizeof(line) - size_t(at), fmt, a...);
    };
    put("RDVK_TRACE d=%llu fi=%llu %s n=%u inst=%u skip=%d pipe=%d pass=%llu", (unsigned long long)draws, (unsigned long long)frame_draw_ - 1, kind, count, instances, int(skipped),
        int(bound_pipeline_ != VK_NULL_HANDLE), (unsigned long long)passes);
    for (uint32_t i = 0; i < rt_count_ && i < MAX_RT; ++i)
        if (rtv_[i]) put(" rt%u=%p:%u:%ux%u", i, static_cast<void*>(rtv_[i]->texture), rtv_[i]->texture->dxgi, rtv_[i]->width, rtv_[i]->height);
    if (dsv_) put(" ds=%p:%u%s", static_cast<void*>(dsv_->texture), dsv_->texture->dxgi, dsv_->read_only_depth ? ":ro" : "");
    put(" vp=%.0f,%.0f,%.0f,%.0f", double(viewport_.TopLeftX), double(viewport_.TopLeftY), double(viewport_.Width), double(viewport_.Height));
    if (raster_ && raster_->desc.ScissorEnable) put(" sc=%d,%d,%d,%d", scissor_.left, scissor_.top, scissor_.right, scissor_.bottom);
    put(" vs=%s ps=%s", shader_[VS] ? shader_[VS]->name : "-", shader_[PS] ? shader_[PS]->name : "-");
    if (blend_) {
        const d3d::RENDER_TARGET_BLEND_DESC& b = blend_->desc.RenderTarget[0];
        put(" blend=%u:%u/%u:%u/%u:wm%x a2c=%u", b.BlendEnable, b.SrcBlend, b.DestBlend, b.SrcBlendAlpha, b.DestBlendAlpha, b.RenderTargetWriteMask,
            blend_->desc.AlphaToCoverageEnable);
    }
    if (depth_) {
        const d3d::DEPTH_STENCIL_DESC& z = depth_->desc;
        put(" depth=%u/%u/%u stencil=%u:%u:ref%u ops=%u%u%u%u/%u%u%u%u masks=%02x/%02x", z.DepthEnable, z.DepthWriteMask, z.DepthFunc, z.StencilEnable,
            z.FrontFace.StencilFunc, stencil_ref_, z.FrontFace.StencilFailOp, z.FrontFace.StencilDepthFailOp, z.FrontFace.StencilPassOp,
            z.FrontFace.StencilFunc, z.BackFace.StencilFailOp, z.BackFace.StencilDepthFailOp, z.BackFace.StencilPassOp, z.BackFace.StencilFunc,
            z.StencilReadMask, z.StencilWriteMask);
    }
    if (raster_) put(" cull=%u", raster_->desc.CullMode);
    if (Shader* ps = shader_[PS]) {
        for (const Shader::BindingInfo& b : ps->bindings) {
            if (b.space == 0 && b.slot < MAX_CB) {
                Buffer* cb = stage_[PS].cb[b.slot];
                put(" cb%u=%u/%u", b.slot, cb ? cb->desc.ByteWidth : 0u, b.slot < 16 ? ps->cb_vec4[b.slot] * 16u : 0u);
            } else if (b.space == 2 && b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b.slot < MAX_SRV) {
                View* v = stage_[PS].srv[b.slot];
                if (!v || !v->texture) { put(" t%u=null", b.slot); continue; }
                Texture* t = v->texture;
                static const std::string dump = property("debug.fp.rdvk_dump");
                char size[32], slot_size[40];
                snprintf(size, sizeof(size), "%ux%u", t->width, t->height);
                snprintf(slot_size, sizeof(slot_size), "t%u:%u:%ux%u", b.slot, t->dxgi, t->width, t->height);
                if (!dump_pending && (dump == size || dump == slot_size)) { dump_pending = t; add_ref(t); }
                put(" t%u=%p:%u:%ux%u:m%u:l%u:u%u:b%x:v%d/%d", b.slot, static_cast<void*>(t), v->dxgi, t->width, t->height, t->mips, t->layers,
                    t->usage, t->bind, int(v->view_type), int(b.view));
            }
        }
    }
    log("%s", line);
}

void Context::open_query(Query* q) {
    uint32_t s = take_slot();
    if (s == UINT32_MAX) return;
    q->slots.push_back(s);
    q->open = true;
    vk.vkCmdBeginQuery(cmd_, pool_of(s), s % QUERY_POOL_SIZE, gpu.caps.occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}

void Context::close_query(Query* q) {
    if (!q->open) return;
    uint32_t s = q->slots.back();
    vk.vkCmdEndQuery(cmd_, pool_of(s), s % QUERY_POOL_SIZE);
    q->open = false;
}

void Context::query_begin(Query* q) {
    ++query_stats[0];
    q->issued = false;
    q->serial = gpu.serial();
    if (!is_occlusion(q) || occlusion_mode() != 0) return;
    if (q->running) query_end(q);   /* Begin twice: the interval restarts */
    retire(q->slots, q->end_serial);
    q->running = true;
    active_queries_.push_back(q);
    if (in_pass_) open_query(q);
}

void Context::query_end(Query* q) {
    ++query_stats[1];
    q->serial = gpu.serial();
    q->issued = true;
    if (!q->running) return;
    close_query(q);
    q->running = false;
    q->end_serial = gpu.serial();
    active_queries_.erase(std::remove(active_queries_.begin(), active_queries_.end(), q), active_queries_.end());
}

int Context::query_samples(Query* q, uint64_t* samples) {
    ++query_stats[2];
    *samples = 0;
    if (!q->issued) { ++query_stats[4]; return 0; }
    if (q->serial >= gpu.serial()) flush();
    gpu.poll();
    if (gpu.completed() < q->serial) { ++query_stats[4]; return 0; }
    int mode = occlusion_mode();
    if (mode == 2) *samples = 1u << 20;
    if (mode == 0) {
        for (uint32_t s : q->slots) {
            uint64_t v = 0;
            VkResult r = vk.vkGetQueryPoolResults(gpu.device, pool_of(s), s % QUERY_POOL_SIZE, 1, sizeof(v), &v, sizeof(v), VK_QUERY_RESULT_64_BIT);
            if (r == VK_NOT_READY) { ++query_stats[4]; return 0; }
            *samples += v;
        }
    }
    ++query_stats[3];
    if (*samples) ++query_stats[5];
    return 1;
}

void Context::query_forget(Query* q) {
    if (q->running) {
        close_query(q);
        q->running = false;
        active_queries_.erase(std::remove(active_queries_.begin(), active_queries_.end(), q), active_queries_.end());
    }
    retire(q->slots, gpu.serial());
}

void Context::end_pass(uint8_t why) {
    if (in_pass_) ++pass_ends[why < PE_COUNT ? why : PE_OTHER];
    if (!in_pass_) {
        bool pending = false;
        for (const PendingClear& c : clear_) pending |= c.color || c.depth || c.stencil;
        if (!pending) { for (PendingClear& c : clear_) c.discard = false; return; }
        begin_pass();
    }
    for (Query* q : active_queries_) close_query(q);
    vk.vkCmdEndRendering(cmd_);
    in_pass_ = false;
}

void Context::clear_rtv(View* v, const float color[4]) {
    if (!v) return;
    if (tracing)
        log("RDVK_TRACE clear_rtv tex=%p dxgi=%u current=%d in_pass=%d color=%g,%g,%g,%g", static_cast<void*>(v->texture), v->texture->dxgi,
            int(std::any_of(rtv_, rtv_ + rt_count_, [&](View* r) { return r == v; })), int(in_pass_), double(color[0]), double(color[1]),
            double(color[2]), double(color[3]));
    for (uint32_t i = 0; i < rt_count_; ++i) {
        if (rtv_[i] != v) continue;
        if (!in_pass_) {   /* folded into the load op of the coming pass */
            clear_[i].color = true;
            memcpy(clear_[i].value.color.float32, color, 16);
            return;
        }
        VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, i, {}};
        memcpy(a.clearValue.color.float32, color, 16);
        VkClearRect rect{{{0, 0}, {v->width, v->height}}, 0, 1};
        vk.vkCmdClearAttachments(cmd_, 1, &a, 1, &rect);
        return;
    }
    /* Not a current target: clear the image outside of a pass. */
    end_pass(PE_CLEAR_RTV);
    cmd_ = gpu.cmd();
    transition_view(v, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barriers.flush(cmd_);
    VkClearColorValue value;
    memcpy(value.float32, color, 16);
    vk.vkCmdClearColorImage(cmd_, v->texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &v->range);
    use(v->texture);
}

void Context::clear_dsv(View* v, uint32_t flags, float depth, uint8_t stencil) {
    if (!v || !(flags & 3)) return;
    bool clear_depth = flags & 1, clear_stencil = (flags & 2) && (v->texture->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (tracing)
        log("RDVK_TRACE clear_dsv view=%p tex=%p current=%d in_pass=%d depth=%d(%g) stencil=%d(%u)", static_cast<void*>(v), static_cast<void*>(v->texture),
            int(v == dsv_), int(in_pass_), int(clear_depth), double(depth), int(clear_stencil), unsigned(stencil));
    if (v == dsv_) {
        if (!in_pass_) {
            PendingClear& c = clear_[MAX_RT];
            if (clear_depth) { c.depth = true; c.value.depthStencil.depth = depth; }
            if (clear_stencil) { c.stencil = true; c.value.depthStencil.stencil = stencil; }
            /* A pending clear of one aspect keeps the other's value. */
            return;
        }
        VkClearAttachment a{VkImageAspectFlags((clear_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (clear_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, {}};
        a.clearValue.depthStencil = {depth, stencil};
        VkClearRect rect{{{0, 0}, {v->width, v->height}}, 0, 1};
        if (a.aspectMask) vk.vkCmdClearAttachments(cmd_, 1, &a, 1, &rect);
        return;
    }
    end_pass(PE_CLEAR_DSV);
    cmd_ = gpu.cmd();
    transition_view(v, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barriers.flush(cmd_);
    VkClearDepthStencilValue value{depth, stencil};
    VkImageSubresourceRange range = v->range;
    range.aspectMask = (clear_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (clear_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    vk.vkCmdClearDepthStencilImage(cmd_, v->texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    use(v->texture);
}

void Context::clear_uav(View* v, const uint32_t values[4], bool floats) {
    if (!v) return;
    end_pass(PE_CLEAR_UAV);
    cmd_ = gpu.cmd();
    if (v->texture) {
        transition_view(v, VK_IMAGE_LAYOUT_GENERAL);
        barriers.flush(cmd_);
        VkClearColorValue value;
        memcpy(floats ? (void*)value.float32 : (void*)value.uint32, values, 16);
        vk.vkCmdClearColorImage(cmd_, v->texture->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &v->range);
        use(v->texture);
    } else if (v->buffer) {
        Buffer* b = v->buffer;
        vk.vkCmdFillBuffer(cmd_, b->buffer, b->offset + v->buffer_offset, v->buffer_size & ~VkDeviceSize(3), values[0]);
        barriers.src |= VK_PIPELINE_STAGE_TRANSFER_BIT; barriers.dst |= SHADERS;
        barriers.memory_src |= VK_ACCESS_TRANSFER_WRITE_BIT; barriers.memory_dst |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        use(b);
    }
}

/* ------------------------------------------------------------------ descriptors */
void Context::bind_descriptors(VkPipelineBindPoint point, const PipelineLayout* layout, const Shader* const* shaders, uint32_t count) {
    (void)shaders; (void)count;
    if (layout->entries.empty()) return;
    VkWriteDescriptorSet writes[64];
    VkDescriptorBufferInfo buffers[64];
    VkDescriptorImageInfo images[64];
    VkBufferView texels[64];
    uint32_t n = 0;
    bool reset = pushed_layout != layout || pushed_point != point || !layout->push;
    if (reset) { pushed_layout = layout; pushed_point = point; for (PushedEntry& p : pushed) p = PushedEntry{}; }
    size_t limit = std::min<size_t>(layout->entries.size(), 64);
    for (size_t i = 0; i < limit; ++i) {
        const LayoutEntry& e = layout->entries[i];
        VkWriteDescriptorSet& w = writes[n];
        w = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = e.binding; w.descriptorCount = 1; w.descriptorType = e.type;
        PushedEntry value{};
        const StageBindings& sb = stage_[e.stage];
        switch (e.space) {
        case 0: {  /* constant buffer */
            VkDescriptorBufferInfo& bi = buffers[n];
            if (e.icb) bi = VkDescriptorBufferInfo{e.icb->uniform, 0, VkDeviceSize((e.icb->uniform_data.size() + 15) & ~size_t(15))};
            else if (Buffer* b = e.slot < MAX_CB ? sb.cb[e.slot] : nullptr) {
                bi = VkDescriptorBufferInfo{b->buffer, b->offset, std::min<VkDeviceSize>(b->desc.ByteWidth, 65536)};
                use(b);
            } else bi = VkDescriptorBufferInfo{dummy_buffer, 0, 65536};
            w.pBufferInfo = &bi;
            value = PushedEntry{uint64_t(bi.buffer), bi.offset, bi.range};
            break;
        }
        case 1: {  /* sampler */
            VkDescriptorImageInfo& ii = images[n];
            Sampler* s = e.slot < MAX_SAMPLER ? sb.sampler[e.slot] : nullptr;
            ii = VkDescriptorImageInfo{s ? s->sampler : default_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w.pImageInfo = &ii;
            value = PushedEntry{uint64_t(ii.sampler), 0, 0};
            break;
        }
        case 2: case 3: {  /* SRV / UAV */
            View* v = e.space == 2 ? (e.slot < MAX_SRV ? sb.srv[e.slot] : nullptr) : (e.slot < MAX_UAV ? uav_[e.slot] : nullptr);
            if (e.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || e.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                VkDescriptorImageInfo& ii = images[n];
                VkImageView typed = v && v->texture && v->view ? view_of_type(v, e.view) : VK_NULL_HANDLE;
                bool ok = typed != VK_NULL_HANDLE;
                VkImageView none = ok ? VK_NULL_HANDLE : dummy_view(e.view);
                ii = VkDescriptorImageInfo{VK_NULL_HANDLE, ok ? typed : none,
                                           ok ? (e.space == 3 ? VK_IMAGE_LAYOUT_GENERAL : v->texture->layout(v->range.baseMipLevel, v->range.baseArrayLayer))
                                              : none ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED};
                if (ok) use(v->texture);
                w.pImageInfo = &ii;
                value = PushedEntry{uint64_t(ii.imageView), uint64_t(ii.imageLayout), 0};
            } else if (e.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || e.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                texels[n] = v && v->texel ? v->texel : dummy_texel();
                if (v && v->buffer) use(v->buffer);
                w.pTexelBufferView = &texels[n];
                value = PushedEntry{uint64_t(texels[n]), 0, 0};
            } else {  /* raw / structured buffer */
                VkDescriptorBufferInfo& bi = buffers[n];
                if (v && v->buffer) {
                    Buffer* b = v->buffer;
                    bi = VkDescriptorBufferInfo{b->buffer, b->offset + v->buffer_offset, v->buffer_size};
                    use(b);
                } else bi = VkDescriptorBufferInfo{dummy_buffer, 0, 65536};
                w.pBufferInfo = &bi;
                value = PushedEntry{uint64_t(bi.buffer), bi.offset, bi.range};
            }
            break;
        }
        default: {  /* UAV counters: not used by the engine */
            VkDescriptorBufferInfo& bi = buffers[n];
            bi = VkDescriptorBufferInfo{dummy_buffer, 0, 256};
            w.pBufferInfo = &bi;
            value = PushedEntry{uint64_t(dummy_buffer), 0, 256};
            break;
        }
        }
        if (!reset && pushed[i].a == value.a && pushed[i].b == value.b && pushed[i].c == value.c) continue;
        pushed[i] = value;
        ++n;
    }
    if (!n) return;
    if (layout->push) {
        vk.vkCmdPushDescriptorSetKHR(cmd_, point, layout->layout, 0, n, writes);
    } else {
        VkDescriptorSet set = allocate_set(layout->set_layout);
        for (uint32_t i = 0; i < n; ++i) writes[i].dstSet = set;
        vk.vkUpdateDescriptorSets(gpu.device, n, writes, 0, nullptr);
        vk.vkCmdBindDescriptorSets(cmd_, point, layout->layout, 0, 1, &set, 0, nullptr);
    }
}

/* ------------------------------------------------------------------ draws */
void Context::prepare_draw() {
    cmd_ = gpu.cmd();
    Shader* const graphics[5] = {shader_[VS], shader_[HS], shader_[DS], shader_[GS], shader_[PS]};
    /* Shader resources reach their sampling layout before the pass (or split it). */
    for (uint32_t s = 0; s < 5; ++s) {
        if (!graphics[s]) continue;
        const StageBindings& sb = stage_[s];
        for (const Shader::BindingInfo& b : graphics[s]->bindings) {
            if (b.space != 2 || b.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b.slot >= MAX_SRV) continue;
            View* v = sb.srv[b.slot];
            if (!v || !v->texture) continue;
            VkImageLayout want = srv_layout(v);
            if (srv_layout_matches(*v, want)) continue;
            if (in_pass_) {
                if (!gpu.caps.defer_attachment_transitions && dsv_ && dsv_->texture == v->texture && dsv_->read_only_depth) continue;
                static unsigned noted = 0;
                if (noted < 24 && ++noted)
                    log("RDVK_SPLIT_CAUSE stage=%u slot=%u tex=%p dxgi=%u %ux%u mips=%u layers=%u bind=0x%x layout=%d want=%d is_rt=%d is_ds=%d ps=%s",
                        s, b.slot, static_cast<void*>(v->texture), v->texture->dxgi, v->texture->width, v->texture->height, v->texture->mips,
                        v->texture->layers, v->texture->bind, int(v->texture->layout(v->range.baseMipLevel, v->range.baseArrayLayer)), int(want),
                        int(std::any_of(rtv_, rtv_ + rt_count_, [&](View* r) { return r && r->texture == v->texture; })),
                        int(dsv_ && dsv_->texture == v->texture), shader_[PS] ? shader_[PS]->name : "-");
                end_pass(PE_SPLIT);
                dirty_ |= D_ALL;
            }
            transition_view(v, want);
        }
    }
    if (!in_pass_) begin_pass();
    else barriers.flush(cmd_);

    uint32_t klass = 2, points = 0;
    VkPrimitiveTopology topology = vk_topology(topology_, &klass, &points);
    if ((dirty_ & D_LAYOUT) || !graphics_layout_) { graphics_layout_ = layout_for(graphics, 5, false); dirty_ &= ~D_LAYOUT; }
    const PipelineLayout* layout = graphics_layout_;
    if (dirty_ & (D_PIPELINE | D_TOPOLOGY)) {
        GraphicsKey key;
        for (int i = 0; i < 5; ++i) key.shader[i] = graphics[i];
        key.input = layout_ ? layout_->key : 0;
        key.blend = blend_ ? blend_->key : 0;
        key.color_count = rt_count_;
        for (uint32_t i = 0; i < rt_count_; ++i) key.color[i] = rtv_[i] ? rtv_[i]->format : VK_FORMAT_UNDEFINED;
        key.depth = dsv_ ? dsv_->format : VK_FORMAT_UNDEFINED;
        key.stencil = dsv_ && (dsv_->texture->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
        key.samples = rt_count_ && rtv_[0] ? rtv_[0]->texture->samples : dsv_ ? dsv_->texture->samples : 1;
        key.topology_class = shader_[HS] ? 3 : klass;
        key.patch_points = points;
        key.sample_mask = sample_mask_;
        key.depth_clamp = 1;
        key.depth_clip = raster_ ? raster_->desc.DepthClipEnable : 1;
        VkPipeline pipeline = graphics_pipeline(key, layout, layout_, blend_);
        if (pipeline != bound_pipeline_) {
            if (pipeline) vk.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound_pipeline_ = pipeline;
            if (pipeline) {
                uint32_t samples = key.samples;
                vk.vkCmdPushConstants(cmd_, layout->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &samples);
            }
        }
        dirty_ &= ~D_PIPELINE;
    }
    if (dirty_ & D_TOPOLOGY) {
        if (dyn.topology != topology) { vk.vkCmdSetPrimitiveTopology(cmd_, topology); dyn.topology = topology; }
        dirty_ &= ~D_TOPOLOGY;
    }
    if (dirty_ & D_VIEWPORT) {
        VkViewport vp{viewport_.TopLeftX, viewport_.TopLeftY + viewport_.Height, viewport_.Width, -viewport_.Height,
                      std::clamp(viewport_.MinDepth, 0.0f, 1.0f), std::clamp(viewport_.MaxDepth, 0.0f, 1.0f)};
        if (!viewport_set_ || viewport_.Width <= 0) {
            uint32_t w = rt_count_ && rtv_[0] ? rtv_[0]->width : dsv_ ? dsv_->width : 1;
            uint32_t h = rt_count_ && rtv_[0] ? rtv_[0]->height : dsv_ ? dsv_->height : 1;
            vp = VkViewport{0, float(h), float(w), -float(h), 0, 1};
        }
        if (!dyn.viewport_valid || memcmp(&vp, &dyn.viewport, sizeof(vp))) { vk.vkCmdSetViewportWithCount(cmd_, 1, &vp); dyn.viewport = vp; dyn.viewport_valid = true; }
        dirty_ &= ~D_VIEWPORT;
    }
    if (dirty_ & D_SCISSOR) {
        VkRect2D sc;
        if (raster_ && raster_->desc.ScissorEnable && scissor_set_) {
            int32_t x = std::max(0, scissor_.left), y = std::max(0, scissor_.top);
            sc = VkRect2D{{x, y}, {uint32_t(std::max(0, scissor_.right - x)), uint32_t(std::max(0, scissor_.bottom - y))}};
        } else sc = VkRect2D{{0, 0}, {16384, 16384}};
        if (!dyn.scissor_valid || memcmp(&sc, &dyn.scissor, sizeof(sc))) { vk.vkCmdSetScissorWithCount(cmd_, 1, &sc); dyn.scissor = sc; dyn.scissor_valid = true; }
        dirty_ &= ~D_SCISSOR;
    }
    if (dirty_ & D_RASTER) {
        const d3d::RASTERIZER_DESC* r = raster_ ? &raster_->desc : nullptr;
        VkCullModeFlags cull = !r ? VK_CULL_MODE_BACK_BIT : r->CullMode == 1 ? VK_CULL_MODE_NONE : r->CullMode == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
        VkFrontFace front = r && r->FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        if (cull != dyn.cull) { vk.vkCmdSetCullMode(cmd_, cull); dyn.cull = cull; }
        if (front != dyn.front) { vk.vkCmdSetFrontFace(cmd_, front); dyn.front = front; }
        float bias[3] = {r ? float(r->DepthBias) : 0.0f, r ? r->DepthBiasClamp : 0.0f, r ? r->SlopeScaledDepthBias : 0.0f};
        if (bias[1] != 0.0f && !gpu.caps.depth_bias_clamp) {
            /* depthBiasClamp is an optional feature (external-theories T02 h): unclamped bias. */
            if (first_time("depth_bias_clamp")) log("RDVK_FALLBACK depthBiasClamp unsupported: DepthBiasClamp %g ignored", double(bias[1]));
            bias[1] = 0.0f;
        }
        VkBool32 enable = bias[0] != 0.0f || bias[2] != 0.0f;
        if (enable != dyn.bias_enable) { vk.vkCmdSetDepthBiasEnable(cmd_, enable); dyn.bias_enable = enable; }
        if (memcmp(bias, dyn.bias, sizeof(bias))) { vk.vkCmdSetDepthBias(cmd_, bias[0], bias[1], bias[2]); memcpy(dyn.bias, bias, sizeof(bias)); }
        dirty_ &= ~D_RASTER;
    }
    if (dirty_ & D_DEPTH) {
        d3d::DEPTH_STENCIL_DESC def{1, 1, 2, 0, 0xFF, 0xFF, {}, {1, 1, 1, 8}, {1, 1, 1, 8}};
        const d3d::DEPTH_STENCIL_DESC& d = depth_ ? depth_->desc : def;
        VkBool32 test = dsv_ ? d.DepthEnable : VK_FALSE;
        VkBool32 write = dsv_ && d.DepthEnable && d.DepthWriteMask && !dsv_->read_only_depth;
        VkCompareOp op = compare_of(d.DepthFunc);
        VkBool32 stencil = dsv_ && d.StencilEnable && (dsv_->texture->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
        static const int debug_flags = atoi(property("debug.fp.rdvk_debug").c_str());
        if (debug_flags & 1) stencil = VK_FALSE;   /* diagnostics: debug.fp.rdvk_debug=1, no stencil test */
        if (test != dyn.depth_test) { vk.vkCmdSetDepthTestEnable(cmd_, test); dyn.depth_test = test; }
        if (write != dyn.depth_write) { vk.vkCmdSetDepthWriteEnable(cmd_, write); dyn.depth_write = write; }
        if (op != dyn.depth_op) { vk.vkCmdSetDepthCompareOp(cmd_, op); dyn.depth_op = op; }
        if (stencil != dyn.stencil_test) { vk.vkCmdSetStencilTestEnable(cmd_, stencil); dyn.stencil_test = stencil; }
        uint32_t hash = d.FrontFace.StencilFailOp | d.FrontFace.StencilDepthFailOp << 4 | d.FrontFace.StencilPassOp << 8 | d.FrontFace.StencilFunc << 12 |
                        d.BackFace.StencilFailOp << 16 | d.BackFace.StencilDepthFailOp << 20 | d.BackFace.StencilPassOp << 24 | d.BackFace.StencilFunc << 28;
        uint32_t masks = d.StencilReadMask | d.StencilWriteMask << 8;
        if (hash != dyn.stencil_hash || masks != (dyn.stencil_ref >> 8 & 0xFFFF) || stencil_ref_ != (dyn.stencil_ref & 0xFF)) {
            vk.vkCmdSetStencilOp(cmd_, VK_STENCIL_FACE_FRONT_BIT, stencil_op(d.FrontFace.StencilFailOp), stencil_op(d.FrontFace.StencilPassOp),
                                 stencil_op(d.FrontFace.StencilDepthFailOp), compare_of(d.FrontFace.StencilFunc));
            vk.vkCmdSetStencilOp(cmd_, VK_STENCIL_FACE_BACK_BIT, stencil_op(d.BackFace.StencilFailOp), stencil_op(d.BackFace.StencilPassOp),
                                 stencil_op(d.BackFace.StencilDepthFailOp), compare_of(d.BackFace.StencilFunc));
            vk.vkCmdSetStencilCompareMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, d.StencilReadMask);
            vk.vkCmdSetStencilWriteMask(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, d.StencilWriteMask);
            vk.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref_ & 0xFF);
            dyn.stencil_hash = hash; dyn.stencil_ref = (stencil_ref_ & 0xFF) | masks << 8;
        }
        dirty_ &= ~D_DEPTH;
    }
    if (dirty_ & D_BLEND) {
        if (memcmp(blend_factor_, dyn.blend, 16)) { vk.vkCmdSetBlendConstants(cmd_, blend_factor_); memcpy(dyn.blend, blend_factor_, 16); }
        dirty_ &= ~D_BLEND;
    }
    if (dirty_ & D_VB) {
        VkBuffer buffers[MAX_VB]; VkDeviceSize offsets[MAX_VB], strides[MAX_VB];
        uint32_t count = 0;
        for (uint32_t i = 0; i < MAX_VB; ++i) if (vb_[i].buffer) count = i + 1;
        if (layout_) for (const auto& b : layout_->bindings) count = std::max(count, b.binding + 1);
        count = std::min(count, MAX_VB);
        for (uint32_t i = 0; i < count; ++i) {
            if (Buffer* b = vb_[i].buffer) { buffers[i] = b->buffer; offsets[i] = b->offset + vb_[i].offset; strides[i] = vb_[i].stride; use(b); }
            else { buffers[i] = dummy_buffer; offsets[i] = 0; strides[i] = 0; }
        }
        if (count) vk.vkCmdBindVertexBuffers2(cmd_, 0, count, buffers, offsets, nullptr, strides);
        dirty_ &= ~D_VB;
    }
    if (dirty_ & D_IB) {
        if (ib_) { vk.vkCmdBindIndexBuffer(cmd_, ib_->buffer, ib_->offset + ib_offset_, ib_format_ == 42 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16); use(ib_); }
        dirty_ &= ~D_IB;
    }
    bind_descriptors(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, graphics, 5);
    dirty_ &= ~D_DESCRIPTORS;
}

void Context::draw(uint32_t count, uint32_t first) {
    if (!count || !shader_[VS]) return;
    if (skip_draw()) return;
    if (fp_skip_draw()) { if (tracing) trace_draw("draw", count, 1, true); return; }
    prepare_draw();
    if (tracing) trace_draw("draw", count, 1, false);
    if (bound_pipeline_) vk.vkCmdDraw(cmd_, count, 1, first, 0);
    ++draws;
    if (fp_lowres_) fp_finish_lowres();
}

void Context::draw_indexed(uint32_t count, uint32_t first, int32_t base, uint32_t instances) {
    if (!count || !instances || !shader_[VS] || !ib_) return;
    if (skip_draw()) return;
    if (fp_skip_draw()) { if (tracing) trace_draw("indexed", count, instances, true); return; }
    if (geometry::drawing.load(std::memory_order_relaxed)) count = geometry::draw(count, first, base);
    prepare_draw();
    if (tracing) trace_draw("indexed", count, instances, false);
    if (bound_pipeline_) vk.vkCmdDrawIndexed(cmd_, count, instances, first, base, 0);
    if (tracing) capture_after_draw();
    ++draws;
    if (fp_lowres_) fp_finish_lowres();
}

void Context::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    Shader* cs = shader_[CS];
    if (!cs || !x || !y || !z) return;
    if (tracing) log("RDVK_TRACE dispatch %ux%ux%u cs=%s", x, y, z, cs->name);
    fp_clamp_dispatch(x, y);
    end_pass(PE_DISPATCH);
    cmd_ = gpu.cmd();
    const StageBindings& sb = stage_[CS];
    for (const Shader::BindingInfo& b : cs->bindings) {
        if (b.space == 2 && b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b.slot < MAX_SRV && sb.srv[b.slot] && sb.srv[b.slot]->texture)
            transition_view(sb.srv[b.slot], srv_layout(sb.srv[b.slot]));
        if (b.space == 3 && b.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b.slot < MAX_UAV && uav_[b.slot] && uav_[b.slot]->texture)
            transition_view(uav_[b.slot], VK_IMAGE_LAYOUT_GENERAL);
    }
    barriers.flush(cmd_);
    const Shader* shaders[1] = {cs};
    const PipelineLayout* layout = layout_for(shaders, 1, true);
    VkPipeline pipeline = compute_pipeline(cs, layout);
    if (!pipeline) return;
    vk.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    bind_descriptors(VK_PIPELINE_BIND_POINT_COMPUTE, layout, shaders, 1);
    vk.vkCmdDispatch(cmd_, x, y, z);
    /* D3D11 orders every dispatch before later reads and writes of its outputs. */
    VkMemoryBarrier mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mem.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mem.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vk.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mem, 0, nullptr, 0, nullptr);
    /* Graphics state is unaffected, but descriptors pushed for compute replaced the graphics ones. */
    pushed_layout = nullptr;
    dirty_ |= D_DESCRIPTORS;
}

/* ------------------------------------------------------------------ buffers and maps */
/* A dynamic buffer of more than 1 MiB (the engine's user vertex/index buffers: 40 MiB, discarded
 * when they wrap) gets an allocation of its own for every DISCARD instead of a stream chunk of that
 * size; the old one is freed once the GPU used it. */
bool Context::discard_home(Buffer* b) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = (b->desc.ByteWidth + 15) & ~VkDeviceSize(15);
    info.usage = gpu.stream.usage();
    VkBuffer home = VK_NULL_HANDLE;
    if (vk.vkCreateBuffer(gpu.device, &info, nullptr, &home) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(gpu.device, home, &req);
    Allocation memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (!memory.memory || !memory.mapped || vk.vkBindBufferMemory(gpu.device, home, memory.memory, memory.offset) != VK_SUCCESS) {
        vk.vkDestroyBuffer(gpu.device, home, nullptr);
        if (memory.memory) gpu.memory.release(memory);
        return false;
    }
    gpu.stream.release(b->chunk, gpu.serial());
    release_home(b);
    b->home = home; b->home_memory = memory;
    b->buffer = home; b->offset = 0; b->mapped = memory.mapped; b->chunk = -1;
    ++b->discards;
    dirty_ |= D_DESCRIPTORS | D_VB | D_IB;
    return true;
}

uint8_t* Context::discard(Buffer* b, uint32_t bytes) {
    if (b->desc.ByteWidth > (1u << 20) && discard_home(b)) return b->mapped;
    Slice s = gpu.stream.allocate((std::max<uint32_t>(bytes, b->desc.ByteWidth) + 15) & ~uint32_t(15));
    gpu.stream.retain(s.chunk);
    gpu.stream.release(b->chunk, gpu.serial());
    if (b->home) release_home(b);
    ++b->discards;
    b->buffer = s.buffer; b->offset = s.offset; b->mapped = s.data; b->chunk = s.chunk;
    dirty_ |= D_DESCRIPTORS | D_VB | D_IB;
    return s.data;
}

/* Dynamic buffers whose contents outlived their stream chunk (mapped once, drawn for many frames)
 * keep the whole 8 MiB chunk from being reused.  Their contents move to an allocation of their own
 * (exact size, host visible) until the next DISCARD gives them a stream slice again; the chunk is
 * free once the frames recorded so far completed.  Runs at present, on the render thread. */
void Context::compact_stream() {
    std::vector<int32_t> pinned;
    gpu.stream.pinned(gpu.completed(), pinned);
    if (pinned.empty()) return;
    std::sort(pinned.begin(), pinned.end());
    uint32_t moved = 0;
    uint64_t bytes = 0;
    VkBufferUsageFlags usage = gpu.stream.usage();
    for_each_dynamic_buffer([&](Buffer* b) {
        if (b->chunk < 0 || !std::binary_search(pinned.begin(), pinned.end(), b->chunk)) return;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = (b->desc.ByteWidth + 15) & ~VkDeviceSize(15);
        info.usage = usage;
        VkBuffer home = VK_NULL_HANDLE;
        if (vk.vkCreateBuffer(gpu.device, &info, nullptr, &home) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(gpu.device, home, &req);
        Allocation memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        if (!memory.memory || !memory.mapped || vk.vkBindBufferMemory(gpu.device, home, memory.memory, memory.offset) != VK_SUCCESS) {
            vk.vkDestroyBuffer(gpu.device, home, nullptr);
            if (memory.memory) gpu.memory.release(memory);
            return;
        }
        memcpy(memory.mapped, b->mapped, b->desc.ByteWidth);   /* Allocation::mapped already points at the allocation */
        gpu.stream.release(b->chunk, gpu.serial());
        b->home = home; b->home_memory = memory;
        b->buffer = home; b->offset = 0; b->mapped = memory.mapped; b->chunk = -1;
        ++moved; bytes += info.size;
    });
    if (moved) dirty_ |= D_DESCRIPTORS | D_VB | D_IB;
    home_buffers_ += moved;
    home_bytes_ += bytes;
}

uint32_t Context::map(Resource* r, uint32_t sub, uint32_t type, uint32_t flags, d3d::MAPPED_SUBRESOURCE* out) {
    (void)flags;
    if (r->kind == Kind::Buffer) {
        auto* b = static_cast<Buffer*>(r);
        if (b->dynamic) {
            if (type == d3d::MAP_WRITE_DISCARD) discard(b, b->desc.ByteWidth);
            out->pData = reinterpret_cast<uint64_t>(b->mapped);
            out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
            return d3d::HR_S_OK;
        }
        if (b->staging) {
            if (type != d3d::MAP_WRITE) wait_for(b);
            out->pData = reinterpret_cast<uint64_t>(b->mapped);
            out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
            return d3d::HR_S_OK;
        }
        return d3d::HR_E_INVALIDARG;
    }
    auto* t = static_cast<Texture*>(r);
    if (sub >= t->mips * t->layers) return d3d::HR_E_INVALIDARG;
    if (t->readback && type == d3d::MAP_READ && sub == 0 && t->readback->issued) {
        ReadbackRing& ring = *t->readback;
        uint32_t index = ring.issued >= READBACK_RING ? ring.next : (ring.next + READBACK_RING - 1) % READBACK_RING;
        return map(ring.ring[index], 0, type, flags, out);
    }
    const Format& f = format(t->dxgi);
    uint32_t mip = sub % t->mips;
    if (t->usage == d3d::USAGE_STAGING) {
        if (type != d3d::MAP_WRITE && type != d3d::MAP_WRITE_DISCARD) wait_for(t);
        out->pData = reinterpret_cast<uint64_t>(t->linear_memory.mapped + t->sub_offset[sub]);
        out->RowPitch = t->sub_pitch[sub];
        out->DepthPitch = t->sub_pitch[sub] * row_count(f, t->sub_height(mip));
        if (tracing && type == d3d::MAP_READ) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(out->pData);
            size_t n = out->DepthPitch, nz = 0;
            for (size_t i = 0; i < n; ++i) nz += p[i] != 0;
            log("RDVK_TRACE map_read tex=%p dxgi=%u %ux%u sub=%u bytes=%zu nonzero=%zu", static_cast<void*>(t), t->dxgi, t->width, t->height, sub, n, nz);
        }
        return d3d::HR_S_OK;
    }
    if (t->usage == d3d::USAGE_DYNAMIC) {
        /* A dynamic texture: the CPU writes a stream slice, Unmap copies it into the image. */
        uint32_t pitch = row_bytes(f, t->sub_width(mip));
        VkDeviceSize bytes = VkDeviceSize(pitch) * row_count(f, t->sub_height(mip)) * t->sub_depth(mip);
        Slice s = gpu.stream.allocate(bytes, 16);
        gpu.stream.retain(s.chunk);
        if (t->sub_offset.size() != size_t(t->mips) * t->layers) { t->sub_offset.assign(size_t(t->mips) * t->layers, 0); t->sub_pitch.assign(size_t(t->mips) * t->layers, 0); }
        t->sub_offset[sub] = s.offset;
        t->sub_pitch[sub] = pitch;
        t->linear = s.buffer;
        t->linear_memory.mapped = s.data - s.offset;
        t->linear_memory.block = s.chunk;  /* the retained chunk */
        out->pData = reinterpret_cast<uint64_t>(s.data);
        out->RowPitch = pitch;
        out->DepthPitch = pitch * row_count(f, t->sub_height(mip));
        return d3d::HR_S_OK;
    }
    return d3d::HR_E_INVALIDARG;
}

void Context::unmap(Resource* r, uint32_t sub) {
    if (r->kind != Kind::Buffer) {
        auto* t = static_cast<Texture*>(r);
        if (t->usage == d3d::USAGE_DYNAMIC && t->linear) {
            end_pass(PE_UNMAP);
            cmd_ = gpu.cmd();
            uint32_t mip = sub % t->mips, layer = sub / t->mips;
            transition(t, VkImageSubresourceRange{t->aspect, mip, 1, layer, 1}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            barriers.flush(cmd_);
            const Format& f = format(t->dxgi);
            VkBufferImageCopy copy{};
            copy.bufferOffset = t->sub_offset[sub];
            copy.bufferRowLength = t->sub_pitch[sub] / f.bytes * f.block;
            copy.imageSubresource = VkImageSubresourceLayers{t->aspect, mip, layer, 1};
            copy.imageExtent = VkExtent3D{t->sub_width(mip), t->sub_height(mip), t->sub_depth(mip)};
            vk.vkCmdCopyBufferToImage(cmd_, t->linear, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition(t, VkImageSubresourceRange{t->aspect, mip, 1, layer, 1}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            gpu.stream.release(t->linear_memory.block, gpu.serial());
            t->linear = VK_NULL_HANDLE;
            use(t);
        }
    }
}

void Context::upload_buffer(Buffer* b, uint32_t offset, const void* data, uint32_t bytes, bool in_stream) {
    if (in_stream) {
        end_pass(PE_UPDATE_BUFFER);
        cmd_ = gpu.cmd();
        barriers.flush(cmd_);
        Slice s = gpu.stream.allocate(bytes, 16);
        memcpy(s.data, data, bytes);
        VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
        VkBufferCopy copy{s.offset, b->offset + offset, bytes};
        vk.vkCmdCopyBuffer(cmd_, s.buffer, b->buffer, 1, &copy);
        VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        use(b);
        return;
    }
    auto lock = gpu.upload_lock();
    Slice s = gpu.staging.allocate(bytes, 16);
    memcpy(s.data, data, bytes);
    gpu.upload_copy(s.buffer, b->buffer, VkBufferCopy{s.offset, b->offset + offset, bytes});
}

void Context::upload_texture_initial(Texture* t, const d3d::SUBRESOURCE_DATA* data) {
    const Format& f = format(t->dxgi);
    uint32_t count = t->mips * t->layers;
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t mip = i % t->mips;
        total += VkDeviceSize(row_bytes(f, t->sub_width(mip))) * row_count(f, t->sub_height(mip)) * t->sub_depth(mip);
        total = (total + 15) & ~VkDeviceSize(15);
    }
    auto lock = gpu.upload_lock();
    Slice s = gpu.staging.allocate(total, 16);
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t mip = i % t->mips, layer = i / t->mips;
        uint32_t pitch = row_bytes(f, t->sub_width(mip)), rows = row_count(f, t->sub_height(mip)), slices = t->sub_depth(mip);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(data[i].pSysMem);
        if (src) {
            uint32_t src_pitch = data[i].SysMemPitch ? data[i].SysMemPitch : pitch;
            uint32_t src_slice = data[i].SysMemSlicePitch ? data[i].SysMemSlicePitch : src_pitch * rows;
            for (uint32_t z = 0; z < slices; ++z)
                for (uint32_t y = 0; y < rows; ++y)
                    memcpy(s.data + at + (size_t(z) * rows + y) * pitch, src + size_t(z) * src_slice + size_t(y) * src_pitch, pitch);
            VkBufferImageCopy c{};
            c.bufferOffset = s.offset + at;
            c.imageSubresource = VkImageSubresourceLayers{t->aspect, mip, layer, 1};
            c.imageExtent = VkExtent3D{t->sub_width(mip), t->sub_height(mip), slices};
            copies.push_back(c);
        }
        at += VkDeviceSize(pitch) * rows * slices;
        at = (at + 15) & ~VkDeviceSize(15);
    }
    /* Recorded by the render thread at the next submit (Gpu::upload). */
    VkImage image = t->image;
    VkBuffer source = s.buffer;
    VkImageSubresourceRange whole{t->aspect, 0, t->mips, 0, t->layers};
    VkImageLayout final_layout = t->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    gpu.upload([image, source, whole, final_layout, copies = std::move(copies)](VkCommandBuffer cmd) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = whole;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        if (!copies.empty()) vk.vkCmdCopyBufferToImage(cmd, source, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = final_layout;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    std::fill(t->layouts.begin(), t->layouts.end(), final_layout);
}

void Context::update_subresource(Resource* r, uint32_t sub, const d3d::BOX* box, const void* data, uint32_t row_pitch, uint32_t depth_pitch) {
    if (!data) return;
    if (r->kind == Kind::Buffer) {
        auto* b = static_cast<Buffer*>(r);
        uint32_t offset = box ? box->left : 0, bytes = box ? box->right - box->left : b->desc.ByteWidth;
        if (!bytes) return;
        geometry::written(b);
        if (b->dynamic || b->staging) { memcpy(b->mapped + offset, data, bytes); return; }
        upload_buffer(b, offset, data, bytes, b->last_use >= gpu.serial());
        return;
    }
    auto* t = static_cast<Texture*>(r);
    if (sub >= t->mips * t->layers) return;
    const Format& f = format(t->dxgi);
    uint32_t mip = sub % t->mips, layer = sub / t->mips;
    uint32_t x = box ? box->left : 0, y = box ? box->top : 0, z = box ? box->front : 0;
    uint32_t w = box ? box->right - box->left : t->sub_width(mip), h = box ? box->bottom - box->top : t->sub_height(mip);
    uint32_t d = box ? box->back - box->front : t->sub_depth(mip);
    if (!w || !h || !d) return;
    if (t->usage == d3d::USAGE_STAGING) {
        uint32_t rows = row_count(f, h);
        for (uint32_t zz = 0; zz < d; ++zz)
            for (uint32_t yy = 0; yy < rows; ++yy)
                memcpy(t->linear_memory.mapped + t->sub_offset[sub] + (size_t(z + zz) * row_count(f, t->sub_height(mip)) + y / f.block + yy) * t->sub_pitch[sub] + x / f.block * f.bytes,
                       static_cast<const uint8_t*>(data) + size_t(zz) * depth_pitch + size_t(yy) * row_pitch, row_bytes(f, w));
        return;
    }
    uint32_t pitch = row_bytes(f, w), rows = row_count(f, h);
    VkDeviceSize bytes = VkDeviceSize(pitch) * rows * d;
    bool in_stream = t->last_use >= gpu.serial();
    std::unique_lock<std::mutex> lock;
    if (!in_stream) lock = gpu.upload_lock();
    Slice s = in_stream ? gpu.stream.allocate(bytes, 16) : gpu.staging.allocate(bytes, 16);
    for (uint32_t zz = 0; zz < d; ++zz)
        for (uint32_t yy = 0; yy < rows; ++yy)
            memcpy(s.data + (size_t(zz) * rows + yy) * pitch, static_cast<const uint8_t*>(data) + size_t(zz) * depth_pitch + size_t(yy) * row_pitch, pitch);
    VkBufferImageCopy copy{};
    copy.bufferOffset = s.offset;
    copy.imageSubresource = VkImageSubresourceLayers{t->aspect, mip, layer, 1};
    copy.imageOffset = VkOffset3D{int32_t(x), int32_t(y), int32_t(z)};
    copy.imageExtent = VkExtent3D{w, h, d};
    VkImageSubresourceRange range{t->aspect, mip, 1, layer, 1};
    VkImageLayout final_layout = t->layout(mip, layer);
    if (final_layout == VK_IMAGE_LAYOUT_UNDEFINED || final_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (in_stream) {
        end_pass(PE_UPDATE_TEXTURE);
        cmd_ = gpu.cmd();
        transition(t, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barriers.flush(cmd_);
        vk.vkCmdCopyBufferToImage(cmd_, s.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(t, range, final_layout);
        use(t);
    } else {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = t->layout(mip, layer); b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image; b.subresourceRange = range;
        VkBuffer source = s.buffer;
        gpu.upload([b, copy, source, final_layout](VkCommandBuffer cmd) {
            VkImageMemoryBarrier x = b;
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &x);
            vk.vkCmdCopyBufferToImage(cmd, source, x.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            x.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; x.newLayout = final_layout;
            x.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; x.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &x);
        });
        t->layouts[layer * t->mips + mip] = final_layout;
    }
}

void Context::copy_resource(Resource* dst, Resource* src) {
    if (!dst || !src) return;
    if (tracing && dst->kind != Kind::Buffer && src->kind != Kind::Buffer) {
        auto* d = static_cast<Texture*>(dst); auto* s = static_cast<Texture*>(src);
        log("RDVK_TRACE copy dst=%p:%u:%ux%u:u%u src=%p:%u:%ux%u:u%u", static_cast<void*>(d), d->dxgi, d->width, d->height, d->usage,
            static_cast<void*>(s), s->dxgi, s->width, s->height, s->usage);
    }
    end_pass(PE_COPY);
    cmd_ = gpu.cmd();
    if (dst->kind == Kind::Buffer && src->kind == Kind::Buffer) {
        auto* d = static_cast<Buffer*>(dst); auto* s = static_cast<Buffer*>(src);
        geometry::written(d);
        barriers.flush(cmd_);
        VkBufferCopy copy{s->offset, d->offset, std::min(d->desc.ByteWidth, s->desc.ByteWidth)};
        vk.vkCmdCopyBuffer(cmd_, s->buffer, d->buffer, 1, &copy);
        VkMemoryBarrier mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mem.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mem.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mem, 0, nullptr, 0, nullptr);
        use(d); use(s);
        return;
    }
    auto* d = static_cast<Texture*>(dst); auto* s = static_cast<Texture*>(src);
    if (!d->readback_slot && d->kind == Kind::Texture2D && d->usage == d3d::USAGE_STAGING && (d->cpu & d3d::CPU_ACCESS_READ) &&
        d->width * d->height <= 16 && d->mips == 1 && d->layers == 1 && d->samples == 1 && s->kind == Kind::Texture2D) {
        if (!d->readback) {
            d->readback = new ReadbackRing();
            d3d::TEXTURE2D_DESC desc{d->width, d->height, 1, 1, d->dxgi, {1, 0}, d3d::USAGE_STAGING, 0, d3d::CPU_ACCESS_READ, 0};
            for (Texture*& slot : d->readback->ring) {
                if (create_texture(2, &desc, nullptr, &slot) != d3d::HR_S_OK) stop("readback ring allocation");
                slot->readback_slot = true;
            }
        }
        ReadbackRing& ring = *d->readback;
        copy_resource(ring.ring[ring.next], src);
        ring.next = (ring.next + 1) % READBACK_RING;
        ++ring.issued;
        return;
    }
    uint32_t mips = std::min(d->mips, s->mips), layers = std::min(d->layers, s->layers);
    if (s->usage == d3d::USAGE_STAGING) return;  /* staging -> GPU copies are not used by the engine */
    transition(s, VkImageSubresourceRange{s->aspect, 0, mips, 0, layers}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (d->usage != d3d::USAGE_STAGING) transition(d, VkImageSubresourceRange{d->aspect, 0, mips, 0, layers}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barriers.flush(cmd_);
    if (d->usage == d3d::USAGE_STAGING) {
        std::vector<VkBufferImageCopy> copies;
        const Format& f = format(d->dxgi);
        for (uint32_t layer = 0; layer < layers; ++layer)
            for (uint32_t mip = 0; mip < mips; ++mip) {
                uint32_t sub = layer * d->mips + mip;
                VkBufferImageCopy c{};
                c.bufferOffset = d->sub_offset[sub];
                c.bufferRowLength = d->sub_pitch[sub] / f.bytes * f.block;
                c.imageSubresource = VkImageSubresourceLayers{s->aspect & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT), mip, layer, 1};
                c.imageExtent = VkExtent3D{s->sub_width(mip), s->sub_height(mip), s->sub_depth(mip)};
                copies.push_back(c);
            }
        vk.vkCmdCopyImageToBuffer(cmd_, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->linear, uint32_t(copies.size()), copies.data());
        VkMemoryBarrier mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mem.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mem.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mem, 0, nullptr, 0, nullptr);
    } else {
        std::vector<VkImageCopy> copies;
        for (uint32_t mip = 0; mip < mips; ++mip) {
            VkImageCopy c{};
            c.srcSubresource = VkImageSubresourceLayers{s->aspect, mip, 0, layers};
            c.dstSubresource = VkImageSubresourceLayers{d->aspect, mip, 0, layers};
            c.extent = VkExtent3D{d->sub_width(mip), d->sub_height(mip), d->sub_depth(mip)};
            copies.push_back(c);
        }
        vk.vkCmdCopyImage(cmd_, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    }
    use(d); use(s);
}

void Context::copy_region(Resource* dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, Resource* src, uint32_t src_sub, const d3d::BOX* box) {
    if (!dst || !src || dst->kind == Kind::Buffer || src->kind == Kind::Buffer) { if (dst && src) copy_resource(dst, src); return; }
    if (tracing) {
        auto* d = static_cast<Texture*>(dst); auto* s = static_cast<Texture*>(src);
        log("RDVK_TRACE copy_region dst=%p:%u:%ux%u:u%u sub%u at %u,%u src=%p:%u:%ux%u:u%u sub%u box=%d", static_cast<void*>(d), d->dxgi, d->width, d->height,
            d->usage, dst_sub, x, y, static_cast<void*>(s), s->dxgi, s->width, s->height, s->usage, src_sub, int(box != nullptr));
    }
    end_pass(PE_COPY_REGION);
    cmd_ = gpu.cmd();
    auto* d = static_cast<Texture*>(dst); auto* s = static_cast<Texture*>(src);
    uint32_t smip = src_sub % s->mips, slayer = src_sub / s->mips, dmip = dst_sub % d->mips, dlayer = dst_sub / d->mips;
    VkImageCopy c{};
    c.srcSubresource = VkImageSubresourceLayers{s->aspect, smip, slayer, 1};
    c.dstSubresource = VkImageSubresourceLayers{d->aspect, dmip, dlayer, 1};
    c.srcOffset = box ? VkOffset3D{int32_t(box->left), int32_t(box->top), int32_t(box->front)} : VkOffset3D{0, 0, 0};
    c.dstOffset = VkOffset3D{int32_t(x), int32_t(y), int32_t(z)};
    c.extent = box ? VkExtent3D{box->right - box->left, box->bottom - box->top, box->back - box->front}
                   : VkExtent3D{s->sub_width(smip), s->sub_height(smip), s->sub_depth(smip)};
    transition(s, VkImageSubresourceRange{s->aspect, smip, 1, slayer, 1}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(d, VkImageSubresourceRange{d->aspect, dmip, 1, dlayer, 1}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barriers.flush(cmd_);
    vk.vkCmdCopyImage(cmd_, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    use(d); use(s);
}

void Context::generate_mips(View* srv) {
    if (!srv || !srv->texture) return;
    Texture* t = srv->texture;
    end_pass(PE_MIPS);
    cmd_ = gpu.cmd();
    uint32_t base = srv->range.baseMipLevel, levels = srv->range.levelCount;
    uint32_t first_layer = srv->range.baseArrayLayer, layers = srv->range.layerCount;
    for (uint32_t m = base + 1; m < base + levels; ++m) {
        transition(t, VkImageSubresourceRange{t->aspect, m - 1, 1, first_layer, layers}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(t, VkImageSubresourceRange{t->aspect, m, 1, first_layer, layers}, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barriers.flush(cmd_);
        VkImageBlit blit{};
        blit.srcSubresource = VkImageSubresourceLayers{t->aspect, m - 1, first_layer, layers};
        blit.srcOffsets[1] = VkOffset3D{int32_t(t->sub_width(m - 1)), int32_t(t->sub_height(m - 1)), int32_t(t->sub_depth(m - 1))};
        blit.dstSubresource = VkImageSubresourceLayers{t->aspect, m, first_layer, layers};
        blit.dstOffsets[1] = VkOffset3D{int32_t(t->sub_width(m)), int32_t(t->sub_height(m)), int32_t(t->sub_depth(m))};
        vk.vkCmdBlitImage(cmd_, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    transition(t, VkImageSubresourceRange{t->aspect, base, levels, first_layer, layers}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    use(t);
}

/* CPU access to a resource the GPU may still use: commands still being recorded are submitted
 * through flush() (the context's command-buffer caches start over), then the CPU waits. */
void Context::wait_for(Resource* r) {
    end_pass(PE_WAIT);
    if (r->last_use >= gpu.serial()) flush();
    gpu.wait_serial(r->last_use);
}

void Context::flush() {
    end_pass(PE_FLUSH);
    barriers.flush(gpu.cmd());
    gpu.submit();
    cmd_ = gpu.cmd();
    reset_command_state();
}

void Context::barrier(const VkImageMemoryBarrier& b, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    barriers.images.push_back(b);
    barriers.src |= src;
    barriers.dst |= dst;
}
void Context::flush_barriers() { barriers.flush(gpu.cmd()); }
void Context::reset_command_state() {
    bound_pipeline_ = VK_NULL_HANDLE;
    pushed_layout = nullptr;
    dyn = DynamicCache{};
    dirty_ = D_ALL;
}

void Context::forget(Object* o) {
    for (auto& s : stage_) {
        for (auto& b : s.cb) if (b == o) b = nullptr;
        for (auto& v : s.srv) if (v == o) v = nullptr;
        for (auto& x : s.sampler) if (x == o) x = nullptr;
    }
    for (auto& v : uav_) if (v == o) v = nullptr;
    for (auto& v : vb_) if (v.buffer == o) v.buffer = nullptr;
    if (ib_ == o) ib_ = nullptr;
    for (auto& s : shader_) if (s == o) s = nullptr;
    if (o->kind >= Kind::VertexShader && o->kind <= Kind::ComputeShader) { layout_cache_.clear(); graphics_layout_ = nullptr; }
    if (layout_ == o) layout_ = nullptr;
    if (blend_ == o) blend_ = nullptr;
    if (depth_ == o) depth_ = nullptr;
    if (raster_ == o) raster_ = nullptr;
    bool target = dsv_ == o;
    for (auto& v : rtv_) target |= v == o;
    if (target) {
        end_pass(PE_FORGET);
        for (auto& v : rtv_) if (v == o) v = nullptr;
        if (dsv_ == o) dsv_ = nullptr;
    }
    if (o->kind == Kind::Texture1D || o->kind == Kind::Texture2D || o->kind == Kind::Texture3D)
        attachment_layouts_.erase(std::remove(attachment_layouts_.begin(), attachment_layouts_.end(), static_cast<Texture*>(o)), attachment_layouts_.end());
    dirty_ = D_ALL;
}
}  // namespace rdvk
