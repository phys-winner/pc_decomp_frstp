#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dxvk {

// Optional native driver cache. Vulkan flags=0 supplies its internal locking;
// this mutex protects only the persistence generation and writer lifetime.
class DxvkPipelineCache {
public:
  DxvkPipelineCache(VkDevice device, PFN_vkCreatePipelineCache create,
    PFN_vkDestroyPipelineCache destroy, PFN_vkGetPipelineCacheData getData,
    PFN_vkCreateGraphicsPipelines graphics, PFN_vkCreateComputePipelines compute,
    const VkPhysicalDeviceProperties& properties, const std::string& directory,
    bool enabled);
  ~DxvkPipelineCache();
  DxvkPipelineCache(const DxvkPipelineCache&) = delete;
  DxvkPipelineCache& operator=(const DxvkPipelineCache&) = delete;

  VkResult createGraphicsPipelines(uint32_t count, const VkGraphicsPipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) const;
  VkResult createComputePipelines(uint32_t count, const VkComputePipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines) const;

private:
  friend struct FrostpunkPipelineCacheTest;
  static constexpr size_t HeaderBytes = 88;
  static constexpr size_t MaxBytes = 32 * 1024 * 1024;
  VkDevice m_device;
  PFN_vkCreatePipelineCache m_create;
  PFN_vkDestroyPipelineCache m_destroy;
  PFN_vkGetPipelineCacheData m_getData;
  PFN_vkCreateGraphicsPipelines m_graphics;
  PFN_vkCreateComputePipelines m_compute;
  VkPhysicalDeviceProperties m_properties;
  VkPipelineCache m_cache = VK_NULL_HANDLE;
  intptr_t m_directory = -1;
  std::string m_name;
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_changed;
  mutable uint64_t m_generation = 0;
  uint64_t m_savedGeneration = 0;
  bool m_stop = false;
  std::thread m_writer;

  void dirty() const;
  void run();
  bool save();
  bool valid(const std::vector<uint8_t>& blob) const;
  std::vector<uint8_t> envelope(const std::vector<uint8_t>& data) const;
};
}
