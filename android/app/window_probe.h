#pragma once
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct window_probe window_probe;
window_probe *window_probe_start(ANativeWindow *window,PFN_vkGetInstanceProcAddr provider,char *error,size_t error_size);
int window_probe_frame(window_probe *,float x,float y);
void window_probe_stop(window_probe *);
const char *window_probe_device(window_probe *);
uint64_t window_probe_frames(window_probe *);
void window_probe_extent(window_probe *,uint32_t *width,uint32_t *height);
const char *window_probe_details(window_probe *);
#ifdef __cplusplus
}
#endif
