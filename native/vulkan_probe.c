/* Query only the system Vulkan loader. No device, queue, surface, swapchain,
 * shader module or render work is created. Capabilities do not prove gameplay. */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p=(const unsigned char *)s; *p; ++p) {
        if (*p=='"' || *p=='\\') printf("\\%c",*p);
        else if (*p<32) printf("\\u%04x",*p);
        else putchar(*p);
    }
    putchar('"');
}

static void version(uint32_t v) {
    printf("{\"raw\":%u,\"major\":%u,\"minor\":%u,\"patch\":%u}",
           v,VK_VERSION_MAJOR(v),VK_VERSION_MINOR(v),VK_VERSION_PATCH(v));
}

static void format(VkPhysicalDevice device, const char *name, VkFormat fmt, VkImageUsageFlags usage) {
    VkFormatProperties p;
    VkImageFormatProperties image={0};
    vkGetPhysicalDeviceFormatProperties(device,fmt,&p);
    VkResult result=vkGetPhysicalDeviceImageFormatProperties(device,fmt,VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,usage,0,&image);
    json_string(name);
    printf(":{\"format\":%d,\"linear_features\":%u,\"optimal_features\":%u,\"buffer_features\":%u,"
           "\"optimal_sampled_image\":%s,\"image_query_usage\":%u,\"image_query_result\":%d,"
           "\"image_sample_counts\":%u,\"max_extent\":[%u,%u,%u],\"max_mip_levels\":%u,\"max_array_layers\":%u}",
           fmt,p.linearTilingFeatures,p.optimalTilingFeatures,p.bufferFeatures,
           (p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)?"true":"false",
           usage,result,image.sampleCounts,image.maxExtent.width,image.maxExtent.height,
           image.maxExtent.depth,image.maxMipLevels,image.maxArrayLayers);
}

