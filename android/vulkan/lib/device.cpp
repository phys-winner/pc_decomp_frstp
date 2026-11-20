#include "formats.h"
#include "stability.h"
#include "device.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <system_error>
#include "rdvk.h"

namespace rdvk {
Vk vk;
Gpu gpu;
Gpu::~Gpu() { stop_submit_thread(); }

const char* vk_result(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "SUCCESS"; case VK_NOT_READY: return "NOT_READY"; case VK_TIMEOUT: return "TIMEOUT";
    case VK_SUBOPTIMAL_KHR: return "SUBOPTIMAL"; case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY"; case VK_ERROR_DEVICE_LOST: return "DEVICE_LOST";
    case VK_ERROR_OUT_OF_DATE_KHR: return "OUT_OF_DATE"; case VK_ERROR_SURFACE_LOST_KHR: return "SURFACE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED"; case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT"; default: return "OTHER";
    }
}
void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) stop("%s failed: %d %s", what, int(r), vk_result(r));
}

/* ------------------------------------------------------------------ allocator */
void Allocator::init(const VkPhysicalDeviceMemoryProperties& properties, VkDeviceSize granularity) {
    properties_ = properties;
    granularity_ = std::max<VkDeviceSize>(granularity, 1);
}
int Allocator::choose(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const {
    int fallback = -1;
    for (uint32_t i = 0; i < properties_.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = properties_.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        if ((f & preferred) == preferred) return int(i);
        if (fallback < 0) fallback = int(i);
    }
    return fallback;
}
Allocation Allocator::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, bool dedicated) {
    int type = choose(req.memoryTypeBits, required, preferred | required);
    if (type < 0) stop("no memory type for bits=0x%x required=0x%x", req.memoryTypeBits, required);
    bool host = properties_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr VkDeviceSize BLOCK = 64ull << 20;
    Allocation out;
    out.type = uint32_t(type);
    if (dedicated || req.size >= BLOCK / 2) {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = req.size;
        info.memoryTypeIndex = uint32_t(type);
        vk_check(vk.vkAllocateMemory(gpu.device, &info, nullptr, &out.memory), "vkAllocateMemory(dedicated)");
        out.size = req.size;
        if (host) { void* p = nullptr; vk_check(vk.vkMapMemory(gpu.device, out.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory"); out.mapped = static_cast<uint8_t*>(p); }
        allocated_ += req.size; used_ += req.size;
        return out;
    }
    VkDeviceSize alignment = std::max(req.alignment, granularity_);
    std::lock_guard lock(mutex_);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t b = 0; b < blocks_.size(); ++b) {
            Block& block = blocks_[b];
            if (block.type != uint32_t(type) || !block.memory) continue;
            for (size_t r = 0; r < block.free.size(); ++r) {
                Range range = block.free[r];
                VkDeviceSize start = (range.offset + alignment - 1) / alignment * alignment;
                if (start + req.size > range.offset + range.size) continue;
                VkDeviceSize end = start + req.size;
                block.free.erase(block.free.begin() + long(r));
                if (end < range.offset + range.size) block.free.insert(block.free.begin() + long(r), Range{end, range.offset + range.size - end});
                if (start > range.offset) block.free.insert(block.free.begin() + long(r), Range{range.offset, start - range.offset});
                out.memory = block.memory; out.offset = start; out.size = req.size; out.block = int32_t(b);
                out.mapped = block.mapped ? block.mapped + start : nullptr;
                used_ += req.size;
                return out;
            }
        }
        Block block;
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = BLOCK;
        info.memoryTypeIndex = uint32_t(type);
        vk_check(vk.vkAllocateMemory(gpu.device, &info, nullptr, &block.memory), "vkAllocateMemory(block)");
        if (host) { void* p = nullptr; vk_check(vk.vkMapMemory(gpu.device, block.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory"); block.mapped = static_cast<uint8_t*>(p); }
        block.size = BLOCK; block.type = uint32_t(type); block.free.push_back(Range{0, BLOCK});
        allocated_ += BLOCK;
        auto empty = std::find_if(blocks_.begin(), blocks_.end(), [](const Block& b) { return !b.memory; });
        if (empty != blocks_.end()) *empty = std::move(block); else blocks_.push_back(std::move(block));
    }
    stop("allocator: no space for %llu bytes", (unsigned long long)req.size);
}
void Allocator::release(const Allocation& a) {
    if (!a.memory) return;
    if (a.block < 0) {
        vk.vkFreeMemory(gpu.device, a.memory, nullptr);
        allocated_ -= a.size; used_ -= a.size;
        return;
    }
    std::lock_guard lock(mutex_);
    Block& block = blocks_.at(size_t(a.block));
    used_ -= a.size;
    auto it = std::lower_bound(block.free.begin(), block.free.end(), a.offset, [](const Range& r, VkDeviceSize o) { return r.offset < o; });
    it = block.free.insert(it, Range{a.offset, a.size});
    if (it + 1 != block.free.end() && it->offset + it->size == (it + 1)->offset) { it->size += (it + 1)->size; block.free.erase(it + 1); }
    if (it != block.free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) { (it - 1)->size += it->size; block.free.erase(it); }
}
void Allocator::destroy() {
    for (Block& b : blocks_) if (b.memory) vk.vkFreeMemory(gpu.device, b.memory, nullptr);
    blocks_.clear();
}

/* ------------------------------------------------------------------ ring */
void Ring::init(VkBufferUsageFlags usage, VkDeviceSize chunk, VkDeviceSize alignment) { usage_ = usage; chunk_ = chunk; alignment_ = alignment; }
int32_t Ring::make(VkDeviceSize size) {
    int32_t index;
    if (!empty_.empty()) { index = empty_.back(); empty_.pop_back(); }
    else { chunks_.emplace_back(); index = int32_t(chunks_.size() - 1); }
    Chunk& c = chunks_[size_t(index)];
    c.used = 0;
    c.refs.store(0, std::memory_order_relaxed);
    c.serial.store(0, std::memory_order_relaxed);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size; info.usage = usage_; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vk.vkCreateBuffer(gpu.device, &info, nullptr, &c.buffer), "vkCreateBuffer(ring)");
    VkMemoryRequirements req; vk.vkGetBufferMemoryRequirements(gpu.device, c.buffer, &req);
    c.memory = gpu.memory.allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true);
    vk_check(vk.vkBindBufferMemory(gpu.device, c.buffer, c.memory.memory, c.memory.offset), "vkBindBufferMemory(ring)");
    c.size = size;
    bytes_ += size;
    return index;
}
Slice Ring::allocate(VkDeviceSize size, VkDeviceSize alignment) {
    if (!alignment) alignment = alignment_;
    std::lock_guard lock(mutex_);
    if (active_ < 0 || (chunks_[size_t(active_)].used + alignment - 1) / alignment * alignment + size > chunks_[size_t(active_)].size) {
        if (active_ >= 0) retired_.push_back(active_);   /* serial set at frame_end */
        VkDeviceSize want = std::max(chunk_, (size + chunk_ - 1) / chunk_ * chunk_);
        /* Most recently idled first: chunks that stay idle age out and are freed (frame_done). */
        auto it = std::find_if(idle_.rbegin(), idle_.rend(), [&](int32_t i) { return chunks_[size_t(i)].size >= want; });
        if (it != idle_.rend()) { active_ = *it; idle_.erase(std::next(it).base()); }
        else active_ = make(want);
        chunks_[size_t(active_)].used = 0;
    }
    Chunk& c = chunks_[size_t(active_)];
    c.serial.store(UINT64_MAX, std::memory_order_relaxed);   /* used by the commands being recorded */
    VkDeviceSize offset = (c.used + alignment - 1) / alignment * alignment;
    c.used = offset + size;
    return Slice{c.buffer, offset, size, c.memory.mapped + offset, active_};
}
void Ring::frame_end(uint64_t serial) {
    std::lock_guard lock(mutex_);
    for (int32_t i : retired_) if (chunks_[size_t(i)].serial == UINT64_MAX) chunks_[size_t(i)].serial = serial;
    /* The active chunk stays active across submissions until it is full; its serial is the last
     * submission that used it. */
    if (active_ >= 0 && chunks_[size_t(active_)].serial == UINT64_MAX) chunks_[size_t(active_)].serial = serial;
}
void Ring::frame_done(uint64_t completed) {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < retired_.size();) {
        Chunk& c = chunks_[size_t(retired_[i])];
        /* refs first: a release bumps the serial before it drops its reference. */
        if (c.refs.load(std::memory_order_acquire) <= 0 && c.serial.load(std::memory_order_acquire) <= completed) {
            c.idle_since = completed;
            idle_.push_back(retired_[i]); retired_.erase(retired_.begin() + long(i));
        } else ++i;
    }
    /* Chunks idle for 600 submissions go back to the allocator (load-time peaks, the chunks of
     * big DISCARDs); two stay for the next spike. */
    size_t keep = 2;
    for (size_t i = 0; i < idle_.size();) {
        Chunk& c = chunks_[size_t(idle_[i])];
        if (c.size > chunk_ || (idle_.size() > keep && completed > c.idle_since + 600)) {
            vk.vkDestroyBuffer(gpu.device, c.buffer, nullptr);
            gpu.memory.release(c.memory);
            bytes_ -= c.size;
            c.buffer = VK_NULL_HANDLE; c.memory = Allocation{}; c.size = 0; c.used = 0;
            empty_.push_back(idle_[i]);
            idle_.erase(idle_.begin() + long(i));
        } else ++i;
    }
}
void Ring::release(int32_t chunk, uint64_t serial) {
    if (chunk < 0) return;
    Chunk& c = chunks_[size_t(chunk)];
    uint64_t current = c.serial.load(std::memory_order_relaxed);
    while (current < serial && !c.serial.compare_exchange_weak(current, serial, std::memory_order_release, std::memory_order_relaxed)) {}
    c.refs.fetch_sub(1, std::memory_order_release);
}
void Ring::pinned(uint64_t completed, std::vector<int32_t>& out) {
    std::lock_guard lock(mutex_);
    for (int32_t i : retired_) {
        const Chunk& c = chunks_[size_t(i)];
        if (c.refs.load(std::memory_order_acquire) > 0 && c.serial.load(std::memory_order_acquire) <= completed) out.push_back(i);
    }
}
void Ring::log_chunks() {
    std::lock_guard lock(mutex_);
    std::string text;
    char item[64];
    for (size_t i = 0; i < chunks_.size(); ++i) {
        const Chunk& c = chunks_[i];
        if (!c.size) continue;
        bool idle = std::find(idle_.begin(), idle_.end(), int32_t(i)) != idle_.end();
        snprintf(item, sizeof(item), " %zu:%lluMB/%s/r%d", i, (unsigned long long)(c.size >> 20),
                 int32_t(i) == active_ ? "active" : idle ? "idle" : "retired", c.refs.load(std::memory_order_relaxed));
        text += item;
    }
    log("RDVK_RING%s", text.c_str());
}
Ring::Stats Ring::stats() {
    std::lock_guard lock(mutex_);
    Stats s;
    s.chunks = uint32_t(chunks_.size() - empty_.size());
    s.idle = uint32_t(idle_.size());
    for (int32_t i : retired_) s.pinned += chunks_[size_t(i)].refs.load(std::memory_order_relaxed) > 0;
    return s;
}
void Ring::destroy() {
    for (Chunk& c : chunks_) { vk.vkDestroyBuffer(gpu.device, c.buffer, nullptr); gpu.memory.release(c.memory); }
    chunks_.clear();
}

/* ------------------------------------------------------------------ device */
static bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return !strcmp(e.extensionName, name); });
}

