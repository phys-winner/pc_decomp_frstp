#define VK_NO_PROTOTYPES
#include "startup_gpu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static VkPhysicalDeviceProperties properties;
static VkPhysicalDeviceFeatures features;
static VkDriverId driver_id;
static VkFormat missing_format;
static unsigned configured, destroyed;
static PFN_vkVoidFunction VKAPI_CALL provider(VkInstance instance, const char *name);

PFN_vkGetInstanceProcAddr frostpunk_gpu_open(const char *runtime, const char *driver,
    char *error, size_t size) {
    (void)runtime; (void)driver; (void)error; (void)size;
    return provider;
}
void win64_d3d11_configure(PFN_vkGetInstanceProcAddr get, void *window) {
    assert(get == provider && window == (void *)0x1234);
    ++configured;
}
static VkResult VKAPI_CALL create(const VkInstanceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)allocator;
    assert(info->pApplicationInfo->apiVersion == VK_API_VERSION_1_3);
    *instance = (VkInstance)1;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator; assert(instance == (VkInstance)1); ++destroyed;
}
static VkResult VKAPI_CALL enumerate(VkInstance instance, uint32_t *count, VkPhysicalDevice *device) {
    assert(instance == (VkInstance)1);
    *count = 1;
    if (device) *device = (VkPhysicalDevice)2;
    return VK_SUCCESS;
}
static void VKAPI_CALL get_properties(VkPhysicalDevice device, VkPhysicalDeviceProperties2 *out) {
    assert(device == (VkPhysicalDevice)2);
    out->properties = properties;
    ((VkPhysicalDeviceDriverProperties *)out->pNext)->driverID = driver_id;
}
static void VKAPI_CALL get_features(VkPhysicalDevice device, VkPhysicalDeviceFeatures *out) {
    assert(device == (VkPhysicalDevice)2); *out = features;
}
static void VKAPI_CALL get_format(VkPhysicalDevice device, VkFormat format, VkFormatProperties *out) {
    assert(device == (VkPhysicalDevice)2);
    out->optimalTilingFeatures = format == missing_format ? 0 : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
}
static PFN_vkVoidFunction VKAPI_CALL provider(VkInstance instance, const char *name) {
    (void)instance;
#define PROC(n, f) if (!strcmp(name, n)) return (PFN_vkVoidFunction)f
    PROC("vkCreateInstance", create);
    PROC("vkDestroyInstance", destroy);
    PROC("vkEnumeratePhysicalDevices", enumerate);
    PROC("vkGetPhysicalDeviceProperties2", get_properties);
    PROC("vkGetPhysicalDeviceFeatures", get_features);
    PROC("vkGetPhysicalDeviceFormatProperties", get_format);
#undef PROC
    return NULL;
}
static void expect(int accepted) {
    unsigned before = configured, instances = destroyed;
    assert(frostpunk_startup_gpu_configure(provider, (void *)0x1234) == accepted);
    assert(configured == before + accepted && destroyed == instances + 1);
}
int main(void) {
#ifdef FP_GPU_SYSTEM
    const int system = 1;
#else
    const int system = 0;
#endif
    properties = (VkPhysicalDeviceProperties){.apiVersion = VK_API_VERSION_1_3,
        .vendorID = 0x1010, .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU};
    features.textureCompressionASTC_LDR = VK_TRUE;
    driver_id = VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
    assert(!frostpunk_startup_gpu_configure(NULL, NULL) && !configured);
    expect(system); /* PowerVR has ASTC without the BC feature. */
    properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU; expect(0);
    properties.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    properties.apiVersion = VK_API_VERSION_1_2; expect(0);
    properties.apiVersion = VK_API_VERSION_1_3;
    features.textureCompressionASTC_LDR = VK_FALSE; expect(0);
    features.textureCompressionASTC_LDR = VK_TRUE;
    missing_format = VK_FORMAT_ASTC_4x4_UNORM_BLOCK; expect(0);
    missing_format = VK_FORMAT_B8G8R8A8_UNORM; expect(0);
    missing_format = VK_FORMAT_UNDEFINED;
    driver_id = VK_DRIVER_ID_ARM_PROPRIETARY; expect(0);
    properties.vendorID = 0x13b5; expect(system);
    properties.vendorID = 0x5143; driver_id = VK_DRIVER_ID_QUALCOMM_PROPRIETARY; expect(0);
    driver_id = VK_DRIVER_ID_MESA_TURNIP; expect(0);
    features.textureCompressionBC = VK_TRUE; expect(1);
    missing_format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; expect(0);
    missing_format = VK_FORMAT_BC3_UNORM_BLOCK; expect(0);
    puts("STARTUP_GPU PowerVR_ASTC Mali_ASTC Turnip_BC invalid_profiles_rejected PASS");
    return 0;
}
