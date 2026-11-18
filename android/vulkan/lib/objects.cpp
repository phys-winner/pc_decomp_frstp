/* Native objects: guest vtables, lifetime, creation of Vulkan resources with D3D11 semantics. */
#include "objects.h"
#include <algorithm>
#include <cstdio>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include "context.h"
#include "geometry.h"
#include "rdvk.h"
#include "stability.h"

namespace rdvk {
DeviceObject* g_device = nullptr;
DxgiDeviceObject* g_dxgi_device = nullptr;
AdapterObject* g_adapter = nullptr;
FactoryObject* g_factory = nullptr;
ContextObject* g_context = nullptr;

namespace {
uint64_t vtables[unsigned(Kind::Count)][256];
const char* const KIND_NAMES[] = {"None", "Device", "DxgiDevice", "Adapter", "Factory", "Context", "SwapChain", "Buffer",
    "Texture1D", "Texture2D", "Texture3D", "Srv", "Rtv", "Dsv", "Uav", "InputLayout", "VertexShader", "HullShader",
    "DomainShader", "GeometryShader", "PixelShader", "ComputeShader", "BlendState", "DepthState", "RasterState", "Sampler", "Query"};
}  // namespace

const char* kind_name(Kind kind) { return unsigned(kind) < unsigned(Kind::Count) ? KIND_NAMES[unsigned(kind)] : "?"; }

void objects_init() {
    for (unsigned k = 0; k < unsigned(Kind::Count); ++k)
        for (unsigned s = 0; s < 256; ++s) vtables[k][s] = RDVK_SENTINEL + (uint64_t(k) * 256 + s) * 8;
    g_device = new DeviceObject();
    g_dxgi_device = new DxgiDeviceObject();
    g_adapter = new AdapterObject();
    g_factory = new FactoryObject();
    g_context = new ContextObject();
}

Object::Object(Kind k) : vtable(reinterpret_cast<uint64_t>(vtables[unsigned(k)])), kind(k) {}

void bad_object(uint64_t guest) { stop("not a native rendering-device object: 0x%llx", (unsigned long long)guest); }

void add_ref(Object* o) { if (o) o->refs.fetch_add(1, std::memory_order_relaxed); }
void release(Object* o) {
    if (!o) return;
    int32_t left = o->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) destroy_object(o);
    else if (left < 0) stop("over-released %s %p", kind_name(o->kind), (void*)o);
}

/* ------------------------------------------------------------------ destruction */
namespace {
std::mutex dynamic_mutex;
std::unordered_set<Buffer*> dynamic_buffers;
}  // namespace

void for_each_dynamic_buffer(const std::function<void(Buffer*)>& fn) {
    std::lock_guard<std::mutex> lock(dynamic_mutex);
    for (Buffer* b : dynamic_buffers) fn(b);
}

void release_home(Buffer* b) {
    if (!b->home) return;
    VkBuffer buffer = b->home; Allocation memory = b->home_memory;
    gpu.defer([buffer, memory] { vk.vkDestroyBuffer(gpu.device, buffer, nullptr); gpu.memory.release(memory); });
    b->home = VK_NULL_HANDLE;
    b->home_memory = Allocation{};
}