int main(void) {
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk capability query",
        .applicationVersion=1,.pEngineName="none",.engineVersion=0,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance=VK_NULL_HANDLE;
    VkResult result=vkCreateInstance(&info,NULL,&instance);
    if (result==VK_ERROR_INCOMPATIBLE_DRIVER) { app.apiVersion=VK_API_VERSION_1_0; result=vkCreateInstance(&info,NULL,&instance); }
    if (result!=VK_SUCCESS) { printf("{\"success\":false,\"stage\":\"vkCreateInstance\",\"result\":%d}\n",result); return 2; }
    uint32_t count=0;
    result=vkEnumeratePhysicalDevices(instance,&count,NULL);
    if (result!=VK_SUCCESS || !count || count>16) {
        printf("{\"success\":false,\"stage\":\"vkEnumeratePhysicalDevices\",\"result\":%d,\"count\":%u}\n",result,count);
        vkDestroyInstance(instance,NULL); return 2;
    }
    VkPhysicalDevice *devices=calloc(count,sizeof(*devices));
    if (!devices) { vkDestroyInstance(instance,NULL); return 2; }
    result=vkEnumeratePhysicalDevices(instance,&count,devices);
    if (result!=VK_SUCCESS) { free(devices); vkDestroyInstance(instance,NULL); return 2; }
    printf("{\"success\":true,\"system_loader_only\":true,\"instance_api\":"); version(app.apiVersion);
    printf(",\"devices\":[");
    for (uint32_t d=0; d<count; ++d) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceFeatures f;
        VkPhysicalDeviceMemoryProperties m;
        vkGetPhysicalDeviceProperties(devices[d],&p);
        vkGetPhysicalDeviceFeatures(devices[d],&f);
        vkGetPhysicalDeviceMemoryProperties(devices[d],&m);
        if (d) putchar(',');
        printf("{\"name\":"); json_string(p.deviceName);
        printf(",\"api_version\":"); version(p.apiVersion);
        printf(",\"driver_version_raw\":%u,\"vendor_id\":%u,\"device_id\":%u,\"device_type\":%d,\"features\":{"
               "\"textureCompressionBC\":%s,\"textureCompressionETC2\":%s,\"textureCompressionASTC_LDR\":%s,"
               "\"geometryShader\":%s,\"tessellationShader\":%s,\"independentBlend\":%s,\"dualSrcBlend\":%s,"
               "\"depthClamp\":%s,\"depthBiasClamp\":%s,\"samplerAnisotropy\":%s,\"multiDrawIndirect\":%s,"
               "\"drawIndirectFirstInstance\":%s,\"fragmentStoresAndAtomics\":%s,\"shaderStorageImageExtendedFormats\":%s},",
               p.driverVersion,p.vendorID,p.deviceID,p.deviceType,
               f.textureCompressionBC?"true":"false",f.textureCompressionETC2?"true":"false",f.textureCompressionASTC_LDR?"true":"false",
               f.geometryShader?"true":"false",f.tessellationShader?"true":"false",f.independentBlend?"true":"false",f.dualSrcBlend?"true":"false",
               f.depthClamp?"true":"false",f.depthBiasClamp?"true":"false",f.samplerAnisotropy?"true":"false",f.multiDrawIndirect?"true":"false",
               f.drawIndirectFirstInstance?"true":"false",f.fragmentStoresAndAtomics?"true":"false",f.shaderStorageImageExtendedFormats?"true":"false");
        printf("\"limits\":{\"maxImageDimension2D\":%u,\"maxBoundDescriptorSets\":%u,\"maxPerStageDescriptorSamplers\":%u,"
               "\"maxPerStageDescriptorSampledImages\":%u,\"maxColorAttachments\":%u,\"framebufferColorSampleCounts\":%u,"
               "\"framebufferDepthSampleCounts\":%u,\"sampledImageColorSampleCounts\":%u,\"sampledImageDepthSampleCounts\":%u},",
               p.limits.maxImageDimension2D,p.limits.maxBoundDescriptorSets,p.limits.maxPerStageDescriptorSamplers,
               p.limits.maxPerStageDescriptorSampledImages,p.limits.maxColorAttachments,p.limits.framebufferColorSampleCounts,
               p.limits.framebufferDepthSampleCounts,p.limits.sampledImageColorSampleCounts,p.limits.sampledImageDepthSampleCounts);
        printf("\"memory_heaps\":[");
        for (uint32_t h=0; h<m.memoryHeapCount; ++h) { if (h) putchar(','); printf("{\"size\":%llu,\"flags\":%u}",(unsigned long long)m.memoryHeaps[h].size,m.memoryHeaps[h].flags); }
        printf("],\"formats\":{");
        VkImageUsageFlags sample=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        format(devices[d],"BC1_RGBA_UNORM",VK_FORMAT_BC1_RGBA_UNORM_BLOCK,sample); putchar(',');
        format(devices[d],"BC3_UNORM",VK_FORMAT_BC3_UNORM_BLOCK,sample); putchar(',');
        format(devices[d],"BC5_UNORM",VK_FORMAT_BC5_UNORM_BLOCK,sample); putchar(',');
        format(devices[d],"BC7_UNORM",VK_FORMAT_BC7_UNORM_BLOCK,sample); putchar(',');
        format(devices[d],"ETC2_RGBA8_UNORM",VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,sample); putchar(',');
        format(devices[d],"ASTC_4x4_UNORM",VK_FORMAT_ASTC_4x4_UNORM_BLOCK,sample); putchar(',');
        format(devices[d],"RGBA8_UNORM",VK_FORMAT_R8G8B8A8_UNORM,sample|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT); putchar(',');
        format(devices[d],"D24_UNORM_S8_UINT",VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        printf("},\"extensions\":[");
        uint32_t ext_count=0;
        result=vkEnumerateDeviceExtensionProperties(devices[d],NULL,&ext_count,NULL);
        VkExtensionProperties *ext=NULL;
        if (result==VK_SUCCESS && ext_count<=4096) ext=calloc(ext_count?ext_count:1,sizeof(*ext));
        if (ext) result=vkEnumerateDeviceExtensionProperties(devices[d],NULL,&ext_count,ext); else result=VK_ERROR_OUT_OF_HOST_MEMORY;
        if (result==VK_SUCCESS) for (uint32_t e=0;e<ext_count;++e) { if(e)putchar(','); json_string(ext[e].extensionName); }
        printf("],\"extension_query_result\":%d}",result); free(ext);
    }
    printf("],\"scope\":\"Feature/format queries only: no VkDevice, shaders, rendering, original game or FPS measurement. Heap sizes are limits, not free memory.\"}\n");
    free(devices); vkDestroyInstance(instance,NULL); return 0;
}
