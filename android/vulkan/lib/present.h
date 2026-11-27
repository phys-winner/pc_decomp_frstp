/* DXGI swap chain object of the native device. */
#pragma once
#include <vector>
#include "objects.h"

namespace rdvk {
struct SwapChainObject : Object {
    SwapChainObject() : Object(Kind::SwapChain) {}
    uint64_t hwnd = 0;
    d3d::SWAP_CHAIN_DESC1 desc{};
    uint32_t width = 0, height = 0;
    Texture* back = nullptr;            /* the engine's back buffer (GetBuffer(0)) */
    void* window = nullptr;             /* ANativeWindow lease */
    uint64_t generation = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered;
    VkSemaphore acquire[Gpu::FRAMES + 1] = {};
    uint32_t acquire_index = 0;
    std::atomic<bool> recreate{false};
    /* Swap chain images: the back buffer size (the compositor scales), or the window size when the
     * "Upscaling" row asks for FSR 1, which the presenter then draws from the back buffer. */
    uint32_t out_width = 0, out_height = 0;
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSurfaceTransformFlagBitsKHR surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    int upscaler = 0;
    std::vector<VkImageView> views;
    VkImageView back_view = VK_NULL_HANDLE;
    Texture* back_view_of = nullptr;
};
SwapChainObject* swapchain_create(uint64_t hwnd, const d3d::SWAP_CHAIN_DESC1& desc);
uint32_t swapchain_resize(SwapChainObject* s, uint32_t width, uint32_t height, uint32_t format);
}  // namespace rdvk
