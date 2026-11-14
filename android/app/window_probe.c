/* Actual Android WSI fixture: Vulkan surface, swapchain, acquired image,
 * transfer clear, submit and present. No original game rendering is claimed. */
#include "window_probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct window_probe {
    ANativeWindow *window;PFN_vkGetInstanceProcAddr gipa;PFN_vkGetDeviceProcAddr gdpa;
    VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkSurfaceKHR surface;
    VkQueue queue;uint32_t family;VkSwapchainKHR swapchain;VkImage images[16];uint32_t count;
    VkCommandPool pool;VkCommandBuffer commands[16];VkSemaphore acquired,rendered;VkFence fence;
    VkExtent2D extent;uint64_t frames;unsigned char used[16];char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
#define FN(name) PFN_vk##name name
    FN(DestroyInstance);FN(DestroySurfaceKHR);FN(GetPhysicalDeviceSurfaceCapabilitiesKHR);FN(GetPhysicalDeviceSurfaceFormatsKHR);
    FN(DestroyDevice);FN(DeviceWaitIdle);FN(GetDeviceQueue);FN(CreateSwapchainKHR);FN(DestroySwapchainKHR);FN(GetSwapchainImagesKHR);
    FN(CreateCommandPool);FN(DestroyCommandPool);FN(AllocateCommandBuffers);FN(ResetCommandBuffer);FN(BeginCommandBuffer);FN(EndCommandBuffer);
    FN(CmdPipelineBarrier);FN(CmdClearColorImage);FN(CreateSemaphore);FN(DestroySemaphore);FN(CreateFence);FN(DestroyFence);
    FN(WaitForFences);FN(ResetFences);FN(AcquireNextImageKHR);FN(QueueSubmit);FN(QueuePresentKHR);FN(QueueWaitIdle);
