/* The hot path of the Liquid Engine's RenderingDeviceDirectX11 backend, written natively over the
 * Vulkan context.  Each function keeps the engine-side state of the original (binding caches,
 * statistics, the fields other engine code reads) exactly and replaces its D3D11 calls with
 * direct calls of rdvk::Context.  Functions not listed in install() keep their translated body;
 * their D3D11 calls (resource creation at load time, rare paths) reach com.cpp.
 *
 * Device object (RenderingDeviceDirectX11Base, `dev`):
 *   +8 scissor flag   +16 user VB (RD)  +24 user VB offset  +28 capacity
 *   +32 user IB offset +36 capacity      +40 user IB (RD)
 *   +320 user vertex data  +328 user index data  +336 user vertex stride
 *   +600 default index buffer (RD)  +616 depth state (RD)  +632 resource support
 *   +640 cull  +644 depth-bias flag  +648 wireframe flag  +656 stencil ref  +660 max samplers
 *   +664 primitives  +668 draw calls  +680 GPU tick period  +688 vsync
 *   +800.. raster states [100 + 8*cull + 4*scissor + 2*bias + wire]
 *   +1144/+1152 VB0/VB1 cache  +1160 IB cache  +1168/+1172/+1176 offsets  +1180 topology
 *   +1184 bound stage bits  +1192..+1216 RTV[4]  +1224 DSV
 *   +1232 texture binding cache [3][16] x 32 bytes  +2768 SRV cache [6][16]  +3536 CS UAV cache [8]
 *   +3600 saved IA state
 */
#include <cstring>
#include "context.h"
#include "geometry.h"
#include "pipeline.h"
#include "present.h"
extern "C" void port_adpf_end(void) __attribute__((weak));
extern "C" void port_adpf_begin(double fps) __attribute__((weak));
extern "C" uint32_t frostpunk_android_frame_limit(void) __attribute__((weak));
#include "stability.h"
#include "rdvk.h"