bool Gpu::init() {
    vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(host->vk_get_instance_proc_addr());
    if (!vk.vkGetInstanceProcAddr) { log("RDVK_GPU no Vulkan provider"); return false; }
    vk.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vk.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    vk.vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        vk.vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Frostpunk";
    app.pEngineName = "Liquid Engine Vulkan (static recompilation port)";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instance_extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = instance_extensions;
    VkResult r = vk.vkCreateInstance(&info, nullptr, &instance);
    if (r != VK_SUCCESS) { log("RDVK_GPU vkCreateInstance %s", vk_result(r)); return false; }
#define RDVK_LOAD_I(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(instance, #name));
    RDVK_VK_INSTANCE(RDVK_LOAD_I)
#undef RDVK_LOAD_I
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count) { log("RDVK_GPU no physical device"); return false; }
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(instance, &count, devices.data());
    physical = devices[0];

    VkPhysicalDevicePushDescriptorPropertiesKHR push{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &push};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vk.vkGetPhysicalDeviceProperties2(physical, &props);
    const VkPhysicalDeviceLimits& limits = props.properties.limits;
    caps.vendor = props.properties.vendorID; caps.device_id = props.properties.deviceID;
    caps.driver_id = driver.driverID; caps.api = props.properties.apiVersion;
    caps.min_uniform_offset = limits.minUniformBufferOffsetAlignment;
    caps.min_storage_offset = limits.minStorageBufferOffsetAlignment;
    caps.non_coherent_atom = limits.nonCoherentAtomSize;
    caps.timestamp_period = limits.timestampPeriod;
    caps.max_anisotropy = limits.maxSamplerAnisotropy;
    snprintf(caps.name, sizeof(caps.name), "%s", props.properties.deviceName);
    if (props.properties.apiVersion < VK_API_VERSION_1_3) { log("RDVK_GPU needs Vulkan 1.3 (%s)", caps.name); return false; }

    std::vector<VkExtensionProperties> extensions;
    vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    extensions.resize(count);
    vk.vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());

    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &eds3};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, &robust2};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &clip};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vk.vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!f13.dynamicRendering || !f13.synchronization2) { log("RDVK_GPU missing dynamic rendering/synchronization2"); return false; }

    /* Enable exactly what the port uses. */
    VkPhysicalDeviceFeatures& core = features.features;
    VkPhysicalDeviceFeatures want{};
    want.robustBufferAccess = core.robustBufferAccess;
    want.tessellationShader = core.tessellationShader;
    want.geometryShader = core.geometryShader;
    want.depthClamp = core.depthClamp;
    want.depthBiasClamp = core.depthBiasClamp;
    want.samplerAnisotropy = core.samplerAnisotropy;
    want.independentBlend = core.independentBlend;
    want.dualSrcBlend = core.dualSrcBlend;
    want.imageCubeArray = core.imageCubeArray;
    want.fragmentStoresAndAtomics = core.fragmentStoresAndAtomics;
    want.shaderStorageImageWriteWithoutFormat = core.shaderStorageImageWriteWithoutFormat;
    want.shaderStorageImageReadWithoutFormat = core.shaderStorageImageReadWithoutFormat;
    want.shaderImageGatherExtended = core.shaderImageGatherExtended;
    want.textureCompressionASTC_LDR = core.textureCompressionASTC_LDR;
    want.textureCompressionBC = core.textureCompressionBC;
    want.occlusionQueryPrecise = core.occlusionQueryPrecise;
    want.fullDrawIndexUint32 = core.fullDrawIndexUint32;
    want.drawIndirectFirstInstance = core.drawIndirectFirstInstance;
    want.multiDrawIndirect = core.multiDrawIndirect;
    want.shaderSampledImageArrayDynamicIndexing = core.shaderSampledImageArrayDynamicIndexing;
    want.shaderInt16 = core.shaderInt16;
    want.vertexPipelineStoresAndAtomics = core.vertexPipelineStoresAndAtomics;
    caps.tessellation = want.tessellationShader; caps.geometry = want.geometryShader; caps.bc = want.textureCompressionBC;
    caps.astc = want.textureCompressionASTC_LDR; caps.depth_clamp = want.depthClamp; caps.sampler_anisotropy = want.samplerAnisotropy;
    caps.independent_blend = want.independentBlend;
    caps.depth_bias_clamp = want.depthBiasClamp; caps.dual_src_blend = want.dualSrcBlend;
    caps.occlusion_precise = want.occlusionQueryPrecise;
    caps.shader_features =
        (core.tessellationShader ? SHADER_FEATURE_TESSELLATION : 0) | (core.geometryShader ? SHADER_FEATURE_GEOMETRY : 0) |
        (core.shaderStorageImageReadWithoutFormat ? SHADER_FEATURE_READ_WITHOUT_FORMAT : 0) |
        (core.shaderStorageImageWriteWithoutFormat ? SHADER_FEATURE_WRITE_WITHOUT_FORMAT : 0) |
        (f11.shaderDrawParameters ? SHADER_FEATURE_DRAW_PARAMETERS : 0) | (f12.vulkanMemoryModel ? SHADER_FEATURE_MEMORY_MODEL : 0) |
        (f13.shaderDemoteToHelperInvocation ? SHADER_FEATURE_DEMOTE : 0) | (core.imageCubeArray ? SHADER_FEATURE_CUBE_ARRAY : 0) |
        (core.shaderImageGatherExtended ? SHADER_FEATURE_GATHER_EXTENDED : 0) | (core.shaderInt16 ? SHADER_FEATURE_INT16 : 0) |
        (f12.shaderFloat16 ? SHADER_FEATURE_FLOAT16 : 0) | (core.sampleRateShading ? SHADER_FEATURE_SAMPLE_RATE : 0) |
        (core.shaderClipDistance ? SHADER_FEATURE_CLIP_DISTANCE : 0) | (core.shaderCullDistance ? SHADER_FEATURE_CULL_DISTANCE : 0) |
        (core.multiViewport ? SHADER_FEATURE_MULTI_VIEWPORT : 0) | (core.shaderInt64 ? SHADER_FEATURE_INT64 : 0) |
        (core.shaderFloat64 ? SHADER_FEATURE_FLOAT64 : 0);
    {   /* diagnostics: debug.fp.rdvk_shader_mask=0xN hides features to exercise the fallbacks */
        std::string mask = property("debug.fp.rdvk_shader_mask");
        if (!mask.empty()) caps.shader_features &= ~uint32_t(strtoul(mask.c_str(), nullptr, 0));
    }
    want.sampleRateShading = core.sampleRateShading;
    want.shaderClipDistance = core.shaderClipDistance;
    want.shaderCullDistance = core.shaderCullDistance;
    want.multiViewport = core.multiViewport;
    want.shaderInt64 = core.shaderInt64;
    want.shaderFloat64 = core.shaderFloat64;
    core = want;
    VkPhysicalDeviceVulkan11Features w11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    w11.shaderDrawParameters = f11.shaderDrawParameters;
    VkPhysicalDeviceVulkan12Features w12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    w12.hostQueryReset = f12.hostQueryReset;
    caps.host_query_reset = f12.hostQueryReset;
    w12.timelineSemaphore = f12.timelineSemaphore;
    w12.shaderFloat16 = f12.shaderFloat16;
    w12.uniformBufferStandardLayout = f12.uniformBufferStandardLayout;
    w12.scalarBlockLayout = f12.scalarBlockLayout;
    w12.vulkanMemoryModel = f12.vulkanMemoryModel;
    w12.descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound;
    w12.samplerFilterMinmax = f12.samplerFilterMinmax;
    VkPhysicalDeviceVulkan13Features w13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    w13.dynamicRendering = VK_TRUE;
    w13.synchronization2 = VK_TRUE;
    w13.maintenance4 = f13.maintenance4;
    w13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;
    w13.shaderTerminateInvocation = f13.shaderTerminateInvocation;
    w13.robustImageAccess = f13.robustImageAccess;
    w13.shaderZeroInitializeWorkgroupMemory = f13.shaderZeroInitializeWorkgroupMemory;
    features.pNext = &w11; w11.pNext = &w12; w12.pNext = &w13; w13.pNext = nullptr;

    std::vector<const char*> device_extensions = {"VK_KHR_swapchain"};
    void** chain = &w13.pNext;
    VkPhysicalDeviceRobustness2FeaturesEXT wr2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    if (has_extension(extensions, "VK_EXT_robustness2") && robust2.nullDescriptor) {
        device_extensions.push_back("VK_EXT_robustness2");
        wr2.nullDescriptor = VK_TRUE;
        *chain = &wr2; chain = &wr2.pNext;
        caps.robustness2_null = true;
    }
    VkPhysicalDeviceDepthClipEnableFeaturesEXT wclip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
    if (has_extension(extensions, "VK_EXT_depth_clip_enable") && clip.depthClipEnable) {
        device_extensions.push_back("VK_EXT_depth_clip_enable");
        wclip.depthClipEnable = VK_TRUE;
        caps.depth_clip = true;
        *chain = &wclip; chain = &wclip.pNext;
    }
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT weds3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (has_extension(extensions, "VK_EXT_extended_dynamic_state3") && (eds3.extendedDynamicState3DepthClampEnable || eds3.extendedDynamicState3DepthClipEnable)) {
        device_extensions.push_back("VK_EXT_extended_dynamic_state3");
        weds3.extendedDynamicState3DepthClampEnable = eds3.extendedDynamicState3DepthClampEnable && want.depthClamp;
        weds3.extendedDynamicState3DepthClipEnable = eds3.extendedDynamicState3DepthClipEnable && wclip.depthClipEnable;
        caps.extended_dynamic_state3_clamp = weds3.extendedDynamicState3DepthClampEnable;
        caps.extended_dynamic_state3_clip = weds3.extendedDynamicState3DepthClipEnable;
        *chain = &weds3; chain = &weds3.pNext;
    }
    if (has_extension(extensions, "VK_KHR_push_descriptor")) {
        device_extensions.push_back("VK_KHR_push_descriptor");
        caps.push_descriptor = true;
        caps.max_push_descriptors = push.maxPushDescriptors;
    }
    for (const char* name : {"VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8"})
        if (has_extension(extensions, name)) { caps.index_uint8 = false; }  /* not needed by the engine */

    uint32_t families = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, fam.data());
    family = UINT32_MAX;
    for (uint32_t i = 0; i < families; ++i)
        if ((fam[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
    if (family == UINT32_MAX) { log("RDVK_GPU no graphics+compute queue"); return false; }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features};
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = uint32_t(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    r = vk.vkCreateDevice(physical, &device_info, nullptr, &device);
    if (r != VK_SUCCESS) { log("RDVK_GPU vkCreateDevice %s", vk_result(r)); return false; }
#define RDVK_LOAD_D(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device, #name));
    RDVK_VK_DEVICE(RDVK_LOAD_D)
#undef RDVK_LOAD_D
    {
        /* Experiments: run the paths of drivers without push descriptors (4) or nullDescriptor (8). */
        int flags = atoi(property("debug.fp.rdvk_debug").c_str());
        if (flags & 4) { caps.push_descriptor = false; log("RDVK_DEBUG push descriptors off"); }
        if (flags & 8) { caps.robustness2_null = false; log("RDVK_DEBUG null descriptors off (stand-ins)"); }
        /* The Mali-G615 driver (r?? on Vulkan 1.3.247) did not keep dynamic state (cull mode, front
         * face...) across vkCmdBeginRendering: the first draw of a pass that inherited them from the
         * previous pass was culled.  The HUD's mask draws are single draws in their own pass, so the
         * scout portraits vanished and a masked arc showed in the temperature dial.  Re-set the
         * state at every pass on Mali; debug.fp.rdvk_dynamic_reset=0|1 overrides. */
        std::string reset = property("debug.fp.rdvk_dynamic_reset");
        caps.reset_dynamic_per_pass = reset == "1" || (reset != "0" && caps.vendor == 0x13b5);
    }
    if (!caps.push_descriptor) vk.vkCmdPushDescriptorSetKHR = nullptr;
    {
        /* PowerVR: the compiler is not thread safe (external-theories T05 e); everywhere after a
         * crashed native session (safe mode level 1). debug.fp.rdvk_serialize=0|1 overrides. */
        std::string serial = property("debug.fp.rdvk_serialize");
        if (serial != "0" && (serial == "1" || caps.vendor == 0x1010 || guard_level() >= 1))
            serialize_driver(serial == "1" ? "debug.fp.rdvk_serialize=1" : caps.vendor == 0x1010 ? "PowerVR" : "safe mode level 1");
    }
    vk.vkCmdSetDepthClampEnableEXT = reinterpret_cast<PFN_vkCmdSetDepthClampEnableEXT>(vk.vkGetDeviceProcAddr(device, "vkCmdSetDepthClampEnableEXT"));
    vk.vkCmdSetDepthClipEnableEXT = reinterpret_cast<PFN_vkCmdSetDepthClipEnableEXT>(vk.vkGetDeviceProcAddr(device, "vkCmdSetDepthClipEnableEXT"));
    vk.vkGetDeviceQueue(device, family, 0, &queue);
    vk.vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    memory.init(memory_properties, limits.bufferImageGranularity);
    stream.init(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 8ull << 20,
                std::max<VkDeviceSize>({caps.min_uniform_offset, caps.min_storage_offset, 16}));
    staging.init(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 32ull << 20, 64);
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = family;
        vk_check(vk.vkCreateCommandPool(device, &pool, nullptr, &f.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = f.pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 2;
        VkCommandBuffer buffers[2];
        vk_check(vk.vkAllocateCommandBuffers(device, &alloc, buffers), "vkAllocateCommandBuffers");
        f.cmd = buffers[0]; f.upload = buffers[1];
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vk.vkCreateFence(device, &fence, nullptr, &f.fence), "vkCreateFence");
    }
    formats_fallbacks();
    begin_frame();
    if (caps.vendor == 0x1010 && guard_level() == 0) {
        caps.defer_attachment_transitions = true;
        try {
            submit_thread_ = std::thread([this] { submit_loop(); });
            log("RDVK_QUEUE async=1");
        } catch (const std::system_error& e) {
            log("RDVK_QUEUE synchronous fallback: %s", e.what());
        }
    }
    log("RDVK_FEATURES depth_bias_clamp=%d dual_src_blend=%d depth_clip=%d independent_blend=%d depth_clamp=%d dynamic_reset_per_pass=%d",
        caps.depth_bias_clamp, caps.dual_src_blend, caps.depth_clip, caps.independent_blend, caps.depth_clamp, caps.reset_dynamic_per_pass);
    log("RDVK_SHADER_FEATURES supported=0x%05x (tess geom read_wo_fmt write_wo_fmt draw_params memory_model demote cube_array gather_ext int16 "
        "float16 sample_rate clip cull multi_viewport int64 float64)", caps.shader_features);
    log("RDVK_GPU device=%s vendor=0x%x driver_id=%u api=%u.%u.%u push_descriptors=%d(%u) null_descriptor=%d tessellation=%d "
        "geometry=%d bc=%d astc=%d eds3_clamp=%d eds3_clip=%d",
        caps.name, caps.vendor, caps.driver_id, VK_API_VERSION_MAJOR(caps.api), VK_API_VERSION_MINOR(caps.api), VK_API_VERSION_PATCH(caps.api),
        caps.push_descriptor, caps.max_push_descriptors, caps.robustness2_null, caps.tessellation, caps.geometry, caps.bc, caps.astc,
        caps.extended_dynamic_state3_clamp, caps.extended_dynamic_state3_clip);
    return true;
}

void Gpu::begin_frame() {
    Frame& f = frame();
    wait_submitted(f.serial);
    if (vk.vkGetFenceStatus(device, f.fence) != VK_SUCCESS) wait_fence(f.fence, "vkWaitForFences(frame)");
    if (f.serial > completed_.load()) completed_.store(f.serial, std::memory_order_release);
    std::vector<std::function<void()>> garbage;
    { std::lock_guard lock(garbage_mutex_); garbage.swap(f.garbage); }
    for (auto& g : garbage) g();
    stream.frame_done(completed());
    staging.frame_done(completed());
    vk_check(vk.vkResetCommandPool(device, f.pool, 0), "vkResetCommandPool");
    f.serial = serial_;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vk.vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer");
}

void Gpu::upload(std::function<void(VkCommandBuffer)> record) {
    uploads_.push_back(UploadJob{VK_NULL_HANDLE, VK_NULL_HANDLE, VkBufferCopy{}, std::move(record)});
}

void Gpu::upload_copy(VkBuffer src, VkBuffer dst, const VkBufferCopy& region) {
    uploads_.push_back(UploadJob{src, dst, region, nullptr});
}

void Gpu::wait_fence(VkFence fence, const char* what) {
    unsigned interrupted = 0, timeouts = 0;
    for (;;) {
        VkResult r = vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (r == VK_SUCCESS) return;
        /* Mali returns VK_ERROR_INITIALIZATION_FAILED when a signal interrupts the wait (ppoll is
         * not restarted; external-theories T05 a, from Skyline's fence_cycle.h via doom-port). */
        if (r == VK_ERROR_INITIALIZATION_FAILED && ++interrupted <= 1000) {
            if (interrupted <= 4) log("RDVK_WAIT_RETRY %s interrupted (VK_ERROR_INITIALIZATION_FAILED) retry=%u", what, interrupted);
            continue;
        }
        /* An unbounded wait that times out: a GPU hang (Turnip/KGSL) unless it clears. */
        if (r == VK_TIMEOUT && ++timeouts <= 3 && vk.vkGetFenceStatus(device, fence) != VK_ERROR_DEVICE_LOST) {
            log("RDVK_WAIT_RETRY %s VK_TIMEOUT retry=%u", what, timeouts);
            continue;
        }
        stop("%s failed: %d %s (GPU hang or device lost)", what, int(r), vk_result(r));
    }
}

void Gpu::submit(VkSemaphore wait, VkPipelineStageFlags wait_stage, VkSemaphore signal, std::function<void()> present) {
    Frame& f = frame();
    Submission job;
    job.wait = wait; job.wait_stage = wait_stage; job.signal = signal; job.fence = f.fence;
    job.serial = serial_;
    job.present = std::move(present);
    {
        std::lock_guard lock(upload_mutex_);
        if (!uploads_.empty()) {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk_check(vk.vkBeginCommandBuffer(f.upload, &begin), "vkBeginCommandBuffer(upload)");
            for (UploadJob& job : uploads_) {
                if (job.record) job.record(f.upload);
                else vk.vkCmdCopyBuffer(f.upload, job.src, job.dst, 1, &job.region);
            }
            uploads_.clear();
            /* Uploads finish before any command of the frame reads them. */
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            vk.vkCmdPipelineBarrier(f.upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            vk_check(vk.vkEndCommandBuffer(f.upload), "vkEndCommandBuffer(upload)");
            job.buffers[job.count++] = f.upload;
        }
        vk_check(vk.vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");
        job.buffers[job.count++] = f.cmd;
        stream.frame_end(serial_);
        staging.frame_end(serial_);
        vk_check(vk.vkResetFences(device, 1, &f.fence), "vkResetFences");
        if (submit_thread_.joinable()) {
            {
                std::lock_guard queue_lock(submissions_mutex_);
                assert(submissions_.size() < FRAMES);
                submissions_.push_back(std::move(job));
            }
            submissions_ready_.notify_all();
        } else {
            submit_now(job);
            submitted_.store(job.serial, std::memory_order_release);
        }
        ++serial_;
        begin_frame();
    }
}

void Gpu::submit_now(Submission& job) {
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = job.count; info.pCommandBuffers = job.buffers;
    if (job.wait) {
        info.waitSemaphoreCount = 1; info.pWaitSemaphores = &job.wait; info.pWaitDstStageMask = &job.wait_stage;
    }
    if (job.signal) { info.signalSemaphoreCount = 1; info.pSignalSemaphores = &job.signal; }
    std::lock_guard lock(queue_mutex_);
    vk_check(vk.vkQueueSubmit(queue, 1, &info, job.fence), "vkQueueSubmit");
    if (job.present) job.present();
}

void Gpu::submit_loop() {
    pthread_setname_np(pthread_self(), "rdvk-queue");
    std::unique_lock lock(submissions_mutex_);
    for (;;) {
        submissions_ready_.wait(lock, [&] { return submission_stop_ || !submissions_.empty(); });
        if (submissions_.empty()) return;
        Submission job = std::move(submissions_.front());
        submissions_.pop_front();
        lock.unlock();
        submit_now(job);
        lock.lock();
        submitted_.store(job.serial, std::memory_order_release);
        submissions_ready_.notify_all();
    }
}

void Gpu::wait_submitted(uint64_t serial) {
    if (serial <= submitted_.load(std::memory_order_acquire)) return;
    std::unique_lock lock(submissions_mutex_);
    submissions_ready_.wait(lock, [&] { return serial <= submitted_.load(std::memory_order_acquire); });
}

void Gpu::wait_submissions() { wait_submitted(serial_ - 1); }

void Gpu::wait_idle() {
    wait_submissions();
    std::lock_guard lock(queue_mutex_);
    vk.vkDeviceWaitIdle(device);
}

void Gpu::stop_submit_thread() {
    if (!submit_thread_.joinable()) return;
    { std::lock_guard lock(submissions_mutex_); submission_stop_ = true; }
    submissions_ready_.notify_all();
    submit_thread_.join();
    submission_stop_ = false;
}

void Gpu::wait_serial(uint64_t serial) {
    if (serial <= completed()) return;
    if (serial >= serial_) submit();  /* the commands are still being recorded */
    assert(serial < serial_);
    wait_submitted(serial);
    for (Frame& f : frames_)
        if (f.serial == serial || (f.serial > completed() && f.serial <= serial))
            wait_fence(f.fence, "vkWaitForFences(serial)");
    poll();
    completed_.store(std::max(completed(), serial), std::memory_order_release);
}

void Gpu::defer(std::function<void()> destroy) {
    std::lock_guard lock(garbage_mutex_);
    frame().garbage.push_back(std::move(destroy));
}

void Gpu::poll() {
    uint64_t done = completed();
    uint64_t submitted = submitted_.load(std::memory_order_acquire);
    for (Frame& f : frames_)
        if (f.serial > done && f.serial <= submitted && vk.vkGetFenceStatus(device, f.fence) == VK_SUCCESS) done = f.serial;
    if (done > completed()) completed_.store(done, std::memory_order_release);
}

void Gpu::destroy() {
    if (!device) return;
    stop_submit_thread();
    wait_idle();
    for (Frame& f : frames_) {
        for (auto& g : f.garbage) g();
        f.garbage.clear();
        vk.vkDestroyFence(device, f.fence, nullptr);
        vk.vkDestroyCommandPool(device, f.pool, nullptr);
    }
    stream.destroy();
    staging.destroy();
    memory.destroy();
    vk.vkDestroyDevice(device, nullptr);
    vk.vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
}
}  // namespace rdvk
