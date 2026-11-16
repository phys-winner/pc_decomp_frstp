#include "../app/host_api.h"
#include "win64_window.h"
#include "win64_display.h"
#include "process.h"
#include "frame_profile.h"
#include "port_settings.h"
#include "switch_performance.h"
#include "cpu64.h"
#include "../dxvk/bridge.h"
#include <android/native_window.h>
#include <sys/system_properties.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
static void *resolve_guest_window(uint64_t hwnd,uint64_t *generation){return win64_window_acquire(hwnd,generation);}
static void release_guest_window(void *window){win64_window_release(window);}
static uint32_t display_width,display_height;
/* Landscape surface aspect for the UI layout: the HUD canvas stays 16:9 and the engine centres it,
 * so a wider phone shows (height*aspect-width)/2 extra canvas units on each side. */
float frostpunk_display_aspect(void){return display_width&&display_height?(float)display_width/(float)display_height:16.f/9.f;}
extern uint32_t frostpunk_android_frame_rates(uint32_t*,uint32_t) __attribute__((weak));
extern void frostpunk_android_request_frame_rate(uint32_t) __attribute__((weak));
void fp_mobile_output_config(uint64_t config,uint32_t percent);

void fp_port_debug_apply(void) {
    fp_fx_apply();
    static char seen[PROP_VALUE_MAX];
    char value[PROP_VALUE_MAX]={0};
    int saved=errno;
    if(__system_property_get("debug.fp.set",value)<=0){seen[0]=0;errno=saved;return;}
    if(!strcmp(seen,value)){errno=saved;return;}
    snprintf(seen,sizeof(seen),"%s",value);
    unsigned id=0,chosen=0;
    if(sscanf(value,"%u=%u",&id,&chosen)==2){
        fp_port_setting_set(id,chosen);
        uint64_t root=rd64(UINT64_C(0x142985AB0));
        if(root && id==FP_SETTING_OUTPUT_SCALE && fp_port_setting_get(id)==chosen) fp_mobile_output_config(root+0xB0u,chosen);
        if(root && id==FP_SETTING_MOBILE_EFFECTS && chosen<=1u) wr8(root+0xB0u+0x2cu,1);
    }
    errno=saved;
}

void fp_mobile_output_config(uint64_t config,uint32_t percent) {
    if (!display_width || !display_height || percent<20 || percent>100) return;
    uint32_t width=(display_width*percent/100)&~1u,height=(display_height*percent/100)&~1u;
    uint64_t modes=rd64(config+0x48);uint32_t count=rd32(config+0x50);
    if(!modes || !count || count>4096)return;
    /* Use the same pending RendererConfig fields and dirty flag as Resolution. */
    uint32_t index=rd32(config+0x30);if(index>=count)index=0;
    uint32_t refresh=frostpunk_android_frame_limit();
    wr32(modes+index*12,width);wr32(modes+index*12+4,height);wr32(modes+index*12+8,refresh);
    wr32(config+0x34,width);wr32(config+0x38,height);wr32(config+0x3c,refresh);wr8(config+0x2c,1);
    win64_display_configure(width,height,refresh);
    win64_d3d11_set_display_mode(width,height,refresh);
    fprintf(stderr,"FP_OUTPUT_RESOLUTION percent=%u pending=%ux%u live=1\n",percent,width,height);
}

