#define VK_NO_PROTOTYPES
#include "startup_gpu.h"
#include "bridge.h"
#include "driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The renderer failure policy (24 November 2026): a driver this gate refuses is the selected renderer's
 * failure to start. The record lets the launcher offer the next renderer; bootstrap.c then stops. */
#include "../launcher-base/native/replacements/renderer_failure.h"

static int gpu_refused(const char *reason) {
    fprintf(stderr,"GPU_STOP %s\n",reason);
    int recorded=port_renderer_failed(reason,0);
    fprintf(stderr,"GPU_RENDERER_FAILURE kind=start recorded=%d\n",recorded);
    return 0;
}

int frostpunk_startup_gpu_init(const char *runtime_dir,const char *driver_dir) {
    char error[512]={0},reason[600];
    PFN_vkGetInstanceProcAddr get=frostpunk_gpu_open(runtime_dir,driver_dir,error,sizeof(error));
    if (!get) { snprintf(reason,sizeof(reason),"Vulkan driver: %s",error); return gpu_refused(reason); }
    return frostpunk_startup_gpu_configure(get,NULL);
}

int frostpunk_startup_gpu_configure(PFN_vkGetInstanceProcAddr get,void *native_window) {
    if (!get) return gpu_refused("Vulkan driver: no vkGetInstanceProcAddr");
    char reason[400];
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    if (!create) return gpu_refused("Vulkan driver: vkCreateInstance missing");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk AOT native startup",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance=VK_NULL_HANDLE;
    VkResult result=create(&info,NULL,&instance);
    if (result!=VK_SUCCESS) { snprintf(reason,sizeof(reason),"vkCreateInstance (Vulkan 1.3): %d",result); return gpu_refused(reason); }
#define LOAD(type,name) type name=(type)get(instance,#name)
    LOAD(PFN_vkDestroyInstance,vkDestroyInstance);
    LOAD(PFN_vkEnumeratePhysicalDevices,vkEnumeratePhysicalDevices);
    LOAD(PFN_vkGetPhysicalDeviceProperties2,vkGetPhysicalDeviceProperties2);
    LOAD(PFN_vkGetPhysicalDeviceFeatures,vkGetPhysicalDeviceFeatures);
    LOAD(PFN_vkGetPhysicalDeviceFormatProperties,vkGetPhysicalDeviceFormatProperties);
#undef LOAD
    if (!vkDestroyInstance || !vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceFeatures || !vkGetPhysicalDeviceFormatProperties) {
        if (vkDestroyInstance) vkDestroyInstance(instance,NULL);
        return gpu_refused("Vulkan driver: missing physical-device query functions");
    }
    uint32_t count=0;
    result=vkEnumeratePhysicalDevices(instance,&count,NULL);
    if (result!=VK_SUCCESS || count!=1) {
        vkDestroyInstance(instance,NULL);
        snprintf(reason,sizeof(reason),"vkEnumeratePhysicalDevices: result=%d, %u GPUs (one expected)",result,count);
        return gpu_refused(reason);
    }
    VkPhysicalDevice device=VK_NULL_HANDLE;
    result=vkEnumeratePhysicalDevices(instance,&count,&device);
    if (result!=VK_SUCCESS || count!=1) {
        vkDestroyInstance(instance,NULL);
        snprintf(reason,sizeof(reason),"vkEnumeratePhysicalDevices changed: result=%d",result);
        return gpu_refused(reason);
    }
    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};
    vkGetPhysicalDeviceProperties2(device,&properties);
    VkPhysicalDeviceFeatures features={0}; vkGetPhysicalDeviceFeatures(device,&features);
    VkFormatProperties bc1={0},bc3={0},bgra={0},astc={0},etc2={0};
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,&bc1);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_BC3_UNORM_BLOCK,&bc3);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_B8G8R8A8_UNORM,&bgra);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,&astc);
    vkGetPhysicalDeviceFormatProperties(device,VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK,&etc2);
#ifdef FP_GPU_SYSTEM
    const char *active_driver=getenv("FROSTPUNK_VULKAN_DRIVER");
    int panvk=driver.driverID==VK_DRIVER_ID_MESA_PANVK&&active_driver&&!strcmp(active_driver,"panvk");
    int valid=((((driver.driverID==VK_DRIVER_ID_ARM_PROPRIETARY || panvk) && properties.properties.vendorID==0x13b5) ||
        (driver.driverID==VK_DRIVER_ID_IMAGINATION_PROPRIETARY && properties.properties.vendorID==0x1010)) &&
        properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU && properties.properties.apiVersion>=VK_API_VERSION_1_3 &&
        features.textureCompressionASTC_LDR && (bgra.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        (astc.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
    int bc_sampled=features.textureCompressionBC && (bc1.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        (bc3.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    /* Mesa Turnip, the launcher's "Vulkan (Turnip)" row, opened by the APK through adrenotools: BC originals. */
    valid=valid||(driver.driverID==VK_DRIVER_ID_MESA_TURNIP && properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU &&
        properties.properties.apiVersion>=VK_API_VERSION_1_3 && bc_sampled);
    /* Qualcomm's own driver, the launcher's "Vulkan" row on Adreno (no longer refused by port policy since
     * 24 November 2026): judged like any other system driver by Vulkan 1.3 and the native renderer's needs.
     * The native renderer samples BC where the driver has it (realme RMX3851, Adreno 735, driver 1.3.128:
     * BC=1) and otherwise the ASTC or ETC2 conversion the launcher makes before the start. */
    valid=valid||(driver.driverID==VK_DRIVER_ID_QUALCOMM_PROPRIETARY && properties.properties.vendorID==0x5143 &&
        properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU && properties.properties.apiVersion>=VK_API_VERSION_1_3 &&
        (bgra.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
        (bc_sampled || (features.textureCompressionASTC_LDR && (astc.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ||
         (features.textureCompressionETC2 && (etc2.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))));
#else
    int valid=(driver.driverID==VK_DRIVER_ID_MESA_TURNIP && properties.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU &&
        properties.properties.apiVersion>=VK_API_VERSION_1_3 && features.textureCompressionBC &&
        (bc1.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (bc3.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
#endif
    fprintf(stderr,"GPU_%s device=%s driver_id=%u api=%u BC=%u ASTC=%u native_aarch64=1 wine_box64=0 offscreen=%d\n",
        valid?"READY":"STOP",properties.properties.deviceName,driver.driverID,properties.properties.apiVersion,features.textureCompressionBC,features.textureCompressionASTC_LDR,native_window==NULL);
    vkDestroyInstance(instance,NULL);
    if (!valid) {
        uint32_t api=properties.properties.apiVersion;
        snprintf(reason,sizeof(reason),"%s (driver id %u, Vulkan %u.%u.%u, BC %u, ASTC %u) does not meet the renderer's needs "
            "(Vulkan 1.3, the expected driver, B8G8R8A8 and BC, ASTC or ETC2 sampling)",properties.properties.deviceName,
            driver.driverID,(api>>22)&0x7fu,(api>>12)&0x3ffu,api&0xfffu,
            features.textureCompressionBC,features.textureCompressionASTC_LDR);
        port_renderer_failed(reason,0);
        return 0;
    }
    win64_d3d11_configure(get,native_window);
    return 1;
}
