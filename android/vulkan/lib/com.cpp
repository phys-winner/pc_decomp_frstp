/* COM safety net: D3D11/DXGI calls of the engine's cold path (resource creation at load time,
 * the window/swap chain, rare backend functions that keep their lifted body) on the native
 * device's objects.  The hot path never comes here: engine.cpp replaces those functions. */
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "context.h"
#include "present.h"
#include "rdvk.h"

namespace rdvk {
namespace {
using namespace d3d;

uint32_t frame_latency = 1;

void out_ptr(uint64_t address, Object* o) { if (address) wr64(address, guest_of(o)); }
template <class T> T* obj(CPU* c, unsigned i) { uint64_t g = arg(c, i); return g ? static_cast<T*>(object_of(g)) : nullptr; }
Resource* resource_arg(CPU* c, unsigned i) { return obj<Resource>(c, i); }

uint32_t iid_head(uint64_t riid) { return riid ? rd32(riid) : 0; }
bool iid_device(uint32_t d) { return d == 0xdb6f6ddb || d == 0xa04bfb29 || d == 0x9d06dffa || d == 0xa05c8c37 || d == 0x8992ab71 || d == 0x8ffde202; }
bool iid_dxgi_device(uint32_t d) { return d == 0x05008617 || d == 0x77db970f || d == 0x54ec77fa || d == 0x3d3e0379; }
bool iid_factory(uint32_t d) { return d == 0x50c83a1c || d == 0x770aae78 || d == 0x7b7166ec || d == 0x25483823 || d == 0x1bc6ea02; }
bool iid_adapter(uint32_t d) { return d == 0x29038f61 || d == 0x2411e7e1 || d == 0x0aa1ae0a; }

uint32_t query_interface(Object* self, uint64_t riid, uint64_t out) {
    if (!out) return HR_E_INVALIDARG;
    wr64(out, 0);
    uint32_t d = iid_head(riid);
    Object* result = nullptr;
    if (d == 0 && riid && rd32(riid + 4) == 0 && rd32(riid + 8) == 0xC0) result = self;       /* IUnknown */
    else if (iid_device(d)) result = g_device;
    else if (iid_dxgi_device(d)) result = g_dxgi_device;
    else if (iid_factory(d)) result = g_factory;
    else if (iid_adapter(d)) result = g_adapter;
    else if (d == 0xc0bfa96c || d == 0xbb2c6faa || d == 0x420d5b32 || d == 0xb4e3c01d || d == 0x917600da) result = g_context;
    else if (d == 0x790a45f7 || d == 0x310d36a0 || d == 0xa8be2ac4 || d == 0x94d99bdb) result = self->kind == Kind::SwapChain ? self : nullptr;
    else if (d == 0x1841e5c8 || d == 0xdc8e63f3 || d == 0x48570b85 || d == 0xf8fb5c27 || d == 0x6f15aaf2 || d == 0x037e866e ||
             d == 0x839d1216 || d == 0xb0e06fe0 || d == 0x28acf509 || d == 0xdfdba067 || d == 0x9fdac92a || d == 0x75b68faa ||
             d == 0x03823efb || d == 0xa6cd7faa || d == 0xda6fea51 || d == 0xd6c00747 || d == 0x9bb4ab81 || d == 0x75b68faa)
        result = self;   /* D3D11 resource/child interfaces of the object itself */
    if (!result) {
        static int notices = 0;
        if (notices++ < 16) log("RDVK_QI unsupported iid=%08x on %s", d, kind_name(self->kind));
        return HR_E_NOINTERFACE;
    }
    add_ref(result);
    wr64(out, guest_of(result));
    return HR_S_OK;
}

uint32_t texture_desc(const Texture* t, uint64_t out) {
    if (t->dimension == 1) {
        TEXTURE1D_DESC d{t->width, t->mips, t->layers, t->dxgi, t->usage, t->bind, t->cpu, t->misc};
        memcpy(reinterpret_cast<void*>(out), &d, sizeof(d));
    } else if (t->dimension == 2) {
        TEXTURE2D_DESC d{t->width, t->height, t->mips, t->layers, t->dxgi, {t->samples, 0}, t->usage, t->bind, t->cpu, t->misc};
        memcpy(reinterpret_cast<void*>(out), &d, sizeof(d));
    } else {
        TEXTURE3D_DESC d{t->width, t->height, t->depth, t->mips, t->dxgi, t->usage, t->bind, t->cpu, t->misc};
        memcpy(reinterpret_cast<void*>(out), &d, sizeof(d));
    }
    return HR_S_OK;
}

/* ---------------------------------------------------------------- ID3D11Device */
bool device_call(CPU* c, unsigned slot) {
    switch (slot) {
    case 3: {  /* CreateBuffer(desc, data, out) */
        Buffer* b = nullptr;
        uint32_t hr = create_buffer(*reinterpret_cast<const BUFFER_DESC*>(arg(c, 1)), reinterpret_cast<const SUBRESOURCE_DATA*>(arg(c, 2)), &b);
        out_ptr(arg(c, 3), b); c->rax = hr; return true;
    }
    case 4: case 5: case 6: {  /* CreateTexture1D/2D/3D(desc, data, out) */
        Texture* t = nullptr;
        uint32_t hr = create_texture(slot - 3, reinterpret_cast<const void*>(arg(c, 1)), reinterpret_cast<const SUBRESOURCE_DATA*>(arg(c, 2)), &t);
        out_ptr(arg(c, 3), t); c->rax = hr; return true;
    }
    case 7: case 8: case 9: case 10: {  /* Create{SRV,UAV,RTV,DSV}(resource, desc, out) */
        Resource* r = resource_arg(c, 1);
        View* v = nullptr;
        uint32_t hr = HR_E_INVALIDARG;
        if (r) {
            if (slot == 7) hr = create_srv(r, reinterpret_cast<const SRV_DESC*>(arg(c, 2)), &v);
            else if (slot == 8) hr = create_uav(r, reinterpret_cast<const UAV_DESC*>(arg(c, 2)), &v);
            else if (slot == 9) hr = create_rtv(r, reinterpret_cast<const RTV_DESC*>(arg(c, 2)), &v);
            else hr = create_dsv(r, reinterpret_cast<const DSV_DESC*>(arg(c, 2)), &v);
        }
        out_ptr(arg(c, 3), v); c->rax = hr; return true;
    }
    case 11: {  /* CreateInputLayout(elements, count, bytecode, size, out) */
        InputLayout* l = nullptr;
        uint32_t hr = create_input_layout(reinterpret_cast<const INPUT_ELEMENT_DESC*>(arg(c, 1)), uint32_t(arg(c, 2)),
                                          reinterpret_cast<const uint8_t*>(arg(c, 3)), size_t(arg(c, 4)), &l);
        out_ptr(arg(c, 5), l); c->rax = hr; return true;
    }
    case 12: case 13: case 15: case 16: case 17: case 18: {  /* Create*Shader(bytecode, size, linkage, out) */
        Kind k = slot == 12 ? Kind::VertexShader : slot == 13 ? Kind::GeometryShader : slot == 15 ? Kind::PixelShader
               : slot == 16 ? Kind::HullShader : slot == 17 ? Kind::DomainShader : Kind::ComputeShader;
        Shader* s = nullptr;
        uint32_t hr = create_shader(k, reinterpret_cast<const uint8_t*>(arg(c, 1)), size_t(arg(c, 2)), &s);
        out_ptr(arg(c, 4), s); c->rax = hr; return true;
    }
    case 20: { BlendState* s = nullptr; uint32_t hr = create_blend(*reinterpret_cast<const BLEND_DESC*>(arg(c, 1)), &s); out_ptr(arg(c, 2), s); c->rax = hr; return true; }
    case 21: { DepthState* s = nullptr; uint32_t hr = create_depth(*reinterpret_cast<const DEPTH_STENCIL_DESC*>(arg(c, 1)), &s); out_ptr(arg(c, 2), s); c->rax = hr; return true; }
    case 22: { RasterState* s = nullptr; uint32_t hr = create_raster(*reinterpret_cast<const RASTERIZER_DESC*>(arg(c, 1)), &s); out_ptr(arg(c, 2), s); c->rax = hr; return true; }
    case 23: { Sampler* s = nullptr; uint32_t hr = create_sampler(*reinterpret_cast<const SAMPLER_DESC*>(arg(c, 1)), &s); out_ptr(arg(c, 2), s); c->rax = hr; return true; }
    case 24: { Query* q = nullptr; uint32_t hr = create_query(*reinterpret_cast<const QUERY_DESC*>(arg(c, 1)), &q); out_ptr(arg(c, 2), q); c->rax = hr; return true; }
    case 29: {  /* CheckFormatSupport(format, out) */
        uint32_t f = uint32_t(arg(c, 1));
        uint32_t support = 0x1 | 0x2 | 0x4 | 0x8 | 0x20 | 0x80 | 0x100 | 0x200 | 0x4000 | 0x8000 | 0x10000 | 0x40000;
        if (f >= 70 && f <= 84) support = 0;   /* no BC on this device class */
        if (arg(c, 2)) wr32(arg(c, 2), support);
        c->rax = support ? HR_S_OK : HR_E_FAIL; return true;
    }
    case 30: { if (arg(c, 3)) wr32(arg(c, 3), uint32_t(arg(c, 2)) <= 1 ? 1 : 0); c->rax = HR_S_OK; return true; }  /* CheckMultisampleQualityLevels */
    case 33: {  /* CheckFeatureSupport(feature, data, size): zero = not supported */
        if (arg(c, 2) && uint32_t(arg(c, 3)) <= 64) memset(reinterpret_cast<void*>(arg(c, 2)), 0, uint32_t(arg(c, 3)));
        c->rax = HR_S_OK; return true;
    }
    case 34: c->rax = HR_DXGI_NOT_FOUND; return true;   /* GetPrivateData */
    case 35: case 36: c->rax = HR_S_OK; return true;          /* SetPrivateData(Interface) */
    case 37: c->rax = 0xb000; return true;                 /* GetFeatureLevel: 11_0 */
    case 38: c->rax = 0; return true;                      /* GetCreationFlags */
    case 39: c->rax = HR_S_OK; return true;                   /* GetDeviceRemovedReason */
    case 40: add_ref(g_context); out_ptr(arg(c, 1), g_context); return true;  /* GetImmediateContext */
    case 41: c->rax = HR_S_OK; return true;
    case 42: c->rax = 0; return true;
    default: return false;
    }
}

/* ---------------------------------------------------------------- DXGI */
void adapter_desc(uint64_t out, bool desc1) {
    ADAPTER_DESC d{};
    const char* name = gpu.caps.name;
    for (int i = 0; i < 127 && name[i]; ++i) d.Description[i] = uint16_t(uint8_t(name[i]));
    d.VendorId = gpu.caps.vendor; d.DeviceId = gpu.caps.device_id;
    /* Video memory: 2 GiB dedicated and 4 GiB shared, as the first native device reported (the
     * engine sizes its texture budget from it, and the memory measurements were made with it).
     * debug.fp.rdvk_vram_mb=N overrides the dedicated size; =heap reports the device heaps as
     * DXVK does (dxgi_adapter.cpp). */
    VkDeviceSize device_memory = 0, shared_memory = 0;
    for (uint32_t i = 0; i < gpu.memory_properties.memoryHeapCount; ++i) {
        const VkMemoryHeap& h = gpu.memory_properties.memoryHeaps[i];
        if (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) device_memory = std::max(device_memory, h.size);
        else shared_memory += h.size;
    }
    if (!shared_memory) shared_memory = device_memory;
    constexpr VkDeviceSize adjustment = 32ull << 20;
    if (device_memory && !(device_memory & 0xffffffffull)) device_memory -= adjustment;
    if (shared_memory && !(shared_memory & 0xffffffffull)) shared_memory -= adjustment;
    std::string forced = property("debug.fp.rdvk_vram_mb");
    if (forced != "heap") { device_memory = 2048ull << 20; shared_memory = 4096ull << 20; }
    if (!forced.empty() && atoll(forced.c_str()) > 0) device_memory = VkDeviceSize(atoll(forced.c_str())) << 20;
    static bool logged = false;
    if (!logged) {
        logged = true;
        log("RDVK_ADAPTER memory dedicated=%llu MB shared=%llu MB", (unsigned long long)(device_memory >> 20), (unsigned long long)(shared_memory >> 20));
    }
    d.DedicatedVideoMemory = device_memory; d.SharedSystemMemory = shared_memory;
    d.LuidLow = 1;
    memcpy(reinterpret_cast<void*>(out), &d, sizeof(d));
    if (desc1) wr32(out + sizeof(d), 0);
}

bool dxgi_call(CPU* c, Object* self, unsigned slot) {
    Kind k = self->kind;
    if (slot >= 3 && slot <= 5) { c->rax = slot == 5 ? HR_DXGI_NOT_FOUND : HR_S_OK; return true; }  /* private data */
    if (slot == 6) {   /* GetParent(riid, out) */
        Object* parent = k == Kind::DxgiDevice ? static_cast<Object*>(g_adapter) : k == Kind::Adapter ? static_cast<Object*>(g_factory)
                       : k == Kind::SwapChain ? static_cast<Object*>(g_factory) : nullptr;
        if (!parent) { c->rax = HR_E_NOINTERFACE; return true; }
        c->rax = query_interface(parent, arg(c, 1), arg(c, 2));
        return true;
    }
    if (k == Kind::DxgiDevice) {
        switch (slot) {
        case 7: add_ref(g_adapter); out_ptr(arg(c, 1), g_adapter); c->rax = HR_S_OK; return true;   /* GetAdapter */
        case 10: c->rax = HR_S_OK; return true;
        case 11: if (arg(c, 1)) wr32(arg(c, 1), 0); c->rax = HR_S_OK; return true;
        case 12: frame_latency = std::max(1u, uint32_t(arg(c, 1))); c->rax = HR_S_OK; return true;   /* SetMaximumFrameLatency */
        case 13: if (arg(c, 1)) wr32(arg(c, 1), frame_latency); c->rax = HR_S_OK; return true;
        default: return false;
        }
    }
    if (k == Kind::Adapter) {
        switch (slot) {
        case 7: if (arg(c, 2)) wr64(arg(c, 2), 0); c->rax = HR_DXGI_NOT_FOUND; return true;    /* EnumOutputs */
        case 8: adapter_desc(arg(c, 1), false); c->rax = HR_S_OK; return true;
        case 9: c->rax = HR_S_OK; return true;
        case 10: adapter_desc(arg(c, 1), true); c->rax = HR_S_OK; return true;
        default: return false;
        }
    }
    if (k == Kind::Factory) {
        switch (slot) {
        case 7: case 12:   /* EnumAdapters(1)(index, out) */
            if (uint32_t(arg(c, 1)) != 0) { if (arg(c, 2)) wr64(arg(c, 2), 0); c->rax = HR_DXGI_NOT_FOUND; return true; }
            add_ref(g_adapter); out_ptr(arg(c, 2), g_adapter); c->rax = HR_S_OK; return true;
        case 8: c->rax = HR_S_OK; return true;                       /* MakeWindowAssociation */
        case 9: if (arg(c, 1)) wr64(arg(c, 1), 0); c->rax = HR_S_OK; return true;
        case 13: c->rax = 1; return true;                         /* IsCurrent */
        case 14: c->rax = 0; return true;                         /* IsWindowedStereoEnabled */
        case 15: {  /* CreateSwapChainForHwnd(device, hwnd, desc1, fullscreen, output, out) */
            const auto& desc = *reinterpret_cast<const SWAP_CHAIN_DESC1*>(arg(c, 3));
            SwapChainObject* s = swapchain_create(arg(c, 2), desc);
            out_ptr(arg(c, 6), s);
            c->rax = HR_S_OK;
            return true;
        }
        default: return false;
        }
    }
    if (k == Kind::SwapChain) {
        auto* s = static_cast<SwapChainObject*>(self);
        switch (slot) {
        case 7: c->rax = query_interface(g_device, arg(c, 1), arg(c, 2)); return true;  /* GetDevice */
        case 8: g_ctx.present(s, uint32_t(arg(c, 1))); c->rax = HR_S_OK; return true;        /* Present */
        case 9: {  /* GetBuffer(index, riid, out) */
            if (!s->back) { c->rax = HR_E_FAIL; return true; }
            add_ref(s->back); out_ptr(arg(c, 3), s->back); c->rax = HR_S_OK; return true;
        }
        case 10: case 14: c->rax = HR_S_OK; return true;
        case 11: if (arg(c, 1)) wr32(arg(c, 1), 0); if (arg(c, 2)) wr64(arg(c, 2), 0); c->rax = HR_S_OK; return true;
        case 13: c->rax = swapchain_resize(s, uint32_t(arg(c, 2)), uint32_t(arg(c, 3)), uint32_t(arg(c, 4))); return true;
        case 15: if (arg(c, 1)) wr64(arg(c, 1), 0); c->rax = HR_DXGI_NOT_FOUND; return true;
        case 16: c->rax = HR_DXGI_INVALID_CALL; return true;
        case 17: if (arg(c, 1)) wr32(arg(c, 1), uint32_t(g_ctx.frames)); c->rax = HR_S_OK; return true;
        case 18: { SWAP_CHAIN_DESC1 d = s->desc; d.Width = s->width; d.Height = s->height; memcpy(reinterpret_cast<void*>(arg(c, 1)), &d, sizeof(d)); c->rax = HR_S_OK; return true; }
        case 20: if (arg(c, 1)) wr64(arg(c, 1), s->hwnd); c->rax = HR_S_OK; return true;
        default: return false;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- ID3D11DeviceContext (cold path) */
Stage context_stage_of(unsigned slot) {
    switch (slot) {
    case 7: case 11: case 25: case 26: return VS;
    case 8: case 9: case 10: case 16: return PS;
    case 22: case 23: case 31: case 32: return GS;
    case 59: case 60: case 61: case 62: return HS;
    case 63: case 64: case 65: case 66: return DS;
    default: return CS;
    }
}
bool context_call(CPU* c, unsigned slot) {
    switch (slot) {
    case 3: add_ref(g_device); out_ptr(arg(c, 1), g_device); return true;   /* GetDevice */
    case 4: c->rax = HR_DXGI_NOT_FOUND; return true;
    case 5: case 6: c->rax = HR_S_OK; return true;
    case 7: case 16: case 22: case 62: case 66: case 71: {  /* *SetConstantBuffers(start, count, buffers) */
        Stage st = context_stage_of(slot);
        uint32_t start = uint32_t(arg(c, 1)), n = uint32_t(arg(c, 2));
        uint64_t list = arg(c, 3);
        for (uint32_t i = 0; i < n; ++i) g_ctx.set_constant_buffer(st, start + i, list ? as<Buffer>(rd64(list + 8 * i)) : nullptr);
        return true;
    }
    case 8: case 25: case 31: case 59: case 63: case 67: {  /* *SetShaderResources */
        Stage st = slot == 8 ? PS : slot == 25 ? VS : slot == 31 ? GS : slot == 59 ? HS : slot == 63 ? DS : CS;
        uint32_t start = uint32_t(arg(c, 1)), n = uint32_t(arg(c, 2));
        uint64_t list = arg(c, 3);
        for (uint32_t i = 0; i < n; ++i) g_ctx.set_srv(st, start + i, list ? as<View>(rd64(list + 8 * i)) : nullptr);
        return true;
    }
    case 10: case 26: case 32: case 61: case 65: case 70: {  /* *SetSamplers */
        Stage st = slot == 10 ? PS : slot == 26 ? VS : slot == 32 ? GS : slot == 61 ? HS : slot == 65 ? DS : CS;
        uint32_t start = uint32_t(arg(c, 1)), n = uint32_t(arg(c, 2));
        uint64_t list = arg(c, 3);
        for (uint32_t i = 0; i < n; ++i) g_ctx.set_sampler(st, start + i, list ? as<Sampler>(rd64(list + 8 * i)) : nullptr);
        return true;
    }
    case 9: case 11: case 23: case 60: case 64: case 69: {  /* *SetShader(shader, instances, count) */
        Stage st = slot == 9 ? PS : slot == 11 ? VS : slot == 23 ? GS : slot == 60 ? HS : slot == 64 ? DS : CS;
        g_ctx.set_shader(st, as<Shader>(arg(c, 1)));
        return true;
    }
    case 12: g_ctx.draw_indexed(uint32_t(arg(c, 1)), uint32_t(arg(c, 2)), int32_t(arg(c, 3))); return true;
    case 13: g_ctx.draw(uint32_t(arg(c, 1)), uint32_t(arg(c, 2))); return true;
    case 14: {  /* Map(resource, sub, type, flags, out) */
        Resource* r = resource_arg(c, 1);
        MAPPED_SUBRESOURCE m{};
        c->rax = r ? g_ctx.map(r, uint32_t(arg(c, 2)), uint32_t(arg(c, 3)), uint32_t(arg(c, 4)), &m) : HR_E_INVALIDARG;
        if (arg(c, 5)) memcpy(reinterpret_cast<void*>(arg(c, 5)), &m, sizeof(m));
        return true;
    }
    case 15: if (Resource* r = resource_arg(c, 1)) g_ctx.unmap(r, uint32_t(arg(c, 2))); return true;
    case 17: g_ctx.set_input_layout(as<InputLayout>(arg(c, 1))); return true;
    case 18: {  /* IASetVertexBuffers(start, count, buffers, strides, offsets) */
        uint32_t start = uint32_t(arg(c, 1)), n = uint32_t(arg(c, 2));
        uint64_t b = arg(c, 3), s = arg(c, 4), o = arg(c, 5);
        for (uint32_t i = 0; i < n; ++i)
            g_ctx.set_vertex_buffer(start + i, b ? as<Buffer>(rd64(b + 8 * i)) : nullptr, o ? rd32(o + 4 * i) : 0, s ? rd32(s + 4 * i) : 0);
        return true;
    }
    case 19: g_ctx.set_index_buffer(as<Buffer>(arg(c, 1)), uint32_t(arg(c, 2)), uint32_t(arg(c, 3))); return true;
    case 20: g_ctx.draw_indexed(uint32_t(arg(c, 1)), uint32_t(arg(c, 3)), int32_t(arg(c, 4)), uint32_t(arg(c, 2))); return true;
    case 21: g_ctx.draw(uint32_t(arg(c, 1)), uint32_t(arg(c, 3))); return true;
    case 24: g_ctx.set_topology(uint32_t(arg(c, 1))); return true;
    case 27: case 28: {  /* Begin/End(query) */
        if (auto* q = as<Query>(arg(c, 1))) { if (slot == 27) g_ctx.query_begin(q); else g_ctx.query_end(q); }
        return true;
    }
    case 29: {  /* GetData(query, data, size, flags) */
        auto* q = as<Query>(arg(c, 1));
        uint64_t data = arg(c, 2);
        uint32_t size = uint32_t(arg(c, 3));
        if (!q) { c->rax = HR_E_INVALIDARG; return true; }
        gpu.poll();
        if (q->desc.Query == QUERY_TIMESTAMP_DISJOINT) {
            if (data && size >= 16) { wr64(data, uint64_t(1e9 / gpu.caps.timestamp_period)); wr32(data + 8, 1); }
            c->rax = HR_S_OK; return true;
        }
        if (q->desc.Query == QUERY_OCCLUSION || q->desc.Query == QUERY_OCCLUSION_PREDICATE) {
            uint64_t samples = 0;
            bool ready = g_ctx.query_samples(q, &samples) != 0;
            if (ready && data && size) {
                if (q->desc.Query == QUERY_OCCLUSION && size >= 8) wr64(data, samples);
                else if (size >= 4) wr32(data, samples ? 1 : 0);
            }
            c->rax = ready ? HR_S_OK : HR_S_FALSE;
            return true;
        }
        if (q->serial >= gpu.serial()) g_ctx.flush();
        bool done = gpu.completed() >= q->serial;
        if (done && data && size) {
            if (q->desc.Query == QUERY_EVENT) { if (size >= 4) wr32(data, 1); else wr8(data, 1); }
            else if (size >= 8) wr64(data, 0);
            else wr8(data, 0);
        }
        c->rax = done ? HR_S_OK : HR_S_FALSE;
        return true;
    }
    case 33: {  /* OMSetRenderTargets(count, rtvs, dsv) */
        uint32_t n = uint32_t(arg(c, 1));
        uint64_t list = arg(c, 2);
        View* rtvs[MAX_RT] = {};
        for (uint32_t i = 0; i < n && i < MAX_RT; ++i) rtvs[i] = list ? as<View>(rd64(list + 8 * i)) : nullptr;
        g_ctx.set_targets(n, rtvs, as<View>(arg(c, 3)));
        return true;
    }
    case 35: {  /* OMSetBlendState(state, factor, mask) */
        float factor[4] = {1, 1, 1, 1};
        if (arg(c, 2)) memcpy(factor, reinterpret_cast<const void*>(arg(c, 2)), 16);
        g_ctx.set_blend(as<BlendState>(arg(c, 1)), factor, uint32_t(arg(c, 3)));
        return true;
    }
    case 36: g_ctx.set_depth(as<DepthState>(arg(c, 1)), uint32_t(arg(c, 2))); return true;
    case 41: g_ctx.dispatch(uint32_t(arg(c, 1)), uint32_t(arg(c, 2)), uint32_t(arg(c, 3))); return true;
    case 43: g_ctx.set_raster(as<RasterState>(arg(c, 1))); return true;
    case 44: if (uint32_t(arg(c, 1)) && arg(c, 2)) g_ctx.set_viewport(*reinterpret_cast<const VIEWPORT*>(arg(c, 2))); return true;
    case 45: if (uint32_t(arg(c, 1)) && arg(c, 2)) g_ctx.set_scissor(*reinterpret_cast<const RECT*>(arg(c, 2))); return true;
    case 46:   /* CopySubresourceRegion(dst, dst_sub, x, y, z, src, src_sub, box) */
        g_ctx.copy_region(resource_arg(c, 1), uint32_t(arg(c, 2)), uint32_t(arg(c, 3)), uint32_t(arg(c, 4)), uint32_t(arg(c, 5)),
                          resource_arg(c, 6), uint32_t(arg(c, 7)), reinterpret_cast<const BOX*>(arg(c, 8)));
        return true;
    case 47: g_ctx.copy_resource(resource_arg(c, 1), resource_arg(c, 2)); return true;
    case 48:   /* UpdateSubresource(resource, sub, box, data, row, depth) */
        if (Resource* r = resource_arg(c, 1))
            g_ctx.update_subresource(r, uint32_t(arg(c, 2)), reinterpret_cast<const BOX*>(arg(c, 3)), reinterpret_cast<const void*>(arg(c, 4)),
                                     uint32_t(arg(c, 5)), uint32_t(arg(c, 6)));
        return true;
    case 50: if (arg(c, 2)) g_ctx.clear_rtv(as<View>(arg(c, 1)), reinterpret_cast<const float*>(arg(c, 2))); return true;
    case 51: case 52: if (arg(c, 2)) g_ctx.clear_uav(as<View>(arg(c, 1)), reinterpret_cast<const uint32_t*>(arg(c, 2)), slot == 52); return true;
    case 53: g_ctx.clear_dsv(as<View>(arg(c, 1)), uint32_t(arg(c, 2)), argf(c, 3), uint8_t(arg(c, 4))); return true;
    case 54: g_ctx.generate_mips(as<View>(arg(c, 1))); return true;
    case 68: {  /* CSSetUnorderedAccessViews(start, count, uavs, counts) */
        uint32_t start = uint32_t(arg(c, 1)), n = uint32_t(arg(c, 2));
        uint64_t list = arg(c, 3);
        for (uint32_t i = 0; i < n; ++i) g_ctx.set_uav(start + i, list ? as<View>(rd64(list + 8 * i)) : nullptr);
        return true;
    }
    case 110: return true;                     /* ClearState: the engine rebinds everything */
    case 111: g_ctx.flush(); return true;      /* Flush */
    case 112: c->rax = 0; return true;         /* GetType: immediate */
    case 113: c->rax = 0; return true;
    default: return false;
    }
}

bool child_call(CPU* c, Object* self, unsigned slot) {
    switch (slot) {
    case 3: add_ref(g_device); out_ptr(arg(c, 1), g_device); return true;   /* GetDevice */
    case 4: c->rax = HR_DXGI_NOT_FOUND; return true;                     /* GetPrivateData */
    case 5: case 6: c->rax = HR_S_OK; return true;                             /* SetPrivateData(Interface) */
    default: break;
    }
    switch (self->kind) {
    case Kind::Buffer:
        if (slot == 7) { wr32(arg(c, 1), 1); return true; }
        if (slot == 8 || slot == 9) { c->rax = 0; return true; }
        if (slot == 10) { memcpy(reinterpret_cast<void*>(arg(c, 1)), &static_cast<Buffer*>(self)->desc, sizeof(BUFFER_DESC)); return true; }
        return false;
    case Kind::Texture1D: case Kind::Texture2D: case Kind::Texture3D: {
        auto* t = static_cast<Texture*>(self);
        if (slot == 7) { wr32(arg(c, 1), t->dimension + 1); return true; }
        if (slot == 8 || slot == 9) { c->rax = 0; return true; }
        if (slot == 10) { texture_desc(t, arg(c, 1)); return true; }
        return false;
    }
    case Kind::Srv: case Kind::Rtv: case Kind::Dsv: case Kind::Uav: {
        auto* v = static_cast<View*>(self);
        if (slot == 7) { add_ref(v->resource); out_ptr(arg(c, 1), v->resource); return true; }
        if (slot == 8) { size_t n = self->kind == Kind::Srv || self->kind == Kind::Dsv ? 24 : 20; memcpy(reinterpret_cast<void*>(arg(c, 1)), v->desc, n); return true; }
        return false;
    }
    case Kind::BlendState: if (slot == 7) { memcpy(reinterpret_cast<void*>(arg(c, 1)), &static_cast<BlendState*>(self)->desc, sizeof(BLEND_DESC)); return true; } return false;
    case Kind::DepthState: if (slot == 7) { memcpy(reinterpret_cast<void*>(arg(c, 1)), &static_cast<DepthState*>(self)->desc, sizeof(DEPTH_STENCIL_DESC)); return true; } return false;
    case Kind::RasterState: if (slot == 7) { memcpy(reinterpret_cast<void*>(arg(c, 1)), &static_cast<RasterState*>(self)->desc, sizeof(RASTERIZER_DESC)); return true; } return false;
    case Kind::Sampler: if (slot == 7) { memcpy(reinterpret_cast<void*>(arg(c, 1)), &static_cast<Sampler*>(self)->desc, sizeof(SAMPLER_DESC)); return true; } return false;
    case Kind::Query: {
        auto* q = static_cast<Query*>(self);
        if (slot == 7) {   /* GetDataSize */
            static const uint32_t sizes[8] = {4, 8, 8, 16, 88, 4, 16, 4};
            c->rax = q->desc.Query < 8 ? sizes[q->desc.Query] : 8;
            return true;
        }
        if (slot == 8) { memcpy(reinterpret_cast<void*>(arg(c, 1)), &q->desc, sizeof(QUERY_DESC)); return true; }
        return false;
    }
    default: return false;
    }
}
}  // namespace

/* Diagnostic (debug.fp.rdvk_census=1 when the game starts): COM calls that still reach the cold
 * path, per kind and slot, logged as RDVK_COM_CENSUS every 600 presents (com_census_frame). */
namespace {
bool census_on() { static const bool on = property("debug.fp.rdvk_census") == "1"; return on; }
std::atomic<uint32_t> census[uint32_t(Kind::Count)][256];
uint32_t census_frames = 0;
}  // namespace

void com_census_frame() {
    if (!census_on() || ++census_frames % 600) return;
    struct Row { uint32_t kind, slot, count; };
    std::vector<Row> rows;
    for (uint32_t k = 0; k < uint32_t(Kind::Count); ++k)
        for (uint32_t s = 0; s < 256; ++s) {
            uint32_t n = census[k][s].exchange(0, std::memory_order_relaxed);
            if (n) rows.push_back(Row{k, s, n});
        }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });
    uint64_t total = 0;
    for (const Row& r : rows) total += r.count;
    log("RDVK_COM_CENSUS frames=600 calls_per_frame=%.1f kinds_slots=%zu", double(total) / 600.0, rows.size());
    for (size_t i = 0; i < rows.size() && i < 16; ++i)
        log("RDVK_COM_CENSUS %s slot=%u per_frame=%.2f", kind_name(Kind(rows[i].kind)), rows[i].slot, double(rows[i].count) / 600.0);
}

int com_call(CPU* c, uint64_t target) {
    uint64_t index = (target - RDVK_SENTINEL) / 8;
    Kind kind = Kind(index / 256);
    unsigned slot = unsigned(index % 256);
    if (census_on() && uint32_t(kind) < uint32_t(Kind::Count)) census[uint32_t(kind)][slot].fetch_add(1, std::memory_order_relaxed);
    Object* self = object_of(c->rcx);
    if (self->kind != kind) stop("COM call through a %s vtable on a %s object", kind_name(kind), kind_name(self->kind));
    switch (slot) {
    case 0: c->rax = query_interface(self, arg(c, 1), arg(c, 2)); return 1;
    case 1: add_ref(self); c->rax = uint32_t(self->refs.load()); return 1;
    case 2: { int32_t left = self->refs.load() - 1; release(self); c->rax = uint32_t(std::max(left, 0)); return 1; }
    default: break;
    }
    bool handled = false;
    switch (kind) {
    case Kind::Device: handled = device_call(c, slot); break;
    case Kind::Context: handled = context_call(c, slot); break;
    case Kind::DxgiDevice: case Kind::Adapter: case Kind::Factory: case Kind::SwapChain: handled = dxgi_call(c, self, slot); break;
    default: handled = child_call(c, self, slot); break;
    }
    if (!handled) stop("unimplemented COM method %s slot %u (return 0x%llx)", kind_name(kind), slot, (unsigned long long)rd64(c->rsp));
    return 1;
}
}  // namespace rdvk
