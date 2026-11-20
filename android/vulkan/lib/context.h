/* The rendering context of the native Vulkan device: pipeline state, resource bindings, render
 * passes (dynamic rendering, clears folded into load ops), layout transitions, draws, dispatches,
 * uploads and maps.  Called directly by the native engine functions (engine.cpp); one recording
 * thread (the engine's renderer), uploads also from loader threads. */
#pragma once
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include "objects.h"

namespace rdvk {

enum Stage : uint32_t { VS = 0, HS = 1, DS = 2, GS = 3, PS = 4, CS = 5, STAGES = 6 };
constexpr uint32_t MAX_CB = 15, MAX_SRV = 32, MAX_SAMPLER = 16, MAX_UAV = 8, MAX_VB = 8, MAX_RT = 8;

struct StageBindings {
    Buffer* cb[MAX_CB] = {};
    View* srv[MAX_SRV] = {};
    Sampler* sampler[MAX_SAMPLER] = {};
};

struct PipelineLayout;
struct SwapChainObject;
void pipelines_forget_shader(Shader* shader);
void swapchain_destroy(Object* o);

class Context {
public:
    void init();
    /* ---- state */
    void set_shader(Stage stage, Shader* shader);
    void set_constant_buffer(Stage stage, uint32_t slot, Buffer* buffer);
    void set_srv(Stage stage, uint32_t slot, View* view);
    void set_sampler(Stage stage, uint32_t slot, Sampler* sampler);
    void set_uav(uint32_t slot, View* view);
    void set_vertex_buffer(uint32_t slot, Buffer* buffer, uint32_t offset, uint32_t stride);
    void set_index_buffer(Buffer* buffer, uint32_t dxgi, uint32_t offset);
    void set_input_layout(InputLayout* layout);
    void set_topology(uint32_t d3d_topology);
    void set_blend(BlendState* state, const float factor[4], uint32_t mask);
    void set_depth(DepthState* state, uint32_t ref);
    void set_raster(RasterState* state);
    void set_viewport(const d3d::VIEWPORT& viewport);
    void set_scissor(const d3d::RECT& rect);
    void set_targets(uint32_t count, View* const* rtvs, View* dsv);
    /* ---- commands */
    void clear_rtv(View* rtv, const float color[4]);
    void clear_dsv(View* dsv, uint32_t flags, float depth, uint8_t stencil);
    void clear_uav(View* uav, const uint32_t values[4], bool floats);
    void draw(uint32_t count, uint32_t first);
    void draw_indexed(uint32_t count, uint32_t first, int32_t base, uint32_t instances = 1);
    void dispatch(uint32_t x, uint32_t y, uint32_t z);
    void copy_resource(Resource* dst, Resource* src);
    void copy_region(Resource* dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z, Resource* src, uint32_t src_sub, const d3d::BOX* box);
    void generate_mips(View* srv);
    void update_subresource(Resource* r, uint32_t sub, const d3d::BOX* box, const void* data, uint32_t row_pitch, uint32_t depth_pitch);
    uint32_t map(Resource* r, uint32_t sub, uint32_t type, uint32_t flags, d3d::MAPPED_SUBRESOURCE* out);
    void unmap(Resource* r, uint32_t sub);
    /* Fast path of the engine's constant/vertex buffer updates: a new slice with the data. */
    uint8_t* discard(Buffer* b, uint32_t bytes);
    void flush();
    void wait_for(Resource* r);
    void compact_stream();
    bool discard_home(Buffer* b);
    uint64_t home_buffers_ = 0, home_bytes_ = 0;   /* moved by compact_stream (cumulative) */
    /* ---- mobile pass policy (policy.cpp) */
    bool fp_skip_draw();
    void fp_clamp_dispatch(uint32_t& x, uint32_t& y);
    void present(SwapChainObject* swapchain, uint32_t interval);
    /* Raw barriers for the presenter; reset of the per-command-buffer caches after a submission. */
    void barrier(const VkImageMemoryBarrier& b, VkPipelineStageFlags src, VkPipelineStageFlags dst);
    void flush_barriers();
    void reset_command_state();
    /* ---- uploads (any thread) */
    void upload_texture_initial(Texture* t, const d3d::SUBRESOURCE_DATA* data);
    void upload_buffer(Buffer* b, uint32_t offset, const void* data, uint32_t bytes, bool in_stream);
    Slice stream_slice(VkDeviceSize bytes);
    /* An object is being destroyed: drop it from the bindings. */
    void forget(Object* o);
    /* ---- current input assembler bindings (saved/restored by the engine) */
    uint32_t vertex_stride(uint32_t slot) const { return slot < MAX_VB ? vb_[slot].stride : 0; }
    uint32_t index_format() const { return ib_format_; }
    Buffer* index_buffer() const { return ib_; }
    uint32_t index_offset() const { return ib_offset_; }
    InputLayout* input_layout() const { return layout_; }
    uint32_t topology() const { return topology_; }
    void vertex_binding(uint32_t slot, Buffer** buffer, uint32_t* offset, uint32_t* stride) const {
        const VertexBinding& b = vb_[slot < MAX_VB ? slot : 0];
        *buffer = slot < MAX_VB ? b.buffer : nullptr; *offset = b.offset; *stride = b.stride;
    }
    /* A world pass (mobile geometry): the G-buffer (two or more colour targets) or depth only. */
    bool world_pass() const {
        uint32_t colour = 0;
        for (uint32_t i = 0; i < rt_count_ && i < MAX_RT; ++i) colour += rtv_[i] != nullptr;
        return colour >= 2 || (colour == 0 && dsv_);
    }
    /* ---- statistics */
    uint64_t draws = 0, passes = 0, pipelines_created = 0, frames = 0;
    /* Why render passes ended (index = PassEnd), for RDVK_PASSES. */
    enum PassEnd : uint8_t { PE_OTHER, PE_TARGETS, PE_CLEAR_RTV, PE_CLEAR_DSV, PE_CLEAR_UAV, PE_SPLIT, PE_DISPATCH, PE_UNMAP,
                             PE_UPDATE_BUFFER, PE_UPDATE_TEXTURE, PE_COPY, PE_COPY_REGION, PE_MIPS, PE_WAIT, PE_FLUSH, PE_FORGET,
                             PE_POLICY, PE_PRESENT, PE_COUNT };
    uint64_t pass_ends[PE_COUNT] = {};
    /* Occlusion queries.  A D3D11 Begin/End may span render passes, a Vulkan query may not: one
     * Vulkan query per render pass of the interval, summed by GetData (as DXVK does).  The first
     * native device answered every occlusion query with 0 samples, so content the engine culls by
     * occlusion (the HUD's scout portraits on the Mali tablet) vanished whenever the answer was
     * already complete.  debug.fp.rdvk_occlusion=stub|visible: old answer / always visible. */
    void query_begin(Query* q);
    void query_end(Query* q);
    int query_samples(Query* q, uint64_t* samples);   /* 1 ready, 0 not yet */
    void query_forget(Query* q);
    uint64_t query_stats[6] = {};   /* begins, ends, getdata, ready, not ready, ready and visible */
    /* One-frame draw trace (debug.fp.rdvk_trace=N: each new N traces the next frame). */
    bool tracing = false;
    uint32_t trace_left_ = 0;   /* debug.fp.rdvk_trace=N with N>1: N consecutive frames */
    Texture* dump_pending = nullptr;
    Texture* capture_copy_ = nullptr;   /* debug.fp.rdvk_capture=FI: rt0 right after draw FI of the traced frame */
    void capture_after_draw();
    uint64_t frame_draw_ = 0, skip_lo_ = 0, skip_hi_ = 0;   /* debug.fp.rdvk_skip=A-B: skip draws A..B-1 of each frame */
    bool skip_draw() { uint64_t i = frame_draw_++; return i >= skip_lo_ && i < skip_hi_; }   /* debug.fp.rdvk_debug bit 1: draws of the 1000x1334 texture opaque */   /* debug.fp.rdvk_dump=WxH: first such texture of the traced frame */
    void trace_draw(const char* kind, uint32_t count, uint32_t instances, bool skipped);
    void trace_poll();

private:
    friend struct PipelineCache;
    void begin_pass();
    void end_pass(uint8_t why = PE_OTHER);
    void open_query(Query* q);
    void close_query(Query* q);
    std::vector<Query*> active_queries_;
    void prepare_draw();
    void prepare_dispatch();
    void bind_descriptors(VkPipelineBindPoint point, const PipelineLayout* layout, const Shader* const* shaders, uint32_t count);
    void transition(Texture* t, const VkImageSubresourceRange& range, VkImageLayout layout, bool rebind = false);
    void transition_view(View* v, VkImageLayout layout);
    void sweep_attachments();
    VkImageLayout srv_layout(const View* v) const;
    void use(Resource* r) { r->last_use = gpu.serial(); }
    const PipelineLayout* layout_for(const Shader* const* shaders, uint32_t count, bool compute);
    struct LayoutKey {
        const Shader* shader[5]; bool compute;
        bool operator==(const LayoutKey& o) const { return !memcmp(shader, o.shader, sizeof(shader)) && compute == o.compute; }
    };
    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& k) const {
            uint64_t h = k.compute ? 7 : 3;
            for (const Shader* s : k.shader) { h ^= uint64_t(uintptr_t(s)); h *= 1099511628211ull; }
            return size_t(h ^ (h >> 29));
        }
    };
    std::unordered_map<LayoutKey, const PipelineLayout*, LayoutKeyHash> layout_cache_;
    bool fp_opaque_full_target() const;
    bool fp_outline_mask_target() const;
    bool fp_begin_lowres();
    void fp_finish_lowres();
    uint32_t fp_mode_ = 0, fp_stage_ = 0, fp_mask_draws_ = 0;
    bool fp_skip_outline_ = false, fp_lowres_ = false;
    Texture* fp_lowres_image_ = nullptr;
    View* fp_lowres_view_ = nullptr;
    View* fp_lowres_out_ = nullptr;
    struct {
        uint32_t count = 0; View* rtv[MAX_RT] = {}; View* dsv = nullptr;
        d3d::VIEWPORT viewport{}; bool viewport_set = false; d3d::RECT scissor{}; bool scissor_set = false;
    } fp_saved_;
    uint64_t fp_minmax_key_ = 0;
    const PipelineLayout* graphics_layout_ = nullptr;

    /* pipeline state */
    Shader* shader_[STAGES] = {};
    InputLayout* layout_ = nullptr;
    BlendState* blend_ = nullptr;
    float blend_factor_[4] = {1, 1, 1, 1};
    uint32_t sample_mask_ = 0xFFFFFFFFu;
    DepthState* depth_ = nullptr;
    uint32_t stencil_ref_ = 0;
    RasterState* raster_ = nullptr;
    uint32_t topology_ = 4;
    d3d::VIEWPORT viewport_{};
    d3d::RECT scissor_{};
    bool viewport_set_ = false, scissor_set_ = false;
    StageBindings stage_[STAGES];
    View* uav_[MAX_UAV] = {};
    struct VertexBinding { Buffer* buffer = nullptr; uint32_t offset = 0, stride = 0; } vb_[MAX_VB];
    Buffer* ib_ = nullptr; uint32_t ib_format_ = 57, ib_offset_ = 0;

    /* render targets */
    View* rtv_[MAX_RT] = {};
    View* dsv_ = nullptr;
    uint32_t rt_count_ = 0;
    bool in_pass_ = false;
    struct PendingClear { bool color = false, depth = false, stencil = false, discard = false; VkClearValue value{}; };
    PendingClear clear_[MAX_RT + 1];
    std::vector<Texture*> attachment_layouts_;   /* textures left in attachment layout */

    /* dynamic state already set in the current command buffer */
    VkPipeline bound_pipeline_ = VK_NULL_HANDLE;
    uint64_t dirty_ = ~0ull;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSampler default_sampler_ = VK_NULL_HANDLE;
};

extern Context g_ctx;
VkCompareOp compare_of(uint32_t func);
}  // namespace rdvk
