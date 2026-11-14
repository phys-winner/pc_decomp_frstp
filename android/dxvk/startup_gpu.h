#pragma once
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Loads the explicit provider and validates Turnip/BC or system Mali/PowerVR ASTC,
 * then configures the Win64 COM bridge. No VkDevice or window is created. */
int frostpunk_startup_gpu_init(const char *runtime_dir,const char *driver_dir);
/* The embedding Android app retains provider/window for the DXVK lifetime. */
int frostpunk_startup_gpu_configure(PFN_vkGetInstanceProcAddr provider,void *native_window);
#ifdef __cplusplus
}
#endif