void destroy_object(Object* o) {
    if (o->kind == Kind::Buffer && static_cast<Buffer*>(o)->dynamic) {
        std::lock_guard<std::mutex> lock(dynamic_mutex);
        dynamic_buffers.erase(static_cast<Buffer*>(o));
    }
    g_ctx.forget(o);
    if (o->kind == Kind::Buffer || o->kind == Kind::InputLayout) geometry::forget(o);
    o->magic = 0;
    switch (o->kind) {
    case Kind::Buffer: {
        auto* b = static_cast<Buffer*>(o);
        if (b->dynamic) { gpu.stream.release(b->chunk, gpu.serial()); release_home(b); }
        if (!b->dynamic && b->buffer) {
            VkBuffer buffer = b->buffer; Allocation memory = b->memory;
            gpu.defer([buffer, memory] { vk.vkDestroyBuffer(gpu.device, buffer, nullptr); gpu.memory.release(memory); });
        }
        delete b;
        break;
    }
    case Kind::Texture1D: case Kind::Texture2D: case Kind::Texture3D: {
        auto* t = static_cast<Texture*>(o);
        if (t->readback) {
            for (Texture* slot : t->readback->ring) release(slot);
            delete t->readback;
            t->readback = nullptr;
        }
        VkImage image = t->image; Allocation memory = t->memory; VkBuffer linear = t->linear; Allocation lm = t->linear_memory;
        gpu.defer([=] {
            if (image) vk.vkDestroyImage(gpu.device, image, nullptr);
            gpu.memory.release(memory);
            if (linear) vk.vkDestroyBuffer(gpu.device, linear, nullptr);
            gpu.memory.release(lm);
        });
        delete t;
        break;
    }
    case Kind::Srv: case Kind::Rtv: case Kind::Dsv: case Kind::Uav: {
        auto* v = static_cast<View*>(o);
        VkImageView view = v->view; VkBufferView texel = v->texel;
        std::array<VkImageView, 7> alt;
        std::copy(std::begin(v->alt), std::end(v->alt), alt.begin());
        gpu.defer([view, texel, alt] {
            if (view) vk.vkDestroyImageView(gpu.device, view, nullptr);
            for (VkImageView a : alt) if (a) vk.vkDestroyImageView(gpu.device, a, nullptr);
            if (texel) vk.vkDestroyBufferView(gpu.device, texel, nullptr);
        });
        release(v->resource);
        delete v;
        break;
    }
    case Kind::Sampler: {
        auto* s = static_cast<Sampler*>(o);
        VkSampler sampler = s->sampler;
        gpu.defer([sampler] { vk.vkDestroySampler(gpu.device, sampler, nullptr); });
        delete s;
        break;
    }
    case Kind::VertexShader: case Kind::HullShader: case Kind::DomainShader: case Kind::GeometryShader:
    case Kind::PixelShader: case Kind::ComputeShader: {
        auto* s = static_cast<Shader*>(o);
        pipelines_forget_shader(s);
        VkShaderModule module = s->module; VkBuffer uniform = s->uniform; Allocation um = s->uniform_memory;
        gpu.defer([=] {
            vk.vkDestroyShaderModule(gpu.device, module, nullptr);
            if (uniform) vk.vkDestroyBuffer(gpu.device, uniform, nullptr);
            gpu.memory.release(um);
        });
        delete s;
        break;
    }
    case Kind::BlendState: delete static_cast<BlendState*>(o); break;
    case Kind::DepthState: delete static_cast<DepthState*>(o); break;
    case Kind::RasterState: delete static_cast<RasterState*>(o); break;
    case Kind::InputLayout: delete static_cast<InputLayout*>(o); break;
    case Kind::Query: g_ctx.query_forget(static_cast<Query*>(o)); delete static_cast<Query*>(o); break;
    case Kind::SwapChain: swapchain_destroy(o); break;
    default:
        /* Device, DXGI and context singletons live for the process. */
        o->magic = OBJECT_MAGIC;
        o->refs.store(1);
        break;
    }
}

