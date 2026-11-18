/* Native objects of the Vulkan rendering device.  Each one is also a guest COM object: its first
 * qword is a guest vtable of sentinel addresses (rdvk_abi.h), so the engine can store it where it
 * stored D3D11 interface pointers and call AddRef/Release/GetDesc on it through the COM safety
 * net.  The hot path reads these structures directly. */
#pragma once
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "d3d.h"
#include "device.h"
#include "formats.h"

namespace rdvk {

enum class Kind : uint32_t {
    None, Device, DxgiDevice, Adapter, Factory, Context, SwapChain,
    Buffer, Texture1D, Texture2D, Texture3D, Srv, Rtv, Dsv, Uav,
    InputLayout, VertexShader, HullShader, DomainShader, GeometryShader, PixelShader, ComputeShader,
    BlendState, DepthState, RasterState, Sampler, Query, Count
};
const char* kind_name(Kind kind);
constexpr uint64_t OBJECT_MAGIC = UINT64_C(0x314a424f4b564452);  /* "RDVKOBJ1" */

struct Object {
    uint64_t vtable;                 /* guest-visible vtable (sentinels of this kind) */
    uint64_t magic = OBJECT_MAGIC;
    Kind kind;
    std::atomic<int32_t> refs{1};
    explicit Object(Kind k);
    Object(const Object&) = delete;
};
static_assert(offsetof(Object, vtable) == 0, "guest vtable first");

void add_ref(Object* o);
void release(Object* o);   /* destroys at zero */
/* Guest pointer -> object (stops on anything that is not one of ours). */
[[noreturn]] void bad_object(uint64_t guest);
inline Object* object_of(uint64_t guest) {
    auto* o = reinterpret_cast<Object*>(guest);
    if (__builtin_expect(!o || o->magic != OBJECT_MAGIC, 0)) bad_object(guest);
    return o;
}
template <class T> T* as(uint64_t guest) { return guest ? static_cast<T*>(object_of(guest)) : nullptr; }
inline uint64_t guest_of(const Object* o) { return reinterpret_cast<uint64_t>(o); }

/* ------------------------------------------------------------------ resources */
struct Resource : Object {
    using Object::Object;
    uint64_t last_use = 0;           /* serial of the last command buffer that referenced it */
    uint64_t last_write = 0;
};

struct Buffer : Resource {
    explicit Buffer() : Resource(Kind::Buffer) {}
    d3d::BUFFER_DESC desc{};
    VkBuffer buffer = VK_NULL_HANDLE;   /* device-local storage (DEFAULT/IMMUTABLE) or the current slice */
    Allocation memory;
    VkDeviceSize offset = 0;            /* offset of the current contents inside `buffer` */
    uint8_t* mapped = nullptr;          /* host pointer of the current contents (dynamic/staging) */
    int32_t chunk = -1;                 /* dynamic: stream chunk retained for the current contents */
    bool dynamic = false, staging = false;
    std::vector<uint8_t> shadow;        /* staging buffers: contents for CPU reads */
    void* geometry = nullptr;           /* mobile geometry: static index buffer state (geometry.cpp) */
    /* Dynamic contents that outlived their stream chunk move to an allocation of their own until
     * the next DISCARD (Context::compact_stream). */
    VkBuffer home = VK_NULL_HANDLE;
    Allocation home_memory;
    uint32_t discards = 0;              /* diagnostics: DISCARDs since the last RDVK_DYNAMIC line */
};

/* Tiny staging readbacks (the 1x1 auto-exposure luminance, copied and mapped for READ in the same
 * frame): copies go round-robin into private staging textures and Map returns the copy issued two
 * frames earlier, which the GPU has normally finished; the first copies keep the exact wait.  The
 * D3D11/DXVK path does the same in android/dxvk/bridge.cpp. */
constexpr uint32_t READBACK_RING = 3;
struct Texture;
struct ReadbackRing { Texture* ring[READBACK_RING] = {}; uint32_t next = 0, issued = 0; };

struct Texture : Resource {
    explicit Texture(Kind k) : Resource(k) {}
    uint32_t dimension = 2, width = 1, height = 1, depth = 1, mips = 1, layers = 1, samples = 1;
    uint32_t dxgi = 0, usage = 0, bind = 0, cpu = 0, misc = 0;
    bool cube = false;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImage image = VK_NULL_HANDLE;
    Allocation memory;
    std::vector<VkImageLayout> layouts; /* per subresource: layer * mips + mip */
    /* STAGING and DYNAMIC: linear host copy of every subresource */
    VkBuffer linear = VK_NULL_HANDLE;
    Allocation linear_memory;
    std::vector<VkDeviceSize> sub_offset;  /* offset of each subresource in `linear` */
    std::vector<uint32_t> sub_pitch;
    uint32_t mapped_count = 0;
    ReadbackRing* readback = nullptr;   /* tiny staging readback target: its private copies */
    bool readback_slot = false;         /* one of those copies */
    VkImageLayout layout(uint32_t mip, uint32_t layer) const { return layouts[layer * mips + mip]; }
    uint32_t sub_width(uint32_t mip) const { return std::max(1u, width >> mip); }
    uint32_t sub_height(uint32_t mip) const { return std::max(1u, height >> mip); }
    uint32_t sub_depth(uint32_t mip) const { return std::max(1u, depth >> mip); }
};

struct View : Object {
    explicit View(Kind k) : Object(k) {}
    Resource* resource = nullptr;        /* holds a reference */
    Texture* texture = nullptr;          /* == resource for texture views */
    Buffer* buffer = nullptr;            /* == resource for buffer views */
    uint8_t desc[24] = {};               /* the D3D11 view desc as created (GetDesc) */
    uint32_t dxgi = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageView view = VK_NULL_HANDLE;   /* image views */
    VkBufferView texel = VK_NULL_HANDLE; /* typed buffer views */
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageView alt[7] = {};             /* views of other compatible types, made on demand (view_of_type) */
    VkImageSubresourceRange range{};
    VkDeviceSize buffer_offset = 0, buffer_size = 0; /* raw/structured buffer views */
    bool read_only_depth = false, read_only_stencil = false;
    uint32_t width = 0, height = 0;      /* render target views: extent of the mip */
};
/* The image view of a texture view for a shader slot declared with view type `type`.  D3D11
 * drivers accept a Texture2D view in a Texture2DArray slot (and cube <-> 2D array); Vulkan
 * requires the declared type, and the Mali driver read zeros (empty scout portraits).  Like DXVK,
 * a view of the declared type over the same subresources is made once per view; an incompatible
 * type (a 3D view in a 2D slot) returns VK_NULL_HANDLE, so the slot reads zeros everywhere. */
VkImageView view_of_type(View* v, VkImageViewType type);

/* ------------------------------------------------------------------ states and shaders */
struct BlendState : Object {
    BlendState() : Object(Kind::BlendState) {}
    d3d::BLEND_DESC desc{};
    VkPipelineColorBlendAttachmentState attachments[8]{};
    uint64_t key = 0;   /* hash for pipeline keys */
};
struct DepthState : Object {
    DepthState() : Object(Kind::DepthState) {}
    d3d::DEPTH_STENCIL_DESC desc{};
};
struct RasterState : Object {
    RasterState() : Object(Kind::RasterState) {}
    d3d::RASTERIZER_DESC desc{};
};
struct Sampler : Object {
    Sampler() : Object(Kind::Sampler) {}
    d3d::SAMPLER_DESC desc{};
    VkSampler sampler = VK_NULL_HANDLE;
};

struct InputLayout : Object {
    InputLayout() : Object(Kind::InputLayout) {}
    struct Attribute { uint32_t location, binding, format_dxgi, offset; VkFormat format; };
    struct Binding { uint32_t binding; bool instanced; uint32_t step; };
    std::vector<Attribute> attributes;
    std::vector<Binding> bindings;
    uint64_t key = 0;
    void* geometry = nullptr;           /* mobile geometry: attribute locations (geometry.cpp) */
};

/* Frostpunk passes of the mobile pass policy (policy.cpp), by the DXBC SHA-1 of their shaders. */
enum PassRoleId : uint8_t {
    ROLE_NONE, ROLE_TONEMAP, ROLE_TONEMAP_OPAQUE, ROLE_OUTLINE_MASK, ROLE_OUTLINE_DILATE, ROLE_OUTLINE_COMPOSITE, ROLE_DEPTH_MINMAX
};

struct Shader : Object {
    explicit Shader(Kind k) : Object(k) {}
    uint8_t role = ROLE_NONE;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;   /* from the build-time library in the APK (shaders.cpp) */
    struct BindingInfo { VkDescriptorType type; uint32_t binding; VkImageViewType view; uint32_t slot; uint8_t space; };
    std::vector<BindingInfo> bindings;    /* resource bindings, `binding` = DXVK binding number */
    uint16_t cb_vec4[16] = {};            /* declared constant buffer sizes (vec4s) by slot, from dcl_constantbuffer */
    uint32_t input_mask = 0, output_mask = 0, patch_vertices = 0;
    std::vector<uint8_t> uniform_data;    /* immediate constant buffer */
    VkBuffer uniform = VK_NULL_HANDLE;
    Allocation uniform_memory;
    uint64_t hash = 0;
    char name[48] = {};
};

struct Query : Object {
    Query() : Object(Kind::Query) {}
    d3d::QUERY_DESC desc{};
    uint64_t serial = 0;   /* event: serial that must complete */
    bool issued = false;
    /* Occlusion: one Vulkan query per render pass of the Begin/End interval (Context::query_*). */
    std::vector<uint32_t> slots;
    bool running = false, open = false;
    uint64_t end_serial = 0;
};

/* ------------------------------------------------------------------ DXGI / device stand-ins */
struct SwapChainObject;
struct DeviceObject : Object { DeviceObject() : Object(Kind::Device) {} };
struct DxgiDeviceObject : Object { DxgiDeviceObject() : Object(Kind::DxgiDevice) {} };
struct AdapterObject : Object { AdapterObject() : Object(Kind::Adapter) {} };
struct FactoryObject : Object { FactoryObject() : Object(Kind::Factory) {} };
struct ContextObject : Object { ContextObject() : Object(Kind::Context) {} };

/* The singletons the engine sees as its D3D11 device and immediate context. */
extern DeviceObject* g_device;
extern DxgiDeviceObject* g_dxgi_device;
extern AdapterObject* g_adapter;
extern FactoryObject* g_factory;
extern ContextObject* g_context;
void objects_init();

/* Creation (D3D11 semantics; used by the COM layer and by native hooks). */
uint32_t create_buffer(const d3d::BUFFER_DESC& desc, const d3d::SUBRESOURCE_DATA* data, Buffer** out);
uint32_t create_texture(uint32_t dimension, const void* desc, const d3d::SUBRESOURCE_DATA* data, Texture** out);
uint32_t create_srv(Resource* resource, const d3d::SRV_DESC* desc, View** out);
uint32_t create_rtv(Resource* resource, const d3d::RTV_DESC* desc, View** out);
uint32_t create_dsv(Resource* resource, const d3d::DSV_DESC* desc, View** out);
uint32_t create_uav(Resource* resource, const d3d::UAV_DESC* desc, View** out);
uint32_t create_sampler(const d3d::SAMPLER_DESC& desc, Sampler** out);
uint32_t create_blend(const d3d::BLEND_DESC& desc, BlendState** out);
uint32_t create_depth(const d3d::DEPTH_STENCIL_DESC& desc, DepthState** out);
uint32_t create_raster(const d3d::RASTERIZER_DESC& desc, RasterState** out);
uint32_t create_shader(Kind kind, const uint8_t* bytecode, size_t size, Shader** out);
uint32_t create_input_layout(const d3d::INPUT_ELEMENT_DESC* elements, uint32_t count, const uint8_t* vs, size_t vs_size, InputLayout** out);
uint32_t create_query(const d3d::QUERY_DESC& desc, Query** out);
void destroy_object(Object* o);
/* Dynamic buffers alive (stream compaction); fn runs under the registry lock. */
void for_each_dynamic_buffer(const std::function<void(Buffer*)>& fn);
void release_home(Buffer* b);   /* frees the buffer's own allocation after the GPU used it */
}  // namespace rdvk
