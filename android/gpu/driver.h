#pragma once
#include <stddef.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* FP_GPU_SYSTEM explicitly selects Android's system Vulkan and ignores the two
 * directories. Otherwise runtime_dir holds our hooks and driver_dir the pinned
 * Bionic Turnip driver with a trailing '/'. There is no automatic fallback. */
PFN_vkGetInstanceProcAddr frostpunk_gpu_open(const char *runtime_dir,
    const char *driver_dir, char *error, size_t error_size);
/* Runtime choice of the game APK: Android's system Vulkan, or Mesa Turnip
 * (Adreno only) through libadrenotools.so, which is loaded on demand from the
 * native library directory runtime_dir. The caller picks the fallback. */
PFN_vkGetInstanceProcAddr frostpunk_gpu_open_system(char *error, size_t error_size);
PFN_vkGetInstanceProcAddr frostpunk_gpu_open_turnip(const char *runtime_dir,
    const char *driver_dir, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