namespace rdvk {
namespace {
constexpr uint64_t PP_CONTEXT = 0x14400F178ull, PP_DEVICE = 0x14400F180ull, DEVICE_OBJECT = 0x14400F170ull;
constexpr uint64_t TOPOLOGY_TABLE = 0x1428A3F70ull;
constexpr uint64_t SAMPLER_LOOKUP = 0x140F4AF20ull;   /* RenderingDevice: sampler state of a key (engine cache) */
constexpr uint64_t BASE_FRAME_BEGIN = 0x140F4A0D0ull; /* RenderingDevice::BeginFrame (engine part) */
constexpr uint64_t DEV_INIT_BASE = 0x140D88720ull;    /* RenderingDeviceDirectX11Base::Init (translated, cold) */

template <class T> T* object(uint64_t guest) { return guest ? static_cast<T*>(object_of(guest)) : nullptr; }
Buffer* rd_buffer(uint64_t rd, unsigned offset) { return rd ? object<Buffer>(rd64(rd + offset)) : nullptr; }

uint32_t index_count(uint32_t type, uint32_t prims) {
    if (!prims) return 0;
    switch (type) {
    case 0: case 1: return prims + 2;
    case 2: case 6: return 3 * prims;
    case 3: return prims + 1;
    case 4: return 2 * prims;
    case 5: return prims;
    case 7: return 4 * prims;
    default: return 0;
    }
}
void set_topology(uint64_t dev, uint32_t type) {
    uint32_t t = rd32(TOPOLOGY_TABLE + 4 * uint64_t(type));
    if (rd32(dev + 1180) != t) { g_ctx.set_topology(t); wr32(dev + 1180, t); }
}
void apply_raster(uint64_t dev) {
    uint64_t index = 100 + 8 * uint64_t(int8_t(rd8(dev + 640))) + 4 * uint64_t(rd32(dev + 8)) + 2 * uint64_t(rd32(dev + 644)) + uint64_t(int32_t(rd32(dev + 648)));
    g_ctx.set_raster(object<RasterState>(rd64(dev + 8 * index)));
}

/* ---------------------------------------------------------------- frame */
void dev_init(CPU* c) {
    uint64_t dev = c->rcx;
    static bool ready = false;
    if (!ready) {
        renderer_qa_start();
        if (!gpu.init()) {
            guard_unsupported();
            renderer_stop(false, "Vulkan device initialisation failed: %s", gpu_failure_note());
        }
        objects_init();
        g_ctx.init();
        pipeline_cache_init();
        geometry::init();
        ready = true;
    }
    wr64(PP_DEVICE, guest_of(g_device));
    wr64(PP_CONTEXT, guest_of(g_context));
    log("RDVK_DEVICE ready device=%s feature_level=11_0", gpu.caps.name);
    guest_call(c, DEV_INIT_BASE, dev);
    ret(c, 1);
}
void dev_frame_begin_query(CPU* c) { uint64_t r = guest_call(c, BASE_FRAME_BEGIN, c->rcx); ret(c, r); }
void dev_frame_end_query(CPU* c) {
    uint64_t dev = c->rcx;
    if (rd64(dev + 1120)) { float period = gpu.caps.timestamp_period * 1e-9f; wr<float>(dev + 680, period); }
    ret(c);
}
void dev_present(CPU* c) {
    if (port_adpf_end) port_adpf_end();
    uint64_t dev = c->rcx, window = c->rdx;
    for (int i = 0; i < 5; ++i) wr64(dev + 1192 + 8 * i, 0);
    g_ctx.set_targets(0, nullptr, nullptr);
    auto* swapchain = window ? reinterpret_cast<SwapChainObject*>(object_of(rd64(window + 64))) : nullptr;
    g_ctx.present(swapchain, rd8(dev + 688));
    ret(c, 1);
    if (port_adpf_begin) port_adpf_begin(frostpunk_android_frame_limit ? frostpunk_android_frame_limit() : 60);
}

/* ---------------------------------------------------------------- render targets */
void dev_commit_render_targets(CPU* c) {
    uint64_t dev = c->rcx;
    uint32_t count = uint32_t(c->rdx);
    View* rtvs[4] = {};
    for (uint32_t i = 0; i < 4 && i < count; ++i) rtvs[i] = object<View>(rd64(dev + 1192 + 8 * i));
    g_ctx.set_targets(count > 4 ? 4 : count, rtvs, object<View>(rd64(dev + 1224)));
    ret(c);
}
void rt_clear(CPU* c) {
    uint64_t rt = c->rcx;
    const float* color = reinterpret_cast<const float*>(c->rdx);
    uint64_t which = c->r8;
    uint64_t views = rd64(rt + 48);
    if (which & 0xFF) g_ctx.clear_rtv(object<View>(rd64(views + 8 * uint64_t(int32_t(which >> 32)))), color);
    else {
        int32_t n = int32_t(rd32(rt + 56));
        for (int32_t i = 0; i < n; ++i) g_ctx.clear_rtv(object<View>(rd64(views + 8 * uint64_t(i))), color);
    }
    ret(c);
}
void ds_clear(CPU* c) {
    uint64_t ds = c->rcx;
    uint32_t engine = uint32_t(int32_t(int8_t(c->rdx)));
    uint32_t flags = ((engine >> 1) & 1) | (((engine >> 2) & 1) << 1);
    float depth; memcpy(&depth, &c->xmm[2], 4);
    g_ctx.clear_dsv(object<View>(rd64(ds + 32)), flags, depth, uint8_t(c->r9));
    ret(c);
}

/* ---------------------------------------------------------------- rasterizer / output merger */
void dev_set_viewport(CPU* c) {
    const uint32_t* pos = reinterpret_cast<const uint32_t*>(c->rdx);
    const uint32_t* size = reinterpret_cast<const uint32_t*>(c->r8);
    d3d::VIEWPORT v{float(pos[0]), float(pos[1]), float(size[0]), float(size[1]), 0, 1};
    v.MinDepth = argf(c, 3);   /* arguments 4 and 5: xmm3 and the stack */
    v.MaxDepth = argf(c, 4);
    g_ctx.set_viewport(v);
    ret(c);
}
void dev_set_scissor(CPU* c) { g_ctx.set_scissor(*reinterpret_cast<const d3d::RECT*>(c->rdx)); ret(c); }
void dev_set_cull(CPU* c) {
    uint64_t dev = c->rcx;
    uint8_t cull = uint8_t(c->rdx);
    if (rd8(dev + 640) != cull) { wr8(dev + 640, cull); apply_raster(dev); }
    ret(c);
}
void dev_set_raster_42(CPU* c) {  /* scissor enable */
    uint64_t dev = c->rcx;
    uint32_t v = uint8_t(c->rdx);
    if ((rd32(dev + 8) == 0) != (v == 0)) { wr32(dev + 8, v); apply_raster(dev); }
    ret(c, v ^ 1);
}
void dev_set_raster_25(CPU* c) {  /* depth bias */
    uint64_t dev = c->rcx;
    uint32_t v = uint8_t(c->rdx);
    if ((rd32(dev + 644) == 0) != (v == 0)) { wr32(dev + 644, v); apply_raster(dev); }
    ret(c, v ^ 1);
}
void dev_set_raster_51(CPU* c) {  /* wireframe */
    uint64_t dev = c->rcx;
    uint32_t v = uint32_t(c->rdx);
    if (rd32(dev + 648) != v) { wr32(dev + 648, v); apply_raster(dev); }
    ret(c);
}
void dev_apply_depth_state(CPU* c) {
    uint64_t dev = c->rcx;
    uint64_t rd = rd64(dev + 616);
    g_ctx.set_depth(rd ? object<DepthState>(rd64(rd + 24)) : nullptr, rd32(dev + 656));
    ret(c);
}
void pso_apply(CPU* c) {
    uint64_t pso = c->rcx, dev = rd64(DEVICE_OBJECT);
    uint8_t bits = rd8(dev + 1184) & uint8_t(~rd8(pso + 103));
    if (bits & 2) g_ctx.set_shader(HS, nullptr);
    if (bits & 4) g_ctx.set_shader(DS, nullptr);
    if (bits & 8) g_ctx.set_shader(GS, nullptr);
    if (bits & 16) g_ctx.set_shader(PS, nullptr);
    bits &= 0xE0;
    uint64_t entries = rd64(pso + 24);
    int32_t count = int32_t(rd32(pso + 32));
    for (int32_t i = 0; i < count; ++i) {
        uint64_t e = entries + 16 * uint64_t(i);
        uint64_t shader = rd64(e + 8);
        uint64_t holder = shader ? rd64(shader + 40) : 0;
        if (!holder) continue;
        uint8_t stage = rd8(e);
        static const Stage stages[5] = {VS, HS, DS, GS, PS};
        if (stage > 4) continue;
        g_ctx.set_shader(stages[stage], object<Shader>(rd64(holder + 24)));
        bits |= uint8_t(1u << stage);
    }
    wr8(dev + 1184, bits);
    g_ctx.set_blend(object<BlendState>(rd64(pso + 104)), nullptr, 0xFFFFFFFFu);
    g_ctx.set_input_layout(object<InputLayout>(rd64(pso + 112)));
    ret(c);
}

/* ---------------------------------------------------------------- shader resources */
void bind_srv(uint64_t dev, uint64_t srv, uint32_t mode, uint32_t slot) {
    wr64(dev + 2768 + 8 * (uint64_t(slot) + 16 * mode), srv);
    if (mode == 0 || mode == 2 || mode == 4 || mode == 5) g_ctx.set_srv(Stage(mode), slot, object<View>(srv));
}
void dev_bind_srv(CPU* c) { bind_srv(c->rcx, c->rdx, uint8_t(c->r8), uint32_t(c->r9)); ret(c); }
void bind_cs_uav(uint64_t dev, uint64_t uav, uint32_t slot) {
    g_ctx.set_uav(slot, object<View>(uav));
    wr64(dev + 3536 + 8 * uint64_t(slot), uav);
}
void dev_bind_cs_uav(CPU* c) { bind_cs_uav(c->rcx, c->rdx, uint32_t(c->r9)); ret(c, c->rdx); }

/* RenderingDeviceDirectX11Base::SetTexture(slot, texture, flags, view, sampler key) */
void set_texture(CPU* c, uint64_t dev, uint32_t slot, uint64_t tex, uint16_t flags, int32_t view, int32_t key) {
    if (flags & 0x100) {   /* compute UAV of a texture */
        bind_cs_uav(dev, tex ? rd64(rd64(tex + 88) + 8 * uint64_t(view)) : 0, slot);
        return;
    }
    uint32_t group = 1, mode = 4;
    if (flags & 0x40) { group = 0; mode = 0; }
    else if (flags & 0x80) { group = 2; mode = 5; }
    uint64_t entry = dev + 1232 + 32 * (uint64_t(slot) + 16 * group);
    uint64_t resource = tex ? rd64(tex + 32) : 0;
    bool same = rd64(entry + 8) == resource && rd16(entry + 16) == flags && int32_t(rd32(entry + 20)) == view &&
                int32_t(rd32(entry + 24)) == key &&
                !(tex && rd64(dev + 2768 + 8 * (uint64_t(slot) + 16 * mode)) != rd64(rd64(tex + 40)));
    if (same) return;
    wr64(entry + 8, resource); wr16(entry + 16, flags); wr32(entry + 20, uint32_t(view)); wr32(entry + 24, uint32_t(key));
    if (tex) {
        wr64(entry, tex);
        uint64_t srv = rd64(rd64(tex + 40) + 8 * uint64_t(view));
        if (group == 0) { bind_srv(dev, srv, 0, slot); bind_srv(dev, srv, 2, slot); }
        else bind_srv(dev, srv, mode, slot);
        /* The sampler state object of the key {flags, view, key} from the engine's own cache. */
        uint32_t packed = uint32_t(flags) | (uint32_t(uint8_t(view)) << 16) | (uint32_t(uint8_t(key)) << 24);
        uint64_t state = guest_call(c, SAMPLER_LOOKUP, dev, reinterpret_cast<uint64_t>(&packed));
        guest_call(c, rd64(rd64(state) + 8), state);
        Sampler* sampler = object<Sampler>(rd64(state + 24));
        if (group == 0) { g_ctx.set_sampler(VS, slot, sampler); g_ctx.set_sampler(DS, slot, sampler); }
        else g_ctx.set_sampler(group == 2 ? CS : PS, slot, sampler);
    } else {
        if (group == 0) { bind_srv(dev, 0, 0, slot); bind_srv(dev, 0, 2, slot); g_ctx.set_sampler(VS, slot, nullptr); g_ctx.set_sampler(DS, slot, nullptr); }
        else { bind_srv(dev, 0, mode, slot); g_ctx.set_sampler(group == 2 ? CS : PS, slot, nullptr); }
        for (int i = 0; i < 4; ++i) wr64(entry + 8 * uint64_t(i), 0);
    }
}
void dev_set_texture(CPU* c) {
    set_texture(c, c->rcx, uint32_t(c->rdx), c->r8, uint16_t(c->r9), int32_t(arg(c, 4)), int32_t(arg(c, 5)));
    ret(c);
}
void dev_set_uavs(CPU* c) {  /* unbinds a texture from every SRV/UAV slot before it is written */
    uint64_t dev = c->rcx, tex = c->rdx;
    uint64_t resource = rd64(tex + 32);
    static const uint16_t flags[3] = {64, 0, 128};
    uint32_t samplers = rd32(dev + 660);
    for (uint32_t group = 0; group < 3; ++group)
        for (uint32_t i = 0; i < samplers; ++i)
            if (rd64(dev + 1240 + 512 * uint64_t(group) + 32 * uint64_t(i)) == resource) set_texture(c, dev, i, 0, flags[group], 0, 0);
    uint64_t is_uav = guest_call(c, rd64(rd64(tex) + 120), tex, 64);
    if (is_uav & 0xFF) {
        uint32_t views = rd32(tex + 24);
        for (uint32_t slot = 0; slot < 8; ++slot)
            for (uint32_t k = 0; k < views; ++k)
                if (rd64(dev + 3536 + 8 * uint64_t(slot)) == rd64(rd64(tex + 88) + 8 * uint64_t(k))) bind_cs_uav(dev, 0, slot);
    }
    ret(c, is_uav & 0xFF);
}
void dev_clear_uavs(CPU* c) {  /* unbinds a buffer's views */
    uint64_t dev = c->rcx, buffer = c->rdx;
    uint64_t uav = rd64(buffer + 24), srv = rd64(buffer + 32);
    if (srv)
        for (uint32_t mode = 0; mode < 6; ++mode)
            for (uint32_t slot = 0; slot < 16; ++slot)
                if (rd64(dev + 2768 + 8 * (uint64_t(slot) + 16 * mode)) == srv) bind_srv(dev, 0, mode, slot);
    if (uav)
        for (uint32_t slot = 0; slot < 8; ++slot)
            if (rd64(dev + 3536 + 8 * uint64_t(slot)) == uav) bind_cs_uav(dev, 0, slot);
    ret(c);
}
void dev_set_constant_buffers(CPU* c) {
    uint64_t list = c->rdx;
    uint32_t start = uint32_t(c->r8), stage = uint8_t(c->r9), count = uint32_t(arg(c, 4));
    if (stage > 5) { ret(c, 0); return; }
    for (uint32_t i = 0; i < count; ++i) g_ctx.set_constant_buffer(Stage(stage), start + i, rd_buffer(rd64(list + 8 * i), 16));
    ret(c, stage);
}
void dev_dispatch(CPU* c) {
    uint64_t shader = c->rdx;
    const uint32_t* groups = reinterpret_cast<const uint32_t*>(c->r8);
    g_ctx.set_shader(CS, object<Shader>(rd64(shader + 24)));
    g_ctx.dispatch(groups[0], groups[1], groups[2]);
    ret(c);
}

/* ---------------------------------------------------------------- input assembler */
void bind_vertex_buffers(uint64_t dev, uint64_t decl, uint64_t vb0, uint64_t vb1, uint32_t off0, uint32_t off1) {
    wr64(dev + 320, 0);
    wr32(dev + 336, 0);
    if (vb0) {
        uint64_t b = rd64(vb0 + 24);
        if (rd64(dev + 1144) != b || rd32(dev + 1168) != off0) {
            g_ctx.set_vertex_buffer(0, object<Buffer>(b), off0, rd32(decl + 144));
            wr64(dev + 1144, b); wr32(dev + 1168, off0);
        }
    }
    if (vb1) {
        uint64_t b = rd64(vb1 + 24);
        if (rd64(dev + 1152) != b || rd32(dev + 1172) != off1) {
            g_ctx.set_vertex_buffer(1, object<Buffer>(b), off1, rd32(decl + 148));
            wr64(dev + 1152, b); wr32(dev + 1172, off1);
        }
    }
}
void bind_index_buffer(uint64_t dev, uint64_t buffer, uint32_t format, uint32_t offset) {
    if (rd64(dev + 1160) != buffer || rd32(dev + 1176) != offset) {
        g_ctx.set_index_buffer(object<Buffer>(buffer), format, offset);
        wr64(dev + 1160, buffer); wr32(dev + 1176, offset);
    }
}
void dev_set_vertex_buffers(CPU* c) { bind_vertex_buffers(c->rcx, c->rdx, c->r8, c->r9, uint32_t(arg(c, 4)), uint32_t(arg(c, 5))); ret(c); }
void dev_set_index_buffer_29(CPU* c) {
    uint64_t dev = c->rcx;
    bind_vertex_buffers(dev, c->rdx, c->r8, c->r9, uint32_t(arg(c, 4)), uint32_t(arg(c, 5)));
    uint64_t ib = rd64(rd64(dev + 600) + 24);
    if (rd64(dev + 1160) != ib) { g_ctx.set_index_buffer(object<Buffer>(ib), 57, 0); wr64(dev + 1160, ib); wr32(dev + 1176, 0); }
    wr64(dev + 328, 0);
    ret(c, 0);
}
void dev_set_index_buffer_30(CPU* c) {  /* user vertices with the default index buffer */
    uint64_t dev = c->rcx, decl = c->rdx, data = c->r8;
    wr64(dev + 320, data);
    wr32(dev + 336, data ? rd32(decl + 144) : 0);
    uint64_t ib = rd64(rd64(dev + 600) + 24);
    if (rd64(dev + 1160) != ib) { g_ctx.set_index_buffer(object<Buffer>(ib), 57, 0); wr64(dev + 1160, ib); wr32(dev + 1176, 0); }
    wr64(dev + 328, 0);
    ret(c, rd64(dev + 600));
}
void dev_set_stream_32(CPU* c) {
    uint64_t dev = c->rcx, ib = arg(c, 4);
    uint32_t format = uint32_t(arg(c, 5));
    bind_vertex_buffers(dev, c->rdx, c->r8, c->r9, 0, 0);
    if (ib) {
        uint64_t b = rd64(ib + 24);
        if (rd64(dev + 1160) != b) {
            g_ctx.set_index_buffer(object<Buffer>(b), format == 102 ? 42 : 57, 0);
            wr64(dev + 1160, b); wr32(dev + 1176, 0);
        }
    }
    wr64(dev + 328, 0);
    ret(c);
}
void user_vertices(uint64_t dev, uint32_t vertices) {
    if (!vertices || !rd64(dev + 320)) return;
    uint32_t bytes = rd32(dev + 336) * vertices, capacity = rd32(dev + 28);
    if (bytes >= capacity) { log("RDVK user vertex data too large (%u/%u)", bytes, capacity); return; }
    uint32_t offset = rd32(dev + 24);
    Buffer* b = rd_buffer(rd64(dev + 16), 24);
    if (offset + bytes > capacity || offset == 0) { offset = 0; g_ctx.discard(b, b->desc.ByteWidth); }
    memcpy(b->mapped + offset, reinterpret_cast<const void*>(rd64(dev + 320)), bytes);
    uint64_t guest = guest_of(b);
    g_ctx.set_vertex_buffer(0, b, offset, rd32(dev + 336));
    wr64(dev + 1144, guest); wr32(dev + 1168, offset);
    wr32(dev + 24, bytes + offset + (bytes & 0xF));
}
void user_indices(uint64_t dev, uint32_t indices) {
    if (!rd64(dev + 328)) return;
    uint32_t bytes = 2 * indices, capacity = rd32(dev + 36);
    if (bytes >= capacity) { log("RDVK user index data too large (%u/%u)", bytes, capacity); return; }
    uint32_t offset = rd32(dev + 32);
    Buffer* b = rd_buffer(rd64(dev + 40), 24);
    if (offset + bytes > capacity || offset == 0) { offset = 0; g_ctx.discard(b, b->desc.ByteWidth); }
    memcpy(b->mapped + offset, reinterpret_cast<const void*>(rd64(dev + 328)), bytes);
    g_ctx.set_index_buffer(b, 57, offset);
    wr64(dev + 1160, guest_of(b)); wr32(dev + 1176, offset);
    wr32(dev + 32, bytes + offset);
}
void dev_user_vertices(CPU* c) { user_vertices(c->rcx, uint32_t(c->rdx)); ret(c); }
void dev_user_indices(CPU* c) { user_indices(c->rcx, uint32_t(c->rdx)); ret(c); }
void dev_save_ia(CPU* c) {
    uint64_t dev = c->rcx;
    for (uint64_t o = 3608; o <= 3648; o += 8) wr64(dev + o, 0);
    wr8(dev + 3600, 1);
    uint64_t save = dev + 3608;   /* {IB, format, offset, VB0, stride, offset, texture, flags} */
    wr64(save + 0, rd64(dev + 1160)); wr32(save + 8, g_ctx.index_format()); wr32(save + 12, rd32(dev + 1176));
    wr64(save + 16, rd64(dev + 1144)); wr32(save + 24, g_ctx.vertex_stride(0)); wr32(save + 28, rd32(dev + 1168));
    wr64(save + 32, rd64(dev + 1744));
    wr16(save + 40, rd16(dev + 1760));
    ret(c);
}
void dev_restore_ia(CPU* c) {
    uint64_t dev = c->rcx, save = dev + 3608;
    if (rd64(dev + 1144) != rd64(save + 16) || rd32(dev + 1168) != rd32(save + 28)) {
        g_ctx.set_vertex_buffer(0, object<Buffer>(rd64(save + 16)), rd32(save + 28), rd32(save + 24));
        wr64(dev + 1144, rd64(save + 16)); wr32(dev + 1168, rd32(save + 28));
    }
    if (rd64(dev + 1160) != rd64(save) || rd32(dev + 1176) != rd32(save + 12)) {
        g_ctx.set_index_buffer(object<Buffer>(rd64(save)), rd32(save + 8), rd32(save + 12));
        wr64(dev + 1160, rd64(save)); wr32(dev + 1176, rd32(save + 12));
    }
    set_texture(c, dev, 0, rd64(save + 32), rd16(save + 40), 0, 0);
    wr8(dev + 3600, 0);
    ret(c);
}

/* ---------------------------------------------------------------- draws */
void dev_draw_indexed(CPU* c) {
    uint64_t dev = c->rcx;
    uint32_t type = uint32_t(c->rdx), vertices = uint32_t(c->r8), start = uint32_t(c->r9);
    uint32_t prims = uint32_t(arg(c, 4));
    int32_t base = int32_t(arg(c, 5));
    wr32(dev + 668, rd32(dev + 668) + 1);
    wr32(dev + 664, rd32(dev + 664) + prims);
    uint32_t indices = index_count(type, prims);
    if (rd64(dev + 320)) { user_vertices(dev, vertices); user_indices(dev, indices); }
    set_topology(dev, type);
    g_ctx.draw_indexed(indices, start, base);
    ret(c);
}
void dev_draw_indexed_instanced(CPU* c) {
    uint64_t dev = c->rcx;
    uint32_t type = uint32_t(c->rdx), indices = uint32_t(c->r8), prims = uint32_t(c->r9), instances = uint32_t(arg(c, 4));
    wr32(dev + 668, rd32(dev + 668) + 1);
    wr32(dev + 664, rd32(dev + 664) + instances * prims);
    set_topology(dev, type);
    g_ctx.draw_indexed(indices, 0, 0, instances);
    ret(c);
}
void dev_draw(CPU* c) {
    uint64_t dev = c->rcx;
    uint32_t type = uint32_t(c->rdx), start = uint32_t(c->r8), prims = uint32_t(c->r9);
    wr32(dev + 668, rd32(dev + 668) + 1);
    wr32(dev + 664, rd32(dev + 664) + prims);
    uint32_t vertices = index_count(type, prims);
    if (rd64(dev + 320)) user_vertices(dev, vertices);
    set_topology(dev, type);
    g_ctx.draw(vertices, start);
    ret(c);
}

/* ---------------------------------------------------------------- buffers and textures */
void buffer_update(CPU* c) {  /* RDDirectX11Buffer::Update(data, size): DISCARD + copy */
    uint64_t rd = c->rcx;
    uint32_t bytes = uint32_t(c->r8);
    Buffer* b = rd_buffer(rd, 16);
    if (b && c->rdx && bytes) {
        uint8_t* p = b->dynamic ? g_ctx.discard(b, bytes) : nullptr;
        if (p) memcpy(p, reinterpret_cast<const void*>(c->rdx), std::min<uint32_t>(bytes, b->desc.ByteWidth));
        else g_ctx.update_subresource(b, 0, nullptr, reinterpret_cast<const void*>(c->rdx), bytes, bytes);
    }
    ret(c);
}
void buffer_map(CPU* c) {  /* (D3D11 buffer, dynamic, offset, -, mode) -> pointer */
    Buffer* b = object<Buffer>(c->rcx);
    uint32_t dynamic = uint8_t(c->rdx), offset = uint32_t(c->r8), mode = uint8_t(arg(c, 4));
    uint32_t type = mode == 1 ? d3d::MAP_READ : mode == 3 ? d3d::MAP_READ_WRITE : (mode == 4 || mode == 6) ? d3d::MAP_WRITE_NO_OVERWRITE
                  : (dynamic ? d3d::MAP_WRITE_DISCARD : d3d::MAP_WRITE);
    d3d::MAPPED_SUBRESOURCE m{};
    if (!b || g_ctx.map(b, 0, type, 0, &m) != d3d::HR_S_OK) { ret(c, 0); return; }
    ret(c, m.pData + offset);
}
void buffer_unmap(CPU* c) { ret(c); }
void vib_unlock(CPU* c) { ret(c); }
void tex_generate_mips(CPU* c) { g_ctx.generate_mips(object<View>(rd64(rd64(c->rcx + 40)))); ret(c); }
void tex_update(CPU* c) {  /* (width in texels, data): whole mip 0 */
    uint64_t rd = c->rcx;
    uint32_t width = uint32_t(c->rdx);
    uint32_t f = rd32(rd + 64);
    uint32_t bpp = (f == 0x1C || f == 0x1D || f == 0x57 || f == 0x5B) ? 4 : f == 0x31 ? 2 : f == 0x41 ? 1 : 4;
    auto* t = object<Texture>(rd64(rd + 32));
    if (t) g_ctx.update_subresource(t, 0, nullptr, reinterpret_cast<const void*>(c->r8), width * bpp, width * bpp);
    ret(c);
}
void tex_update_region(CPU* c) {  /* (mip, face, -, -, row pitch, data, depth pitch) */
    uint64_t rd = c->rcx;
    auto* t = object<Texture>(rd64(rd + 32));
    if (t) {
        uint32_t mip = uint32_t(c->rdx), face = uint32_t(c->r8);
        uint32_t sub = t->layers > 1 ? mip + t->mips * face : mip;
        g_ctx.update_subresource(t, sub, nullptr, reinterpret_cast<const void*>(arg(c, 6)), uint32_t(arg(c, 5)), uint32_t(arg(c, 7)));
    }
    ret(c);
}
}  // namespace

uint32_t read_texture(uint64_t guest, uint32_t* width, uint32_t* height, uint32_t* format, uint8_t** rgba) {
    *rgba = nullptr;
    Object* o = guest ? object_of(guest) : nullptr;
    if (!o || o->kind != Kind::Texture2D) return d3d::HR_E_INVALIDARG;
    auto* src = static_cast<Texture*>(o);
    *width = src->width; *height = src->height; *format = src->dxgi;
    bool bgra = src->dxgi == 87 || src->dxgi == 91, rgba8 = src->dxgi == 28 || src->dxgi == 29;
    if (!(bgra || rgba8) || !src->width || !src->height || src->width > 16384 || src->height > 16384 || src->samples != 1)
        return 0x80070032u;   /* HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), as the D3D11 path */
    d3d::TEXTURE2D_DESC desc{src->width, src->height, src->mips, src->layers, src->dxgi, {1, 0}, d3d::USAGE_STAGING, 0, d3d::CPU_ACCESS_READ, 0};
    Texture* staging = nullptr;
    uint32_t hr = create_texture(2, &desc, nullptr, &staging);
    if (hr != d3d::HR_S_OK) return hr;
    g_ctx.copy_resource(staging, src);
    d3d::MAPPED_SUBRESOURCE mapped{};
    hr = g_ctx.map(staging, 0, d3d::MAP_READ, 0, &mapped);
    if (hr == d3d::HR_S_OK) {
        size_t row = size_t(src->width) * 4;
        auto* pixels = static_cast<uint8_t*>(malloc(row * src->height));
        if (!pixels) hr = d3d::HR_E_OUTOFMEMORY;
        else {
            for (uint32_t y = 0; y < src->height; ++y) {
                uint8_t* out = pixels + row * y;
                memcpy(out, reinterpret_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, row);
                if (bgra) for (uint32_t x = 0; x < src->width; ++x) std::swap(out[x * 4], out[x * 4 + 2]);
            }
            *rgba = pixels;
        }
        g_ctx.unmap(staging, 0);
    }
    release(staging);
    return hr;
}

int native_init(rdvk_fn table[RDVK_HOOK_MAX]) {
#define RDVK_NATIVE2(index, fn) table[RDVK_HOOK_##index] = fn;
    std::string only = property("debug.fp.rdvk_native");   /* "none": every hook on its translated body (COM path) */
    if (only != "none") {
        RDVK_NATIVE2(DEV_FRAME_BEGIN_QUERY, dev_frame_begin_query)
        RDVK_NATIVE2(DEV_FRAME_END_QUERY, dev_frame_end_query)
        RDVK_NATIVE2(DEV_PRESENT, dev_present)
        RDVK_NATIVE2(DEV_COMMIT_RENDER_TARGETS, dev_commit_render_targets)
        RDVK_NATIVE2(RT_CLEAR, rt_clear)
        RDVK_NATIVE2(DS_CLEAR, ds_clear)
        RDVK_NATIVE2(DEV_SET_VIEWPORT, dev_set_viewport)
        RDVK_NATIVE2(DEV_SET_SCISSOR, dev_set_scissor)
        RDVK_NATIVE2(DEV_SET_CULL, dev_set_cull)
        RDVK_NATIVE2(DEV_SET_RASTER_42, dev_set_raster_42)
        RDVK_NATIVE2(DEV_SET_RASTER_25, dev_set_raster_25)
        RDVK_NATIVE2(DEV_SET_RASTER_51, dev_set_raster_51)
        RDVK_NATIVE2(DEV_APPLY_DEPTH_STATE, dev_apply_depth_state)
        RDVK_NATIVE2(PSO_APPLY, pso_apply)
        RDVK_NATIVE2(DEV_BIND_SRV, dev_bind_srv)
        RDVK_NATIVE2(DEV_BIND_CS_UAV, dev_bind_cs_uav)
        RDVK_NATIVE2(DEV_SET_TEXTURE, dev_set_texture)
        RDVK_NATIVE2(DEV_SET_UAVS, dev_set_uavs)
        RDVK_NATIVE2(DEV_CLEAR_UAVS, dev_clear_uavs)
        RDVK_NATIVE2(DEV_SET_CONSTANT_BUFFERS, dev_set_constant_buffers)
        RDVK_NATIVE2(DEV_DISPATCH, dev_dispatch)
        RDVK_NATIVE2(DEV_SET_VERTEX_BUFFERS, dev_set_vertex_buffers)
        RDVK_NATIVE2(DEV_SET_INDEX_BUFFER_29, dev_set_index_buffer_29)
        RDVK_NATIVE2(DEV_SET_INDEX_BUFFER_30, dev_set_index_buffer_30)
        RDVK_NATIVE2(DEV_SET_STREAM_32, dev_set_stream_32)
        RDVK_NATIVE2(DEV_USER_VERTICES, dev_user_vertices)
        RDVK_NATIVE2(DEV_USER_INDICES, dev_user_indices)
        RDVK_NATIVE2(DEV_SAVE_IA, dev_save_ia)
        RDVK_NATIVE2(DEV_RESTORE_IA, dev_restore_ia)
        RDVK_NATIVE2(DEV_DRAW_INDEXED, dev_draw_indexed)
        RDVK_NATIVE2(DEV_DRAW_INDEXED_INSTANCED, dev_draw_indexed_instanced)
        RDVK_NATIVE2(DEV_DRAW, dev_draw)
        RDVK_NATIVE2(BUFFER_UPDATE, buffer_update)
        RDVK_NATIVE2(BUFFER_MAP, buffer_map)
        RDVK_NATIVE2(BUFFER_UNMAP, buffer_unmap)
        RDVK_NATIVE2(VIB_UNLOCK, vib_unlock)
        RDVK_NATIVE2(TEX_GENERATE_MIPS, tex_generate_mips)
        RDVK_NATIVE2(TEX_UPDATE, tex_update)
        RDVK_NATIVE2(TEX_UPDATE_REGION, tex_update_region)
    }
    RDVK_NATIVE2(DEV_INIT, dev_init)
#undef RDVK_NATIVE2

    unsigned n = 0;
    for (unsigned i = 0; i < RDVK_HOOK_MAX; ++i) n += table[i] != nullptr;
    log("RDVK_NATIVE hooks=%u of %u (others keep their translated body over the COM path)", n, RDVK_HOOK_COUNT);
    return 1;
}
}  // namespace rdvk
