#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ANativeWindow ANativeWindow;
enum { FP_ANDROID_WINDOW=1,FP_ANDROID_FOCUS=2,FP_ANDROID_MOTION=3,FP_ANDROID_KEY=4,FP_ANDROID_DESTROY=5,FP_ANDROID_TEXT=6 };
typedef struct {
    /* repeat is the cumulative key repeat, or a synthetic pointer acknowledgement ticket. */
    uint32_t type,action,source,keycode,scancode,meta,buttons,repeat;
    uint64_t time_ns,window_generation;
    int32_t pointer_id,width,height,focused;
    float x,y,scroll_x,scroll_y;
} FrostpunkAndroidEvent;
typedef struct {
    uint32_t abi_version,struct_size;
    void *context;
    /* Each non-NULL acquire owns a real ANativeWindow reference until release.
     * Generation changes on lifecycle/re-size. A retained reference keeps the
     * object valid, not the Android consumer; surface-lost still needs handling. */
    ANativeWindow *(*acquire_window)(void *,uint64_t *generation);
    void (*release_window)(ANativeWindow *);
    int (*poll_event)(void *,FrostpunkAndroidEvent *,int timeout_ms);
    int (*wait)(void *,int timeout_ms);
    void (*wake)(void *);
    int (*is_stopping)(void *);
    void (*set_status)(void *,const char *text);
    const char *game_path,*profile_path,*private_path,*native_library_path,*gpu_driver_path;
    /* Return PFN_vkGetInstanceProcAddr as a POSIX function address. The provider
     * and its libraries remain loaded for the application's lifetime. */
    void *(*vulkan_provider)(void *);
    /* Optional ABI1 tail. Return 1 only when the real Activity accepts the UI
     * pointer-icon request, 0 on host/JNI failure. Applied state is asynchronous.
     * visible selects TYPE_ARROW; zero selects a non-NULL TYPE_NULL icon. */
    int (*set_pointer_visible)(void *,int visible);
    /* Optional ABI1 tail. Non-consuming checkpoint of input publication and
     * explicit wakes. It changes before a newly queued event becomes visible. */
    uint64_t (*event_sequence)(void *);
} FrostpunkAndroidHost;
#define FP_ANDROID_HOST_BASE_SIZE offsetof(FrostpunkAndroidHost,set_pointer_visible)
/* Probe APK exposes the platform contract, not any successful Win64 HWND API.
 * A later runtime adapter may call its translated process entry on a dedicated
 * worker and consume these events; Android's app-glue looper keeps running. */
const FrostpunkAndroidHost *frostpunk_android_host(void);
#ifdef __cplusplus
}
#endif