/* ------------------------------------------------------------------ buffers */
uint32_t create_buffer(const d3d::BUFFER_DESC& desc, const d3d::SUBRESOURCE_DATA* data, Buffer** out) {
    *out = nullptr;
    if (!desc.ByteWidth) return d3d::HR_E_INVALIDARG;
    auto* b = new Buffer();
    b->desc = desc;
    b->dynamic = desc.Usage == d3d::USAGE_DYNAMIC;
    b->staging = desc.Usage == d3d::USAGE_STAGING;
    VkDeviceSize size = (desc.ByteWidth + 15) & ~VkDeviceSize(15);
    if (b->dynamic) {
        /* Contents live in stream slices; the first Map(DISCARD) provides one.  A dynamic buffer
         * with initial data gets a slice now. */
        Slice s = g_ctx.stream_slice(size);
        b->buffer = s.buffer; b->offset = s.offset; b->mapped = s.data; b->chunk = s.chunk;
        gpu.stream.retain(s.chunk);
        if (data && data->pSysMem) memcpy(b->mapped, reinterpret_cast<const void*>(data->pSysMem), desc.ByteWidth);
        else memset(b->mapped, 0, desc.ByteWidth);
        {
            std::lock_guard<std::mutex> lock(dynamic_mutex);
            dynamic_buffers.insert(b);
        }
        *out = b;
        return d3d::HR_S_OK;
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (desc.BindFlags & d3d::BIND_VERTEX_BUFFER) info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (desc.BindFlags & d3d::BIND_INDEX_BUFFER) info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (desc.BindFlags & d3d::BIND_CONSTANT_BUFFER) info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (desc.BindFlags & (d3d::BIND_SHADER_RESOURCE | d3d::BIND_UNORDERED_ACCESS))
        info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (desc.MiscFlags & 0x10) info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    vk_check(vk.vkCreateBuffer(gpu.device, &info, nullptr, &b->buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(gpu.device, b->buffer, &req);
    if (b->staging)
        b->memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    else
        b->memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    vk_check(vk.vkBindBufferMemory(gpu.device, b->buffer, b->memory.memory, b->memory.offset), "vkBindBufferMemory");
    b->mapped = b->memory.mapped;
    if (data && data->pSysMem) {
        if (b->mapped) memcpy(b->mapped, reinterpret_cast<const void*>(data->pSysMem), desc.ByteWidth);
        else g_ctx.upload_buffer(b, 0, reinterpret_cast<const void*>(data->pSysMem), desc.ByteWidth, false);
        geometry::buffer_created(b, desc, reinterpret_cast<const void*>(data->pSysMem));
    }
    *out = b;
    return d3d::HR_S_OK;
}

/* ------------------------------------------------------------------ textures */
uint32_t create_texture(uint32_t dimension, const void* raw, const d3d::SUBRESOURCE_DATA* data, Texture** out) {
    *out = nullptr;
    Kind kind = dimension == 1 ? Kind::Texture1D : dimension == 3 ? Kind::Texture3D : Kind::Texture2D;
    auto* t = new Texture(kind);
    t->dimension = dimension;
    if (dimension == 1) {
        auto& d = *static_cast<const d3d::TEXTURE1D_DESC*>(raw);
        t->width = d.Width; t->mips = d.MipLevels; t->layers = d.ArraySize; t->dxgi = d.Format;
        t->usage = d.Usage; t->bind = d.BindFlags; t->cpu = d.CPUAccessFlags; t->misc = d.MiscFlags;
    } else if (dimension == 2) {
        auto& d = *static_cast<const d3d::TEXTURE2D_DESC*>(raw);
        t->width = d.Width; t->height = d.Height; t->mips = d.MipLevels; t->layers = d.ArraySize; t->dxgi = d.Format;
        t->samples = std::max(1u, d.SampleDesc.Count);
        t->usage = d.Usage; t->bind = d.BindFlags; t->cpu = d.CPUAccessFlags; t->misc = d.MiscFlags;
    } else {
        auto& d = *static_cast<const d3d::TEXTURE3D_DESC*>(raw);
        t->width = d.Width; t->height = d.Height; t->depth = d.Depth; t->mips = d.MipLevels; t->dxgi = d.Format;
        t->usage = d.Usage; t->bind = d.BindFlags; t->cpu = d.CPUAccessFlags; t->misc = d.MiscFlags;
    }
    if (!t->width || !t->height || !t->depth || !t->layers) { delete t; return d3d::HR_E_INVALIDARG; }
    uint32_t full = 1 + uint32_t(std::floor(std::log2(double(std::max({t->width, t->height, t->depth})))));
    if (!t->mips) t->mips = full;
    t->cube = (t->misc & d3d::MISC_TEXTURECUBE) != 0;
    const Format& f = format(t->dxgi);
    t->vk_format = f.image;
    t->aspect = f.depth ? f.aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    /* Typeless depth formats without depth binding are colour textures of the same size. */
    if (f.depth && !(t->bind & d3d::BIND_DEPTH_STENCIL) && f.typeless) {
        t->vk_format = f.view == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_FORMAT_R32G32_SFLOAT : f.view;
        t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (t->vk_format == VK_FORMAT_R32_SFLOAT && (t->bind & d3d::BIND_DEPTH_STENCIL)) { t->vk_format = VK_FORMAT_D32_SFLOAT; t->aspect = VK_IMAGE_ASPECT_DEPTH_BIT; }
    t->layouts.assign(size_t(t->mips) * t->layers, VK_IMAGE_LAYOUT_UNDEFINED);

    if (t->usage == d3d::USAGE_STAGING) {
        /* CPU copies: a linear buffer holds every subresource tightly packed. */
        VkDeviceSize total = 0;
        for (uint32_t layer = 0; layer < t->layers; ++layer)
            for (uint32_t mip = 0; mip < t->mips; ++mip) {
                uint32_t pitch = row_bytes(f, t->sub_width(mip));
                t->sub_offset.push_back(total);
                t->sub_pitch.push_back(pitch);
                total += VkDeviceSize(pitch) * row_count(f, t->sub_height(mip)) * t->sub_depth(mip);
                total = (total + 15) & ~VkDeviceSize(15);
            }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = std::max<VkDeviceSize>(total, 16);
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vk_check(vk.vkCreateBuffer(gpu.device, &info, nullptr, &t->linear), "vkCreateBuffer(staging texture)");
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(gpu.device, t->linear, &req);
        t->linear_memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        vk_check(vk.vkBindBufferMemory(gpu.device, t->linear, t->linear_memory.memory, t->linear_memory.offset), "bind staging texture");
        if (data) {
            for (uint32_t i = 0; i < t->mips * t->layers; ++i) {
                uint32_t mip = i % t->mips;
                if (!data[i].pSysMem) continue;
                uint32_t rows = row_count(f, t->sub_height(mip)) * t->sub_depth(mip);
                for (uint32_t r = 0; r < rows; ++r)
                    memcpy(t->linear_memory.mapped + t->sub_offset[i] + size_t(r) * t->sub_pitch[i],
                           reinterpret_cast<const uint8_t*>(data[i].pSysMem) + size_t(r) * data[i].SysMemPitch, t->sub_pitch[i]);
            }
        }
        *out = t;
        return d3d::HR_S_OK;
    }

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = dimension == 1 ? VK_IMAGE_TYPE_1D : dimension == 3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = t->vk_format;
    info.extent = {t->width, t->height, t->depth};
    info.mipLevels = t->mips;
    info.arrayLayers = t->layers;
    info.samples = VkSampleCountFlagBits(t->samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (t->bind & d3d::BIND_RENDER_TARGET) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (t->bind & d3d::BIND_DEPTH_STENCIL) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (t->bind & d3d::BIND_UNORDERED_ACCESS) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    /* A mutable format only where a view can differ from the image format, always with the list of
     * those formats.  Without a list Turnip lays a mutable colour image out linearly and without
     * UBWC (render targets and BC textures on Adreno); DXVK passes the DXGI family the same way.
     * Depth views use the image's own format, so depth images are never mutable. */
    VkFormat view_formats[8];
    uint32_t view_format_count = t->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? view_format_list(t->dxgi, t->vk_format, view_formats) : 0;
    VkImageFormatListCreateInfo format_list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (view_format_count > 1) {
        info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        format_list.viewFormatCount = view_format_count;
        format_list.pViewFormats = view_formats;
        info.pNext = &format_list;
    }
    if (t->cube && t->layers % 6 == 0) info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (dimension == 3) info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check_image_format(info.format, info.usage);
    VkResult r = vk.vkCreateImage(gpu.device, &info, nullptr, &t->image);
    if (r != VK_SUCCESS) {
        log("RDVK_TEXTURE_FAILED %ux%ux%u mips=%u layers=%u dxgi=%u vk=%d bind=0x%x: %s", t->width, t->height, t->depth, t->mips,
            t->layers, t->dxgi, int(t->vk_format), t->bind, vk_result(r));
        delete t;
        return d3d::HR_E_INVALIDARG;
    }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(gpu.device, t->image, &req);
    bool attachment = (t->bind & (d3d::BIND_RENDER_TARGET | d3d::BIND_DEPTH_STENCIL)) != 0;
    t->memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, attachment && req.size >= (4u << 20));
    vk_check(vk.vkBindImageMemory(gpu.device, t->image, t->memory.memory, t->memory.offset), "vkBindImageMemory");
    if (data) g_ctx.upload_texture_initial(t, data);
    *out = t;
    return d3d::HR_S_OK;
}

/* ------------------------------------------------------------------ views */
namespace {
VkImageViewType srv_view_type(const Texture* t, uint32_t dim) {
    switch (dim) {
    case d3d::SRV_TEXTURE1D: return VK_IMAGE_VIEW_TYPE_1D;
    case d3d::SRV_TEXTURE1DARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case d3d::SRV_TEXTURE2D: case d3d::SRV_TEXTURE2DMS: return VK_IMAGE_VIEW_TYPE_2D;
    case d3d::SRV_TEXTURE2DARRAY: case d3d::SRV_TEXTURE2DMSARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case d3d::SRV_TEXTURE3D: return VK_IMAGE_VIEW_TYPE_3D;
    case d3d::SRV_TEXTURECUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case d3d::SRV_TEXTURECUBEARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: break;
    }
    (void)t;
    return VK_IMAGE_VIEW_TYPE_2D;
}
VkFormat texture_view_format(const Texture* t, uint32_t dxgi, VkImageAspectFlags* aspect) {
    if (!dxgi) dxgi = t->dxgi;
    VkFormat f = view_format(dxgi, aspect);
    if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {   /* depth image: views read its depth (or stencil) aspect */
        f = t->vk_format;
        if (aspect && *aspect == VK_IMAGE_ASPECT_COLOR_BIT) *aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (aspect) {
        *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        if (format(dxgi).depth) f = t->vk_format;    /* typeless depth format used as colour texture */
    }
    return f;
}
VkImageView make_image_view(const Texture* t, VkImageViewType type, VkFormat f, const VkImageSubresourceRange& range, bool alpha_only) {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = t->image;
    info.viewType = type;
    info.format = f;
    info.subresourceRange = range;
    if (alpha_only) info.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    VkImageView view = VK_NULL_HANDLE;
    VkResult r = vk.vkCreateImageView(gpu.device, &info, nullptr, &view);
    if (r != VK_SUCCESS) stop("vkCreateImageView %s format=%d type=%d", vk_result(r), int(f), int(type));
    return view;
}
bool is_texture(const Resource* r) { return r && (r->kind == Kind::Texture1D || r->kind == Kind::Texture2D || r->kind == Kind::Texture3D); }
const char* view_type_name(VkImageViewType t) {
    static const char* const names[] = {"1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "CUBE_ARRAY"};
    return unsigned(t) < 7 ? names[t] : "?";
}
}  // namespace

VkImageView view_of_type(View* v, VkImageViewType type) {
    if (type == v->view_type || unsigned(type) >= 7 || !v->texture) return v->view;
    if (v->alt[type]) return v->alt[type];
    const Texture* t = v->texture;
    bool cube_image = t->cube && t->layers % 6 == 0;
    VkImageSubresourceRange r = v->range;
    VkImageViewType from = v->view_type;
    bool ok = false;
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: ok = from == VK_IMAGE_VIEW_TYPE_1D_ARRAY; r.layerCount = 1; break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = from == VK_IMAGE_VIEW_TYPE_1D; break;
    case VK_IMAGE_VIEW_TYPE_2D:
        ok = from == VK_IMAGE_VIEW_TYPE_2D_ARRAY || from == VK_IMAGE_VIEW_TYPE_CUBE || from == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        r.layerCount = 1;
        break;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        ok = from == VK_IMAGE_VIEW_TYPE_2D || from == VK_IMAGE_VIEW_TYPE_CUBE || from == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
        ok = cube_image && r.layerCount >= 6 && (from == VK_IMAGE_VIEW_TYPE_2D_ARRAY || from == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        r.layerCount = 6;
        break;
    default: break;   /* 3D and cube arrays: no compatible view */
    }
    char key[64];
    snprintf(key, sizeof(key), "view_type_%d_%d_%d", int(from), int(type), ok ? 1 : 0);
    if (first_time(key))
        log("RDVK_VIEW_TYPE a %s view (dxgi %u, %ux%u, layers %u) is bound to a %s shader slot: %s", view_type_name(from), v->dxgi,
            t->width, t->height, v->range.layerCount, view_type_name(type), ok ? "a view of the slot's type is used" : "incompatible, the slot reads zeros");
    if (!ok) return VK_NULL_HANDLE;
    v->alt[type] = make_image_view(t, type, v->format, r, v->kind == Kind::Srv && v->dxgi == 65);
    return v->alt[type];
}

uint32_t create_srv(Resource* resource, const d3d::SRV_DESC* desc, View** out) {
    *out = nullptr;
    auto* v = new View(Kind::Srv);
    v->resource = resource; add_ref(resource);
    if (desc) memcpy(v->desc, desc, sizeof(*desc));
    if (resource->kind == Kind::Buffer) {
        auto* b = static_cast<Buffer*>(resource);
        v->buffer = b;
        uint32_t first = desc ? desc->a : 0, count = desc ? desc->b : 0;
        uint32_t stride = b->desc.StructureByteStride ? b->desc.StructureByteStride : (desc && desc->Format ? format(desc->Format).bytes : 4);
        v->buffer_offset = VkDeviceSize(first) * stride;
        v->buffer_size = count ? VkDeviceSize(count) * stride : b->desc.ByteWidth - v->buffer_offset;
        v->dxgi = desc ? desc->Format : 0;
        if (v->dxgi && !b->dynamic) {
            VkBufferViewCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
            info.buffer = b->buffer; info.format = format(v->dxgi).view; info.offset = v->buffer_offset; info.range = v->buffer_size;
            vk_check(vk.vkCreateBufferView(gpu.device, &info, nullptr, &v->texel), "vkCreateBufferView");
        }
        *out = v;
        return d3d::HR_S_OK;
    }
    auto* t = static_cast<Texture*>(resource);
    v->texture = t;
    d3d::SRV_DESC d{};
    if (desc) d = *desc;
    else {
        d.Format = t->dxgi;
        d.ViewDimension = t->dimension == 1 ? (t->layers > 1 ? d3d::SRV_TEXTURE1DARRAY : d3d::SRV_TEXTURE1D)
                        : t->dimension == 3 ? d3d::SRV_TEXTURE3D
                        : t->cube ? (t->layers > 6 ? d3d::SRV_TEXTURECUBEARRAY : d3d::SRV_TEXTURECUBE)
                        : (t->layers > 1 ? d3d::SRV_TEXTURE2DARRAY : d3d::SRV_TEXTURE2D);
        d.a = 0; d.b = UINT32_MAX; d.c = 0; d.d = t->layers;
        memcpy(v->desc, &d, sizeof(d));
    }
    v->dxgi = d.Format ? d.Format : t->dxgi;
    VkImageAspectFlags aspect = 0;
    v->format = texture_view_format(t, v->dxgi, &aspect);
    v->view_type = srv_view_type(t, d.ViewDimension);
    v->range.aspectMask = aspect;
    v->range.baseMipLevel = d.a;
    v->range.levelCount = d.b == UINT32_MAX || d.b == 0 ? t->mips - d.a : d.b;
    switch (d.ViewDimension) {
    case d3d::SRV_TEXTURE1DARRAY: case d3d::SRV_TEXTURE2DARRAY: v->range.baseArrayLayer = d.c; v->range.layerCount = d.d; break;
    case d3d::SRV_TEXTURECUBE: v->range.layerCount = 6; break;
    case d3d::SRV_TEXTURECUBEARRAY: v->range.baseArrayLayer = d.c; v->range.layerCount = d.d * 6; break;
    case d3d::SRV_TEXTURE2DMSARRAY: v->range.baseArrayLayer = d.a; v->range.layerCount = d.b; v->range.baseMipLevel = 0; v->range.levelCount = 1; break;
    case d3d::SRV_TEXTURE2DMS: v->range.baseMipLevel = 0; v->range.levelCount = 1; v->range.layerCount = 1; break;
    default: v->range.layerCount = 1; break;
    }
    if (!v->range.layerCount) v->range.layerCount = 1;
    v->view = make_image_view(t, v->view_type, v->format, v->range, v->dxgi == 65);
    *out = v;
    return d3d::HR_S_OK;
}

uint32_t create_rtv(Resource* resource, const d3d::RTV_DESC* desc, View** out) {
    *out = nullptr;
    if (!is_texture(resource)) return d3d::HR_E_INVALIDARG;
    auto* t = static_cast<Texture*>(resource);
    auto* v = new View(Kind::Rtv);
    v->resource = resource; add_ref(resource); v->texture = t;
    d3d::RTV_DESC d{};
    if (desc) d = *desc; else { d.Format = t->dxgi; d.ViewDimension = t->layers > 1 ? d3d::RTV_TEXTURE2DARRAY : d3d::RTV_TEXTURE2D; d.c = t->layers; }
    memcpy(v->desc, &d, sizeof(d));
    v->dxgi = d.Format ? d.Format : t->dxgi;
    VkImageAspectFlags aspect = 0;
    v->format = texture_view_format(t, v->dxgi, &aspect);
    v->range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    v->range.levelCount = 1; v->range.layerCount = 1;
    switch (d.ViewDimension) {
    case d3d::RTV_TEXTURE2D: case d3d::RTV_TEXTURE1D: v->range.baseMipLevel = d.a; break;
    case d3d::RTV_TEXTURE2DARRAY: case d3d::RTV_TEXTURE1DARRAY: v->range.baseMipLevel = d.a; v->range.baseArrayLayer = d.b; v->range.layerCount = std::max(1u, d.c); break;
    case d3d::RTV_TEXTURE3D: v->range.baseMipLevel = d.a; break;
    default: break;
    }
    v->view_type = v->range.layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    v->width = t->sub_width(v->range.baseMipLevel);
    v->height = t->sub_height(v->range.baseMipLevel);
    v->view = make_image_view(t, v->view_type, v->format, v->range, false);
    *out = v;
    return d3d::HR_S_OK;
}

uint32_t create_dsv(Resource* resource, const d3d::DSV_DESC* desc, View** out) {
    *out = nullptr;
    if (!is_texture(resource)) return d3d::HR_E_INVALIDARG;
    auto* t = static_cast<Texture*>(resource);
    auto* v = new View(Kind::Dsv);
    v->resource = resource; add_ref(resource); v->texture = t;
    d3d::DSV_DESC d{};
    if (desc) d = *desc; else { d.Format = t->dxgi; d.ViewDimension = d3d::DSV_TEXTURE2D; }
    memcpy(v->desc, &d, sizeof(d));
    v->dxgi = d.Format ? d.Format : t->dxgi;
    v->format = t->vk_format;
    v->read_only_depth = d.Flags & d3d::DSV_READ_ONLY_DEPTH;
    v->read_only_stencil = d.Flags & d3d::DSV_READ_ONLY_STENCIL;
    v->range.aspectMask = t->aspect;
    v->range.levelCount = 1; v->range.layerCount = 1;
    if (d.ViewDimension == d3d::DSV_TEXTURE2D || d.ViewDimension == d3d::DSV_TEXTURE1D) v->range.baseMipLevel = d.a;
    else if (d.ViewDimension == d3d::DSV_TEXTURE2DARRAY || d.ViewDimension == d3d::DSV_TEXTURE1DARRAY) {
        v->range.baseMipLevel = d.a; v->range.baseArrayLayer = d.b; v->range.layerCount = std::max(1u, d.c);
    }
    v->view_type = v->range.layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    v->width = t->sub_width(v->range.baseMipLevel);
    v->height = t->sub_height(v->range.baseMipLevel);
    v->view = make_image_view(t, v->view_type, v->format, v->range, false);
    *out = v;
    return d3d::HR_S_OK;
}

uint32_t create_uav(Resource* resource, const d3d::UAV_DESC* desc, View** out) {
    *out = nullptr;
    auto* v = new View(Kind::Uav);
    v->resource = resource; add_ref(resource);
    if (desc) memcpy(v->desc, desc, sizeof(*desc));
    if (resource->kind == Kind::Buffer) {
        auto* b = static_cast<Buffer*>(resource);
        v->buffer = b;
        uint32_t first = desc ? desc->a : 0, count = desc ? desc->b : 0;
        uint32_t stride = b->desc.StructureByteStride ? b->desc.StructureByteStride : (desc && desc->Format ? format(desc->Format).bytes : 4);
        v->buffer_offset = VkDeviceSize(first) * stride;
        v->buffer_size = count ? VkDeviceSize(count) * stride : b->desc.ByteWidth - v->buffer_offset;
        v->dxgi = desc ? desc->Format : 0;
        if (v->dxgi) {
            VkBufferViewCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
            info.buffer = b->buffer; info.format = format(v->dxgi).view; info.offset = v->buffer_offset; info.range = v->buffer_size;
            vk_check(vk.vkCreateBufferView(gpu.device, &info, nullptr, &v->texel), "vkCreateBufferView(uav)");
        }
        *out = v;
        return d3d::HR_S_OK;
    }
    auto* t = static_cast<Texture*>(resource);
    v->texture = t;
    d3d::UAV_DESC d{};
    if (desc) d = *desc; else { d.Format = t->dxgi; d.ViewDimension = t->dimension == 3 ? d3d::UAV_TEXTURE3D : d3d::UAV_TEXTURE2D; }
    v->dxgi = d.Format ? d.Format : t->dxgi;
    VkImageAspectFlags aspect = 0;
    v->format = texture_view_format(t, v->dxgi, &aspect);
    v->range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    v->range.baseMipLevel = d.a; v->range.levelCount = 1; v->range.layerCount = 1;
    v->view_type = VK_IMAGE_VIEW_TYPE_2D;
    if (d.ViewDimension == d3d::UAV_TEXTURE2DARRAY) { v->range.baseArrayLayer = d.b; v->range.layerCount = std::max(1u, d.c); v->view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; }
    if (d.ViewDimension == d3d::UAV_TEXTURE3D) v->view_type = VK_IMAGE_VIEW_TYPE_3D;
    if (d.ViewDimension == d3d::UAV_TEXTURE1D) v->view_type = VK_IMAGE_VIEW_TYPE_1D;
    v->view = make_image_view(t, v->view_type, v->format, v->range, false);
    *out = v;
    return d3d::HR_S_OK;
}

/* ------------------------------------------------------------------ states */
namespace {
VkFilter filter_of(uint32_t bits) { return bits ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
VkSamplerAddressMode address_of(uint32_t mode) {
    switch (mode) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
}  // namespace
VkCompareOp compare_of(uint32_t func) {
    switch (func) {
    case 1: return VK_COMPARE_OP_NEVER; case 2: return VK_COMPARE_OP_LESS; case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL; case 5: return VK_COMPARE_OP_GREATER; case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS;
    }
}

uint32_t create_sampler(const d3d::SAMPLER_DESC& desc, Sampler** out) {
    auto* s = new Sampler();
    s->desc = desc;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    uint32_t filter = desc.Filter;
    bool anisotropic = (filter & 0x40) != 0, comparison = (filter & 0x80) != 0;
    info.magFilter = anisotropic ? VK_FILTER_LINEAR : filter_of(filter & 0x4);
    info.minFilter = anisotropic ? VK_FILTER_LINEAR : filter_of(filter & 0x10);
    info.mipmapMode = (anisotropic || (filter & 0x1)) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = address_of(desc.AddressU);
    info.addressModeV = address_of(desc.AddressV);
    info.addressModeW = address_of(desc.AddressW);
    info.mipLodBias = desc.MipLODBias;
    info.anisotropyEnable = anisotropic && gpu.caps.sampler_anisotropy;
    info.maxAnisotropy = std::min(float(std::max(1u, desc.MaxAnisotropy)), gpu.caps.max_anisotropy);
    info.compareEnable = comparison;
    info.compareOp = compare_of(desc.ComparisonFunc);
    info.minLod = desc.MinLOD;
    info.maxLod = desc.MaxLOD > 1000.0f ? VK_LOD_CLAMP_NONE : desc.MaxLOD;
    const float* c = desc.BorderColor;
    bool opaque = c[3] > 0.5f;
    bool white = c[0] > 0.5f && c[1] > 0.5f && c[2] > 0.5f;
    info.borderColor = opaque ? (white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    vk_check(vk.vkCreateSampler(gpu.device, &info, nullptr, &s->sampler), "vkCreateSampler");
    *out = s;
    return d3d::HR_S_OK;
}

namespace {
VkBlendFactor blend_factor(uint32_t b) {
    switch (b) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendFactor alpha_factor(uint32_t b) {
    VkBlendFactor f = blend_factor(b);
    if (f == VK_BLEND_FACTOR_SRC_COLOR) return VK_BLEND_FACTOR_SRC_ALPHA;
    if (f == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR) return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (f == VK_BLEND_FACTOR_DST_COLOR) return VK_BLEND_FACTOR_DST_ALPHA;
    if (f == VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR) return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    if (f == VK_BLEND_FACTOR_CONSTANT_COLOR) return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    if (f == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR) return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    if (f == VK_BLEND_FACTOR_SRC1_COLOR) return VK_BLEND_FACTOR_SRC1_ALPHA;
    if (f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR) return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    return f;
}
VkBlendOp blend_op(uint32_t o) {
    switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
}  // namespace

uint32_t create_blend(const d3d::BLEND_DESC& desc, BlendState** out) {
    auto* b = new BlendState();
    b->desc = desc;
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < 8; ++i) {
        const d3d::RENDER_TARGET_BLEND_DESC& r = desc.RenderTarget[desc.IndependentBlendEnable ? i : 0];
        VkPipelineColorBlendAttachmentState& a = b->attachments[i];
        a.blendEnable = r.BlendEnable;
        a.srcColorBlendFactor = blend_factor(r.SrcBlend);
        a.dstColorBlendFactor = blend_factor(r.DestBlend);
        a.colorBlendOp = blend_op(r.BlendOp);
        a.srcAlphaBlendFactor = alpha_factor(r.SrcBlendAlpha);
        a.dstAlphaBlendFactor = alpha_factor(r.DestBlendAlpha);
        a.alphaBlendOp = blend_op(r.BlendOpAlpha);
        a.colorWriteMask = r.RenderTargetWriteMask & 0xF;
        if (!a.blendEnable) {
            a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        if (!gpu.caps.dual_src_blend) {
            /* Without dualSrcBlend SRC1 factors read the first output (external-theories T02 f). */
            bool changed = false;
            for (VkBlendFactor* f : {&a.srcColorBlendFactor, &a.dstColorBlendFactor, &a.srcAlphaBlendFactor, &a.dstAlphaBlendFactor}) {
                VkBlendFactor single = *f == VK_BLEND_FACTOR_SRC1_COLOR ? VK_BLEND_FACTOR_SRC_COLOR
                                     : *f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
                                     : *f == VK_BLEND_FACTOR_SRC1_ALPHA ? VK_BLEND_FACTOR_SRC_ALPHA
                                     : *f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : *f;
                changed |= single != *f;
                *f = single;
            }
            if (changed && first_time("dual_src_blend")) log("RDVK_FALLBACK dualSrcBlend unsupported: SRC1 blend factors use the first output");
        }
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&a);
        for (size_t j = 0; j < sizeof(a); ++j) { key ^= bytes[j]; key *= 1099511628211ull; }
    }
    key ^= desc.AlphaToCoverageEnable; key *= 1099511628211ull;
    b->key = key;
    *out = b;
    return d3d::HR_S_OK;
}
uint32_t create_depth(const d3d::DEPTH_STENCIL_DESC& desc, DepthState** out) { auto* d = new DepthState(); d->desc = desc; *out = d; return d3d::HR_S_OK; }
uint32_t create_raster(const d3d::RASTERIZER_DESC& desc, RasterState** out) { auto* r = new RasterState(); r->desc = desc; *out = r; return d3d::HR_S_OK; }
uint32_t create_query(const d3d::QUERY_DESC& desc, Query** out) { auto* q = new Query(); q->desc = desc; *out = q; return d3d::HR_S_OK; }
}  // namespace rdvk
