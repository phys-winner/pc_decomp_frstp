/* Mobile pass policy of the native Vulkan device.
 *
 * The policy of tools/dxvk_mobile_overlay.py (the D3D11/DXVK path; see its description) on the
 * device's own state.  Passes are recognised by the DXBC SHA-1 of their shaders (role of a Shader,
 * tagged at build time in the shader library: tools/build_shaderlib.py).  Frame order after the deferred lighting: tone map -> outline mask -> outline
 * dilate -> outline composite -> UI.  Always on: the depth min/max dispatch is clamped to the depth
 * extent (after one full grid per binding change), the opaque full-target tone map starts with
 * DONT_CARE instead of LOAD, and the outline dilate/composite are skipped when no mask draw
 * happened.  "Object highlight" off drops the outlines; "Mobile post-processing" runs the opaque
 * tone map at the scene resolution into a private target and scales it to the output with a
 * nearest-filter blit when the scene has at most 60 % of the output pixels.  debug.fp.fx overrides
 * the mode bits, debug.fp.fxlog=1 logs FP_FX counters every 300 frames.
 */
#include <cstdlib>
#include <cstring>
#include <string>
#include "context.h"
#include "rdvk.h"

namespace rdvk {
namespace {
enum : uint32_t {
    FpOutlinesOff = 1u << 0, FpClampMinMax = 1u << 1, FpDiscardTonemap = 1u << 2, FpSkipEmptyOutline = 1u << 3,
    FpLowResTonemap = 1u << 4,
    FpExact = FpClampMinMax | FpDiscardTonemap | FpSkipEmptyOutline, FpMobile = FpLowResTonemap,
};
struct Stats {
    uint32_t frames, mask_draws, outline_passes, outlines_skipped, discards, kept, dispatches, clamped;
    uint32_t grid_x, grid_y, clamp_x, clamp_y, depth_w, depth_h, lowres;
};
Stats stats{};
uint32_t polls = 0, override_mode = ~0u;
bool fxlog = false;

uint32_t policy() {
    if (!(polls++ % 30u)) {
        std::string value = property("debug.fp.fx");
        override_mode = value.empty() ? ~0u : uint32_t(strtoul(value.c_str(), nullptr, 0));
        fxlog = property("debug.fp.fxlog") == "1";
    }
    if (override_mode != ~0u) return override_mode;
    uint32_t outlines = host->object_highlight && !host->object_highlight() ? FpOutlinesOff : 0u;
    return FpExact | outlines | (host->mobile_effects && host->mobile_effects() ? FpMobile : 0u);
}
void frame_stats(uint32_t mode) {
    if (++stats.frames < 300u) return;
    if (fxlog)
        log("FP_FX frames=%u mode=0x%x mask_draws=%u outline_passes=%u outlines_skipped=%u tonemap_discards=%u tonemap_kept=%u "
            "tonemap_lowres=%u minmax=%u clamped=%u grid=%ux%u->%ux%u depth=%ux%u renderer=vulkan",
            stats.frames, mode, stats.mask_draws, stats.outline_passes, stats.outlines_skipped, stats.discards, stats.kept,
            stats.lowres, stats.dispatches, stats.clamped, stats.grid_x, stats.grid_y, stats.clamp_x, stats.clamp_y,
            stats.depth_w, stats.depth_h);
    stats = Stats{};
}
}  // namespace

/* Before a draw: true when the policy drops it. */
bool Context::fp_skip_draw() {
    uint32_t role = shader_[PS] ? shader_[PS]->role : ROLE_NONE;
    if (role == ROLE_TONEMAP || role == ROLE_TONEMAP_OPAQUE) {
        fp_mode_ = policy();
        fp_stage_ = 1;
        fp_mask_draws_ = 0;
        frame_stats(fp_mode_);
        if (role == ROLE_TONEMAP_OPAQUE) {
            bool full = fp_opaque_full_target();
            if ((fp_mode_ & FpLowResTonemap) && full && fp_begin_lowres()) stats.lowres += 1;
            else if ((fp_mode_ & FpDiscardTonemap) && full && !in_pass_) { clear_[0].discard = true; stats.discards += 1; }
            else stats.kept += 1;
        }
        return false;
    }
    /* The hover/selection mask follows the tone map: pass-through colour into an RGBA16F target
     * with the scene depth bound. */
    if (role == ROLE_OUTLINE_MASK && fp_stage_ == 1 && fp_outline_mask_target()) {
        fp_mask_draws_ += 1;
        stats.mask_draws += 1;
        return (fp_mode_ & FpOutlinesOff) != 0;
    }
    if (role == ROLE_OUTLINE_DILATE && fp_stage_ == 1) {
        fp_stage_ = 2;
        fp_skip_outline_ = (fp_mode_ & FpOutlinesOff) || ((fp_mode_ & FpSkipEmptyOutline) && !fp_mask_draws_);
        stats.outline_passes += 1;
        stats.outlines_skipped += fp_skip_outline_ ? 1 : 0;
        return fp_skip_outline_;
    }
    if (role == ROLE_OUTLINE_COMPOSITE && fp_stage_ == 2) {
        fp_stage_ = 0;
        return fp_skip_outline_;
    }
    fp_stage_ = 0;
    return false;
}

bool Context::fp_opaque_full_target() const {
    if (!rtv_[0] || dsv_) return false;
    for (uint32_t i = 1; i < MAX_RT; ++i)
        if (rtv_[i]) return false;
    const View* view = rtv_[0];
    if (!view->texture || view->range.layerCount != 1) return false;
    float w = float(view->width), h = float(view->height);
    if (!viewport_set_ || viewport_.TopLeftX > 0.0f || viewport_.TopLeftY > 0.0f || viewport_.TopLeftX + viewport_.Width < w ||
        viewport_.TopLeftY + viewport_.Height < h)
        return false;
    if (raster_ && raster_->desc.ScissorEnable &&
        (!scissor_set_ || scissor_.left > 0 || scissor_.top > 0 || scissor_.right < int32_t(view->width) || scissor_.bottom < int32_t(view->height)))
        return false;
    if (blend_) {
        const d3d::RENDER_TARGET_BLEND_DESC& rt = blend_->desc.RenderTarget[0];
        if (rt.BlendEnable || (rt.RenderTargetWriteMask & 0xf) != 0xf) return false;
    }
    return true;
}

bool Context::fp_outline_mask_target() const {
    return rtv_[0] && dsv_ && rtv_[0]->dxgi == 10;   /* DXGI_FORMAT_R16G16B16A16_FLOAT */
}

bool Context::fp_begin_lowres() {
    View* input = stage_[PS].srv[0];
    View* output = rtv_[0];
    if (!input || !input->texture || input->texture->kind != Kind::Texture2D || !output->texture) return false;
    uint32_t in_w = input->texture->sub_width(input->range.baseMipLevel), in_h = input->texture->sub_height(input->range.baseMipLevel);
    /* Worth it when the scene has at most 60 % of the output pixels (Render scale below 100 %). */
    if (uint64_t(in_w) * in_h * 5u > uint64_t(output->width) * output->height * 3u) return false;
    if (!fp_lowres_image_ || fp_lowres_image_->width != in_w || fp_lowres_image_->height != in_h ||
        fp_lowres_image_->dxgi != output->texture->dxgi || fp_lowres_view_->dxgi != output->dxgi) {
        if (fp_lowres_view_) { release(fp_lowres_view_); fp_lowres_view_ = nullptr; }
        if (fp_lowres_image_) { release(fp_lowres_image_); fp_lowres_image_ = nullptr; }
        /* Same format and view format as the output, so the tone-map pipeline is unchanged. */
        d3d::TEXTURE2D_DESC desc{in_w, in_h, 1, 1, output->texture->dxgi, {1, 0}, d3d::USAGE_DEFAULT, d3d::BIND_RENDER_TARGET, 0, 0};
        if (create_texture(2, &desc, nullptr, &fp_lowres_image_, output->format) != d3d::HR_S_OK) { fp_lowres_image_ = nullptr; return false; }
        d3d::RTV_DESC view{output->dxgi, d3d::RTV_TEXTURE2D, 0, 0, 0};
        if (create_rtv(fp_lowres_image_, &view, &fp_lowres_view_) != d3d::HR_S_OK) {
            release(fp_lowres_image_); fp_lowres_image_ = nullptr; fp_lowres_view_ = nullptr;
            return false;
        }
    }
    fp_saved_.count = rt_count_;
    for (uint32_t i = 0; i < MAX_RT; ++i) fp_saved_.rtv[i] = rtv_[i];
    fp_saved_.dsv = dsv_;
    fp_saved_.viewport = viewport_; fp_saved_.viewport_set = viewport_set_;
    fp_saved_.scissor = scissor_; fp_saved_.scissor_set = scissor_set_;
    View* targets[1] = {fp_lowres_view_};
    set_targets(1, targets, nullptr);
    clear_[0].discard = true;
    set_viewport(d3d::VIEWPORT{0.0f, 0.0f, float(in_w), float(in_h), viewport_.MinDepth, viewport_.MaxDepth});
    set_scissor(d3d::RECT{0, 0, int32_t(in_w), int32_t(in_h)});
    fp_lowres_out_ = output;
    fp_lowres_ = true;
    return true;
}

/* After the low-resolution tone map: back to the game's targets and viewport, then scale the
 * result onto the output (the game's own tone map magnifies the scene with point sampling). */
void Context::fp_finish_lowres() {
    fp_lowres_ = false;
    View* out = fp_lowres_out_;
    fp_lowres_out_ = nullptr;
    set_targets(fp_saved_.count, fp_saved_.rtv, fp_saved_.dsv);
    set_viewport(fp_saved_.viewport);
    set_scissor(fp_saved_.scissor);
    viewport_set_ = fp_saved_.viewport_set;
    scissor_set_ = fp_saved_.scissor_set;
    end_pass(PE_POLICY);
    cmd_ = gpu.cmd();
    Texture* src = fp_lowres_image_;
    Texture* dst = out->texture;
    transition(src, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(dst, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, out->range.baseMipLevel, 1, out->range.baseArrayLayer, 1},
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    flush_barriers();
    VkImageBlit region{};
    region.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = VkOffset3D{int32_t(src->width), int32_t(src->height), 1};
    region.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, out->range.baseMipLevel, out->range.baseArrayLayer, 1};
    region.dstOffsets[1] = VkOffset3D{int32_t(out->width), int32_t(out->height), 1};
    vk.vkCmdBlitImage(cmd_, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                      VK_FILTER_NEAREST);
    use(src); use(dst);
}

/* Depth min/max reduction for shadow-cascade fitting: 16x16 threads per group.  Groups past the
 * depth extent only write a constant result; after one full grid for each depth/target/grid
 * combination those texels hold it, so later frames skip them. */
void Context::fp_clamp_dispatch(uint32_t& x, uint32_t& y) {
    Shader* cs = shader_[CS];
    if (!cs || cs->role != ROLE_DEPTH_MINMAX) return;
    stats.dispatches += 1;
    stats.grid_x = x; stats.grid_y = y;
    if (!(fp_mode_ & FpClampMinMax)) return;
    View* depth = stage_[CS].srv[10];
    View* target = uav_[0];
    if (!depth || !target || !depth->texture || depth->texture->kind != Kind::Texture2D || !target->texture) return;
    uint32_t w = depth->texture->sub_width(depth->range.baseMipLevel), h = depth->texture->sub_height(depth->range.baseMipLevel);
    stats.depth_w = w; stats.depth_h = h;
    uint64_t key = uint64_t(uintptr_t(depth->texture));
    key = key * 1000003u ^ uint64_t(uintptr_t(target->texture));
    key = key * 1000003u ^ (uint64_t(w) << 32 | h);
    key = key * 1000003u ^ (uint64_t(x) << 32 | y);
    if (key != fp_minmax_key_) { fp_minmax_key_ = key; return; }
    x = std::min(x, (w + 15u) / 16u);
    y = std::min(y, (h + 15u) / 16u);
    stats.clamped += 1;
    stats.clamp_x = x; stats.clamp_y = y;
}
}  // namespace rdvk