#undef FN
};
void window_probe_stop(window_probe *p) {
    if(!p)return;
    if(p->device) {
        if(p->DeviceWaitIdle)p->DeviceWaitIdle(p->device);
        if(p->fence)p->DestroyFence(p->device,p->fence,NULL);
        if(p->acquired)p->DestroySemaphore(p->device,p->acquired,NULL);
        if(p->rendered)p->DestroySemaphore(p->device,p->rendered,NULL);
        if(p->pool)p->DestroyCommandPool(p->device,p->pool,NULL);
        if(p->swapchain)p->DestroySwapchainKHR(p->device,p->swapchain,NULL);
        if(p->DestroyDevice)p->DestroyDevice(p->device,NULL);
    }
    if(p->surface&&p->DestroySurfaceKHR)p->DestroySurfaceKHR(p->instance,p->surface,NULL);
    if(p->instance&&p->DestroyInstance)p->DestroyInstance(p->instance,NULL);
    if(p->window)ANativeWindow_release(p->window);free(p);
}
window_probe *window_probe_start(ANativeWindow *window,PFN_vkGetInstanceProcAddr provider,char *error,size_t size) {
    window_probe *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->gipa=provider;if(window){p->window=window;ANativeWindow_acquire(window);}
#define FAIL(detail) do{snprintf(error,size,"%s",detail);goto failed;}while(0)
#define CHECK(call) do{VkResult result=(call);if(result!=VK_SUCCESS){snprintf(error,size,"%s VkResult=%d",#call,result);goto failed;}}while(0)
#define INSTANCE(name) p->name=(PFN_vk##name)provider(p->instance,"vk"#name);if(!p->name)FAIL("missing vk"#name)
#define DEVICE(name) p->name=(PFN_vk##name)p->gdpa(p->device,"vk"#name);if(!p->name)FAIL("missing vk"#name)
    if(!window||!provider)FAIL("no native window or custom Vulkan provider");
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)provider(NULL,"vkCreateInstance");if(!create)FAIL("missing vkCreateInstance");
    const char *extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk native window probe",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
    CHECK(create(&info,NULL,&p->instance));INSTANCE(DestroyInstance);INSTANCE(DestroySurfaceKHR);
    PFN_vkCreateAndroidSurfaceKHR surface=(PFN_vkCreateAndroidSurfaceKHR)provider(p->instance,"vkCreateAndroidSurfaceKHR");if(!surface)FAIL("missing Android surface extension");
    VkAndroidSurfaceCreateInfoKHR android={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=window};CHECK(surface(p->instance,&android,NULL,&p->surface));
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)provider(p->instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)provider(p->instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures features=(PFN_vkGetPhysicalDeviceFeatures)provider(p->instance,"vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)provider(p->instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR support=(PFN_vkGetPhysicalDeviceSurfaceSupportKHR)provider(p->instance,"vkGetPhysicalDeviceSurfaceSupportKHR");
    if(!enumerate||!properties||!features||!families||!support)FAIL("missing physical device functions");
    uint32_t count=0;CHECK(enumerate(p->instance,&count,NULL));if(!count||count>8)FAIL("physical device count unsupported");
    VkPhysicalDevice devices[8];CHECK(enumerate(p->instance,&count,devices));int found=0;
    for(uint32_t i=0;i<count&&!found;++i) {
        VkPhysicalDeviceProperties prop;VkPhysicalDeviceFeatures feat;properties(devices[i],&prop);features(devices[i],&feat);
        if(prop.apiVersion<VK_API_VERSION_1_3)continue;
#ifdef FP_GPU_SYSTEM
        if(prop.vendorID!=0x13b5||!feat.textureCompressionASTC_LDR)continue;
#else
        if(!feat.textureCompressionBC)continue;
#endif
        uint32_t n=0;families(devices[i],&n,NULL);if(n>32)continue;VkQueueFamilyProperties queues[32];families(devices[i],&n,queues);
        for(uint32_t q=0;q<n;++q){VkBool32 present=0;CHECK(support(devices[i],q,p->surface,&present));
            if(present&&(queues[q].queueFlags&VK_QUEUE_GRAPHICS_BIT)){p->physical=devices[i];p->family=q;strncpy(p->name,prop.deviceName,sizeof(p->name)-1);found=1;break;}}
    }
    if(!found)FAIL("no Vulkan1.3+BC graphics queue supporting the real Android surface");
    PFN_vkCreateDevice device=(PFN_vkCreateDevice)provider(p->instance,"vkCreateDevice");p->gdpa=(PFN_vkGetDeviceProcAddr)provider(p->instance,"vkGetDeviceProcAddr");
    if(!device||!p->gdpa)FAIL("missing device creation functions");
    float priority=1;VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=p->family,.queueCount=1,.pQueuePriorities=&priority};
#ifdef FP_GPU_SYSTEM
    VkPhysicalDeviceFeatures enabled={.textureCompressionASTC_LDR=VK_TRUE};
#else
    VkPhysicalDeviceFeatures enabled={.textureCompressionBC=VK_TRUE};
#endif
    const char *device_ext[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue,.enabledExtensionCount=1,.ppEnabledExtensionNames=device_ext,.pEnabledFeatures=&enabled};
    CHECK(device(p->physical,&dc,NULL,&p->device));
    DEVICE(DestroyDevice);DEVICE(DeviceWaitIdle);DEVICE(GetDeviceQueue);DEVICE(CreateSwapchainKHR);DEVICE(DestroySwapchainKHR);DEVICE(GetSwapchainImagesKHR);
    DEVICE(CreateCommandPool);DEVICE(DestroyCommandPool);DEVICE(AllocateCommandBuffers);DEVICE(ResetCommandBuffer);DEVICE(BeginCommandBuffer);DEVICE(EndCommandBuffer);
    DEVICE(CmdPipelineBarrier);DEVICE(CmdClearColorImage);DEVICE(CreateSemaphore);DEVICE(DestroySemaphore);DEVICE(CreateFence);DEVICE(DestroyFence);
    DEVICE(WaitForFences);DEVICE(ResetFences);DEVICE(AcquireNextImageKHR);DEVICE(QueueSubmit);DEVICE(QueuePresentKHR);DEVICE(QueueWaitIdle);
    INSTANCE(GetPhysicalDeviceSurfaceCapabilitiesKHR);INSTANCE(GetPhysicalDeviceSurfaceFormatsKHR);
    p->GetDeviceQueue(p->device,p->family,0,&p->queue);
    VkSurfaceCapabilitiesKHR caps;CHECK(p->GetPhysicalDeviceSurfaceCapabilitiesKHR(p->physical,p->surface,&caps));
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))FAIL("surface does not allow transfer-clear fixture");
    uint32_t format_count=0;CHECK(p->GetPhysicalDeviceSurfaceFormatsKHR(p->physical,p->surface,&format_count,NULL));
    if(!format_count||format_count>64)FAIL("surface format count unsupported");VkSurfaceFormatKHR formats[64];CHECK(p->GetPhysicalDeviceSurfaceFormatsKHR(p->physical,p->surface,&format_count,formats));
    VkSurfaceFormatKHR format=formats[0];for(uint32_t i=0;i<format_count;++i)if(formats[i].format==VK_FORMAT_R8G8B8A8_UNORM||formats[i].format==VK_FORMAT_B8G8R8A8_UNORM){format=formats[i];break;}
    p->extent=caps.currentExtent;
    if(p->extent.width==UINT32_MAX){p->extent.width=(uint32_t)ANativeWindow_getWidth(window);p->extent.height=(uint32_t)ANativeWindow_getHeight(window);
        if(p->extent.width<caps.minImageExtent.width)p->extent.width=caps.minImageExtent.width;if(p->extent.width>caps.maxImageExtent.width)p->extent.width=caps.maxImageExtent.width;
        if(p->extent.height<caps.minImageExtent.height)p->extent.height=caps.minImageExtent.height;if(p->extent.height>caps.maxImageExtent.height)p->extent.height=caps.maxImageExtent.height;}
    if(!p->extent.width||!p->extent.height)FAIL("zero Android surface extent");
    uint32_t image_count=caps.minImageCount+1;if(caps.maxImageCount&&image_count>caps.maxImageCount)image_count=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&alpha))alpha=(VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1u));
    VkSwapchainCreateInfoKHR swap={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=p->surface,.minImageCount=image_count,
        .imageFormat=format.format,.imageColorSpace=format.colorSpace,.imageExtent=p->extent,.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.currentTransform,.compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};
    CHECK(p->CreateSwapchainKHR(p->device,&swap,NULL,&p->swapchain));CHECK(p->GetSwapchainImagesKHR(p->device,p->swapchain,&p->count,NULL));
    if(!p->count||p->count>16)FAIL("swapchain image count unsupported");CHECK(p->GetSwapchainImagesKHR(p->device,p->swapchain,&p->count,p->images));
    VkCommandPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=p->family};CHECK(p->CreateCommandPool(p->device,&pool,NULL,&p->pool));
    VkCommandBufferAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=p->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=p->count};CHECK(p->AllocateCommandBuffers(p->device,&allocation,p->commands));
    VkSemaphoreCreateInfo semaphore={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};CHECK(p->CreateSemaphore(p->device,&semaphore,NULL,&p->acquired));CHECK(p->CreateSemaphore(p->device,&semaphore,NULL,&p->rendered));
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags=VK_FENCE_CREATE_SIGNALED_BIT};CHECK(p->CreateFence(p->device,&fence,NULL,&p->fence));return p;
failed:window_probe_stop(p);return NULL;
#undef FAIL
#undef CHECK
#undef INSTANCE
#undef DEVICE
}
int window_probe_frame(window_probe *p,float x,float y) {
#define CHECK(call) do{VkResult result=(call);if(result!=VK_SUCCESS)return (int)result;}while(0)
    CHECK(p->WaitForFences(p->device,1,&p->fence,VK_TRUE,UINT64_MAX));uint32_t image=0;
    VkResult acquire=p->AcquireNextImageKHR(p->device,p->swapchain,16000000,p->acquired,VK_NULL_HANDLE,&image);
    if(acquire==VK_TIMEOUT||acquire==VK_NOT_READY)return 0;if(acquire==VK_ERROR_OUT_OF_DATE_KHR||acquire==VK_ERROR_SURFACE_LOST_KHR)return 2;
    if(acquire!=VK_SUCCESS&&acquire!=VK_SUBOPTIMAL_KHR)return acquire;
    VkCommandBuffer cmd=p->commands[image];CHECK(p->ResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};CHECK(p->BeginCommandBuffer(cmd,&begin));
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=p->used[image]?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=p->images[image],.subresourceRange=range};
    p->CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkClearColorValue color={{0.08f+x*0.4f,0.12f+y*0.4f,0.35f,1.0f}};p->CmdClearColorImage(cmd,p->images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    p->CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);CHECK(p->EndCommandBuffer(cmd));
    CHECK(p->ResetFences(p->device,1,&p->fence));VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&p->acquired,.pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&cmd,.signalSemaphoreCount=1,.pSignalSemaphores=&p->rendered};CHECK(p->QueueSubmit(p->queue,1,&submit,p->fence));
    VkPresentInfoKHR present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&p->rendered,.swapchainCount=1,.pSwapchains=&p->swapchain,.pImageIndices=&image};
    VkResult result=p->QueuePresentKHR(p->queue,&present);CHECK(p->QueueWaitIdle(p->queue));p->used[image]=1;
    if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_ERROR_SURFACE_LOST_KHR)return 2;
    if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR)return result;++p->frames;return result==VK_SUBOPTIMAL_KHR?2:1;
#undef CHECK
}
const char *window_probe_device(window_probe *p){return p?p->name:"";}
uint64_t window_probe_frames(window_probe *p){return p?p->frames:0;}

void window_probe_extent(window_probe *p,uint32_t *width,uint32_t *height){*width=p?p->extent.width:0;*height=p?p->extent.height:0;}
const char *window_probe_details(window_probe *p){(void)p;return "null";}
