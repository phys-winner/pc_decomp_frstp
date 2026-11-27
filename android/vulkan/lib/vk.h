/* Vulkan entry points of the rendering device, loaded from the runtime's provider
 * (system loader on Mali/PowerVR, adrenotools Turnip on Adreno). */
#pragma once
#include <vulkan/vulkan.h>

namespace rdvk {
#define RDVK_VK_INSTANCE(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define RDVK_VK_DEVICE(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkGetImageSubresourceLayout) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkGetFenceStatus) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkResetQueryPool) \
    X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer) X(vkCmdBindVertexBuffers) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) X(vkCmdCopyBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdBlitImage2) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdUpdateBuffer) X(vkCmdFillBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdPipelineBarrier) \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdCopyQueryPoolResults) \
    X(vkCmdPushConstants) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier2) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetDepthBiasEnable) X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetPrimitiveRestartEnable) X(vkCmdPushDescriptorSetKHR) X(vkCmdBindVertexBuffers2) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

struct Vk {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
#define RDVK_DECLARE(name) PFN_##name name = nullptr;
    RDVK_VK_INSTANCE(RDVK_DECLARE)
    RDVK_VK_DEVICE(RDVK_DECLARE)
    /* VK_EXT_extended_dynamic_state3 (optional) */
    PFN_vkCmdSetDepthClampEnableEXT vkCmdSetDepthClampEnableEXT = nullptr;
    PFN_vkCmdSetDepthClipEnableEXT vkCmdSetDepthClipEnableEXT = nullptr;
#undef RDVK_DECLARE
};
extern Vk vk;
}  // namespace rdvk
