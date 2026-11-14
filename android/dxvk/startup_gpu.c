#define VK_NO_PROTOTYPES
#include "startup_gpu.h"
#include "bridge.h"
#include "driver.h"
#include <stdio.h>
#include <stdlib.h>

int frostpunk_startup_gpu_init(const char *runtime_dir,const char *driver_dir) {
    char error[512]={0};
    PFN_vkGetInstanceProcAddr get=frostpunk_gpu_open(runtime_dir,driver_dir,error,sizeof(error));
    if (!get) { fprintf(stderr,"GPU_STOP provider=%s\n",error); return 0; }
    return frostpunk_startup_gpu_configure(get,NULL);
}

int frostpunk_startup_gpu_configure(PFN_vkGetInstanceProcAddr get,void *native_window) {
    if (!get) return 0;
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    if (!create) { fprintf(stderr,"GPU_STOP vkCreateInstance missing\n"); return 0; }
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk AOT native startup",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance=VK_NULL_HANDLE;
    VkResult result=create(&info,NULL,&instance);
    if (result!=VK_SUCCESS) { fprintf(stderr,"GPU_STOP Vulkan1.3 instance result=%d\n",result); return 0; }
#define LOAD(type,name) type name=(type)get(instance,#name)
    LOAD(PFN_vkDestroyInstance,vkDestroyInstance);
    LOAD(PFN_vkEnumeratePhysicalDevices,vkEnumeratePhysicalDevices);
    LOAD(PFN_vkGetPhysicalDeviceProperties2,vkGetPhysicalDeviceProperties2);
    LOAD(PFN_vkGetPhysicalDeviceFeatures,vkGetPhysicalDeviceFeatures);
    LOAD(PFN_vkGetPhysicalDeviceFormatProperties,vkGetPhysicalDeviceFormatProperties);
#undef LOAD
    if (!vkDestroyInstance || !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceFeatures || !vkGetPhysicalDeviceFormatProperties) {
        fprintf(stderr,"GPU_STOP missing native provider query function\n");
        if (vkDestroyInstance) vkDestroyInstance(instance,NULL);
        return 0;
    }
    uint32_t count=0;
    result=vkEnumeratePhysicalDevices(instance,&count,NULL);
    if (result!=VK_SUCCESS || count!=1) { fprintf(stderr,"GPU_STOP expected one physical GPU result=%d count=%u\n",result,count); vkDestroyInstance(instance,NULL); return 0; }
    VkPhysicalDevice device=VK_NULL_HANDLE;
    result=vkEnumeratePhysicalDevices(instance,&count,&device);
    if (result!=VK_SUCCESS || count!=1) { fprintf(stderr,"GPU_STOP GPU enumeration changed result=%d\n",result); vkDestroyInstance(instance,NULL); return 0; }
    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};
    vkGetPhysicalDeviceProperties2(device,&properties);
    VkPhysicalDeviceFeatures features={0}; vkGetPhysicalDeviceFeatures(device,&features);
    VkFormatProperties bc1={0},bc3={0},bgra={0},astc={0};
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,&bc1);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_BC3_UNORM_BLOCK,&bc3);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_B8G8R8A8_UNORM,&bgra);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,&astc);
#ifdef FP_GPU_SYSTEM
    int valid=(((driver.driverID==VK_DRIVER_ID_ARM_PROPRIETARY && properties.properties.vendorID==0x13b5) ||
        (driver.driverID==VK_DRIVER_ID_IMAGINATION_PROPRIETARY && properties.properties.vendorID==0x1010)) &&
        properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU && properties.properties.apiVersion>=VK_API_VERSION_1_3 &&
        features.textureCompressionASTC_LDR && (bgra.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        (astc.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
    /* Mesa Turnip, selected in the Graphics menu ("Vulkan driver") and opened by the APK through adrenotools. */
    valid=valid||(driver.driverID==VK_DRIVER_ID_MESA_TURNIP && properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU &&
        properties.properties.apiVersion>=VK_API_VERSION_1_3 && features.textureCompressionBC &&
        (bc1.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (bc3.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
#else
    int valid=(driver.driverID==VK_DRIVER_ID_MESA_TURNIP && properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU &&
        properties.properties.apiVersion>=VK_API_VERSION_1_3 && features.textureCompressionBC &&
        (bc1.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (bc3.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
#endif
    fprintf(stderr,"GPU_%s device=%s driver_id=%u api=%u BC=%u ASTC=%u native_aarch64=1 wine_box64=0 offscreen=%d\n",
        valid?"READY":"STOP",properties.properties.deviceName,driver.driverID,properties.properties.apiVersion,features.textureCompressionBC,features.textureCompressionASTC_LDR,native_window==NULL);
    vkDestroyInstance(instance,NULL);
    if (!valid) return 0;
    win64_d3d11_configure(get,native_window);
    return 1;
}
