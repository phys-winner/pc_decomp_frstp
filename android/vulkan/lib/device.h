/* Vulkan device, memory, frames and uploads of the native rendering device. */
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "vk.h"

namespace rdvk {

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* mapped = nullptr;  /* host pointer of offset 0 when the memory is host visible */
    int32_t block = -1;         /* -1: dedicated allocation */
    uint32_t type = 0;
};

/* Sub-allocates 64 MiB blocks per memory type with a sorted free list; big requests get their
 * own allocation.  Thread safe. */
class Allocator {
public:
    void init(const VkPhysicalDeviceMemoryProperties& properties, VkDeviceSize granularity);
    Allocation allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, bool dedicated = false);
    void release(const Allocation& allocation);
    void destroy();
    uint64_t allocated_bytes() const { return allocated_.load(std::memory_order_relaxed); }
    uint64_t used_bytes() const { return used_.load(std::memory_order_relaxed); }
private:
    struct Range { VkDeviceSize offset, size; };
    struct Block { VkDeviceMemory memory = VK_NULL_HANDLE; uint8_t* mapped = nullptr; VkDeviceSize size = 0; uint32_t type = 0; std::vector<Range> free; };
    int choose(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const;
    std::mutex mutex_;
    VkPhysicalDeviceMemoryProperties properties_{};
    VkDeviceSize granularity_ = 1;
    std::vector<Block> blocks_;
    std::atomic<uint64_t> allocated_{0}, used_{0};
};

/* Linear host-visible ring for per-frame data (constant buffers, dynamic vertices, uploads):
 * chunks are recycled after the frame that used them completed on the GPU. */
struct Slice {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* data = nullptr;
    int32_t chunk = -1;
};
/* Chunks are recycled when the GPU finished the last frame that wrote them and no dynamic buffer
 * still keeps its contents there (retain/release: a D3D11 DISCARD buffer stays valid until its
 * next Map, possibly for many frames). */
class Ring {
public:
    void init(VkBufferUsageFlags usage, VkDeviceSize chunk, VkDeviceSize alignment);
    Slice allocate(VkDeviceSize size, VkDeviceSize alignment = 0);
    void retain(int32_t chunk) { if (chunk >= 0) chunks_[size_t(chunk)].refs.fetch_add(1, std::memory_order_relaxed); }
    /* The contents in `chunk` are no longer kept from now on; commands recorded up to `serial` may
     * still read them, so the chunk is not reused before that submission completed. */
    void release(int32_t chunk, uint64_t serial);
    /* Retired chunks whose frames completed but that dynamic contents still keep. */
    void pinned(uint64_t completed, std::vector<int32_t>& out);
    struct Stats { uint32_t chunks = 0, idle = 0, pinned = 0; };
    Stats stats();
    VkBufferUsageFlags usage() const { return usage_; }
    void log_chunks();   /* diagnostics: RDVK_RING line with every chunk's size and state */
    void frame_done(uint64_t completed_serial);  /* recycle chunks of completed frames */
    void frame_end(uint64_t serial);             /* chunks used so far belong to this serial */
    void destroy();
    uint64_t bytes() const { return bytes_; }
private:
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE; Allocation memory; VkDeviceSize size = 0, used = 0;
        uint64_t idle_since = 0;   /* completed serial when the chunk became idle */
        std::atomic<uint64_t> serial{0};
        std::atomic<int32_t> refs{0};
    };
    int32_t make(VkDeviceSize size);
    std::mutex mutex_;
    VkBufferUsageFlags usage_ = 0;
    VkDeviceSize chunk_ = 0, alignment_ = 16;
    std::deque<Chunk> chunks_;
    int32_t active_ = -1;
    std::vector<int32_t> retired_, idle_, empty_;   /* empty_: slots whose chunk was freed */
    uint64_t bytes_ = 0;
};

enum : uint32_t {
    SHADER_FEATURE_TESSELLATION = 1u << 0, SHADER_FEATURE_GEOMETRY = 1u << 1, SHADER_FEATURE_READ_WITHOUT_FORMAT = 1u << 2,
    SHADER_FEATURE_WRITE_WITHOUT_FORMAT = 1u << 3, SHADER_FEATURE_DRAW_PARAMETERS = 1u << 4, SHADER_FEATURE_MEMORY_MODEL = 1u << 5,
    SHADER_FEATURE_DEMOTE = 1u << 6, SHADER_FEATURE_CUBE_ARRAY = 1u << 7, SHADER_FEATURE_GATHER_EXTENDED = 1u << 8,
    SHADER_FEATURE_INT16 = 1u << 9, SHADER_FEATURE_FLOAT16 = 1u << 10, SHADER_FEATURE_SAMPLE_RATE = 1u << 11,
    SHADER_FEATURE_CLIP_DISTANCE = 1u << 12, SHADER_FEATURE_CULL_DISTANCE = 1u << 13, SHADER_FEATURE_MULTI_VIEWPORT = 1u << 14,
    SHADER_FEATURE_INT64 = 1u << 15, SHADER_FEATURE_FLOAT64 = 1u << 16, SHADER_FEATURE_COUNT = 17
};
struct Features {
    bool push_descriptor = false, extended_dynamic_state3_clamp = false, extended_dynamic_state3_clip = false;
    bool robustness2_null = false, tessellation = false, geometry = false, bc = false, astc = true;
    bool depth_clamp = false, sampler_anisotropy = false, independent_blend = false, index_uint8 = false;
    bool depth_bias_clamp = false, dual_src_blend = false, depth_clip = false;
    bool host_query_reset = false, occlusion_precise = false;
    /* SPIR-V capabilities of the game's shaders that depend on optional device features. */
    uint32_t shader_features = 0;   /* SHADER_FEATURE_* supported by the device */
    bool reset_dynamic_per_pass = false;   /* re-set all dynamic state after each vkCmdBeginRendering (Mali) */
    bool defer_attachment_transitions = false;
    uint32_t vendor = 0, device_id = 0, driver_id = 0, api = 0;
    uint32_t max_push_descriptors = 0;
    VkDeviceSize min_uniform_offset = 256, min_storage_offset = 256, non_coherent_atom = 64;
    float timestamp_period = 1.0f, max_anisotropy = 1.0f;
    char name[256] = {};
};