__attribute__((visibility("default"))) int frostpunk_android_run(const FrostpunkAndroidHost *host) {
    if (!host || host->abi_version!=1 || host->struct_size<FP_ANDROID_HOST_BASE_SIZE || !host->context ||
        !host->acquire_window || !host->release_window || !host->poll_event || !host->wait ||
        !host->wake || !host->is_stopping || !host->vulkan_provider || !host->game_path ||
        !host->profile_path || !host->native_library_path || !host->gpu_driver_path) return 64;
    if (host->is_stopping(host->context)) return 0;
    uint64_t generation=0;ANativeWindow *window=host->acquire_window(host->context,&generation);
    if (!window) return 69;
    PFN_vkGetInstanceProcAddr provider=(PFN_vkGetInstanceProcAddr)host->vulkan_provider(host->context);
    if (!provider) { host->release_window(window);return 69; }
    const char *active_driver=getenv("FROSTPUNK_VULKAN_DRIVER");char hardware[PROP_VALUE_MAX]={0};
    // shortcut: Mali warm state precompile can blank the frame, restore it after fixing the cached pipeline path.
    int disable_state_cache=active_driver&&!strcmp(active_driver,"system")&&
        __system_property_get("ro.hardware.vulkan",hardware)>0&&!strcmp(hardware,"mali");
    char executable[PATH_MAX];int size=snprintf(executable,sizeof(executable),"%s/Frostpunk.exe",host->game_path);
    /* Pipeline caches live in app-private storage.  The native Mali cache (FPVKPC02) trusts only an
     * owner-only directory, and the shared /sdcard/Frostpunk profile (0770) never qualifies: since
     * profiles moved there no session reused a compiled pipeline, and with pipeline libraries
     * unsupported every first draw compiled synchronously (the first world-map open stalled ~1.1 s
     * on each start on the Pixel 10a).  Keys already name device, driver and DXVK build, so one
     * folder serves every profile. */
    char pipeline_cache[PATH_MAX];const char *cache_path=host->profile_path;
    if (host->private_path && host->private_path[0]=='/' &&
        snprintf(pipeline_cache,sizeof(pipeline_cache),"%s/pipeline-cache",host->private_path)<(int)sizeof(pipeline_cache) &&
        (!mkdir(pipeline_cache,0700) || errno==EEXIST) && !chmod(pipeline_cache,0700)) cache_path=pipeline_cache;
    if (size<0 || (size_t)size>=sizeof(executable) ||
        setenv("FROSTPUNK_GPU_RUNTIME",host->native_library_path,1) ||
        setenv("FROSTPUNK_GPU_DRIVER",host->gpu_driver_path,1) ||
        setenv("FROSTPUNK_SERVICES_RUNTIME",host->native_library_path,1) ||
        setenv("FROSTPUNK_SERVICES_PROFILE",host->profile_path,1) ||
        setenv("DXVK_STATE_CACHE_PATH",cache_path,1) ||
        (disable_state_cache && setenv("DXVK_STATE_CACHE","0",1)) ||
        setenv("FROSTPUNK_GAME_ROOT",host->game_path,1)) { host->release_window(window);return 71; }
    /* The guest desktop matches the real surface, including a portrait quarter-turn. A 180 degree
     * flip keeps the same size, so an already-running swapchain does not have to be rebuilt. */
    int32_t surface_width=ANativeWindow_getWidth(window),surface_height=ANativeWindow_getHeight(window);
    if (surface_width<=1 || surface_height<=1 || surface_width>16384 || surface_height>16384) { host->release_window(window);return 69; }
    uint32_t width=(uint32_t)surface_width,height=(uint32_t)surface_height;
    display_width=width;display_height=height;
    fp_output_display_size(width, height);
    /* Graphics menu "Output resolution" (debug.frostpunk.display_scale 0.2-1 overrides it for tests)
     * shrinks the desktop for GPU budget; DXVK presents the smaller back buffer and the Android
     * compositor scales it to the surface. The graphics transaction also applies it live. */
    uint32_t rates[32],rate_count=frostpunk_android_frame_rates?frostpunk_android_frame_rates(rates,32):0;
    if(rate_count)fp_port_frame_rates_configure(rates,rate_count);
    fp_port_settings_load(host->profile_path);
    /* The window asked for the display's highest rate before the saved frame limit was known
     * (a 144 Hz panel kept running at 120-144 Hz under a 60 FPS limit): ask again for the limit. */
    if(frostpunk_android_request_frame_rate)frostpunk_android_request_frame_rate(frostpunk_android_frame_limit());
    char scale_text[PROP_VALUE_MAX]={0};
    double scale=__system_property_get("debug.frostpunk.display_scale",scale_text)>0?strtod(scale_text,NULL):
                 fp_port_setting_get(FP_SETTING_OUTPUT_SCALE)/100.0;
    if (scale>=0.2 && scale<1.0) { width=(uint32_t)(width*scale)&~1u; height=(uint32_t)(height*scale)&~1u; } else scale=1.0;
    char render_line[160];
    snprintf(render_line,sizeof(render_line),"display surface=%dx%d guest=%ux%u scale=%.2f",surface_width,surface_height,width,height,scale);
    frostpunk_render_log(render_line);
    win64_window_configure(host);
    uint32_t refresh=frostpunk_android_frame_limit();
    win64_display_configure(width,height,refresh);
    win64_d3d11_set_display_mode(width,height,refresh);
    win64_d3d11_set_window_resolver(resolve_guest_window,release_guest_window);
    fprintf(stderr,"ANDROID_RUNTIME startup_requested=1 host_abi=1 window_generation=%llu CPU_static_only=1 guest_virtual_display=%ux%u@%u surface=%dx%d display_scale=%.2f mobile_effects=%d state_cache_disabled=%d\n",
            (unsigned long long)generation,width,height,refresh,surface_width,surface_height,scale,frostpunk_dxvk_mobile_effects(),disable_state_cache);
    if(host->set_status)host->set_status(host->context,"");
    char program[]="Frostpunk ARM64",mode[]="--run-local";char *arguments[]={program,mode,executable,NULL};
    int result=frostpunk_process_main(3,arguments,provider,window);
    fp_frame_profile_stop();
    host->release_window(window);return result;
}