class Gpu {
public:
    ~Gpu();
    bool init();
    void destroy();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    Features caps;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    Allocator memory;
    Ring stream;   /* constant buffers, dynamic vertex/index data, small uploads */
    Ring staging;  /* texture and buffer uploads */

    /* Frames: one command buffer recorded by the render thread per submission. */
    static constexpr unsigned FRAMES = 3;
    VkCommandBuffer cmd() { return frame().cmd; }
    /* Uploads from any thread, queued under upload_lock(): the render thread records them into the
     * frame's upload command buffer at the next submit, before cmd().  No two threads ever record
     * into one command pool (the PowerVR driver corrupted its heap when loader threads recorded
     * the upload buffer while the render thread recorded the frame from the same pool). */
    void upload(std::function<void(VkCommandBuffer)> record);
    void upload_copy(VkBuffer src, VkBuffer dst, const VkBufferCopy& region);
    std::unique_lock<std::mutex> upload_lock() { return std::unique_lock<std::mutex>(upload_mutex_); }
    /* vkWaitForFences without a timeout; retries waits a signal interrupted (Mali). */
    void wait_fence(VkFence fence, const char* what);
    /* Submits the frame's command buffers and queues any presentation behind them. */
    void submit(VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags wait_stage = 0, VkSemaphore signal = VK_NULL_HANDLE,
                std::function<void()> present = {});
    void wait_submissions();  /* host submission and presentation calls, without waiting for the GPU */
    void wait_idle();
    void wait_serial(uint64_t serial);       /* CPU wait until that submission completed */
    uint64_t serial() const { return serial_; }             /* serial of the commands being recorded */
    uint64_t completed() const { return completed_.load(std::memory_order_acquire); }
    void defer(std::function<void()> destroy);              /* runs after the current serial completed */
    void poll();
    /* Completion marker (wiki note 24): every submission ends with a write of its serial into a small
     * host-visible buffer, so "is it done" is a memory read.  vkGetFenceStatus of Turnip on KGSL can
     * wait for the GPU; fences stay for the waits that must block and are still asked before a slot's
     * fence and pool are reset, once the marker shows that the slot completed (then the fence is
     * signalled).  debug.fp.rdvk_marker=0 restores fence-status polling.  0 = no marker. */
    uint64_t marker_done();
    bool has_marker() const { return done_map_ != nullptr; }
    /* Render-thread sync statistics, logged as RDVK_SYNC every 600 frames (present.cpp). */
    struct SyncStats {
        uint64_t status_calls = 0, status_ns = 0;        /* vkGetFenceStatus */
        uint64_t fence_waits = 0, fence_wait_ns = 0;     /* vkWaitForFences in wait_serial (> 50 us) */
        uint64_t slot_waits = 0, slot_wait_ns = 0;       /* begin_frame: waits for a free frame slot (> 50 us) */
        uint64_t submits = 0, submit_ns = 0;             /* vkQueueSubmit */
        uint64_t present_ns = 0, acquire_ns = 0;         /* vkQueuePresentKHR, vkAcquireNextImageKHR */
        uint64_t map_waits = 0, map_wait_ns = 0;         /* Map of a staging resource the GPU still used */
        uint64_t map_staging = 0, map_flags = 0;         /* staging Maps that may wait, those with nonzero MapFlags */
        uint64_t poll_calls = 0;                         /* Gpu::poll (GetData, query_samples, wait_serial) */
    } sync;

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE, upload = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        std::vector<std::function<void()>> garbage;
    };
    Frame& frame() { return frames_[serial_ % FRAMES]; }
    void begin_frame();
    Frame frames_[FRAMES];
    struct UploadJob {
        VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
        VkBufferCopy region{};
        std::function<void(VkCommandBuffer)> record;   /* or a buffer copy */
    };
    std::vector<UploadJob> uploads_;                   /* under upload_mutex_ */
    struct Submission {
        VkCommandBuffer buffers[2]{};
        uint32_t count = 0;
        VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
        VkPipelineStageFlags wait_stage = 0;
        VkFence fence = VK_NULL_HANDLE;
        uint64_t serial = 0;
        std::function<void()> present;
    };
    void submit_now(Submission& job);
    void submit_loop();
    void stop_submit_thread();
    void wait_submitted(uint64_t serial);
    std::deque<Submission> submissions_;
    std::thread submit_thread_;
    std::mutex submissions_mutex_;
    std::condition_variable submissions_ready_;
    bool submission_stop_ = false;
    uint64_t serial_ = 1;
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> completed_{0};
    void marker_init();
    void marker_record(VkCommandBuffer cmd, uint64_t serial);
    bool fence_done(VkFence fence);   /* vkGetFenceStatus, counted */
    VkBuffer done_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory done_mem_ = VK_NULL_HANDLE;
    volatile uint32_t* done_map_ = nullptr;
    bool done_coherent_ = true;
    std::mutex upload_mutex_, queue_mutex_, garbage_mutex_;
};

extern Gpu gpu;
const char* vk_result(VkResult result);
void vk_check(VkResult result, const char* what);
}  // namespace rdvk
