#define _GNU_SOURCE
#include "port_settings.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Percent of the Android surface; applied through the game's graphics transaction. */
static const uint32_t output_scales[] = {20, 30, 40, 50, 60, 67, 75, 80, 90, 100};
static char settings_path[PATH_MAX];
static pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t output_scale = 50, mobile_effects = 1;
static _Atomic uint32_t switch_graphics = 1;
static _Atomic uint32_t object_highlight = 1, ui_scale = 200, debug_menu = 0;
static _Atomic uint32_t touch_offset = 64, upscaler = 0;
static _Atomic uint32_t switch_ui = 1, switch_ui_active;
static _Atomic uint32_t frame_limit,display_limit=60;
static _Atomic uint32_t model_detail = 5;
/* 0 until port-settings.txt contains building_quality. The menu still shows the step that matches
 * the meshes already in use, so the row is not a second hidden default. */
static int building_quality_chosen;
static _Atomic uint32_t building_quality = 0;
static int free_building_chosen;
static _Atomic uint32_t free_building = 1;
static _Atomic uint32_t output_native_w, output_native_h;
static uint32_t frame_rates[FP_MAX_FRAME_LIMITS]={30,60},frame_rate_count=2;
static pthread_mutex_t rates_mutex=PTHREAD_MUTEX_INITIALIZER;
extern void fp_mobile_debug_menu_request(void) __attribute__((weak));
extern void frostpunk_android_request_frame_rate(uint32_t) __attribute__((weak));

uint32_t fp_port_frame_rates(uint32_t rates[FP_MAX_FRAME_LIMITS]){
    pthread_mutex_lock(&rates_mutex);uint32_t count=frame_rate_count;
    memcpy(rates,frame_rates,count*sizeof(uint32_t));pthread_mutex_unlock(&rates_mutex);return count;
}
static int valid_frame_rate(uint32_t value){
    if(!value)return 1;
    for(uint32_t i=0;i<frame_rate_count;++i)if(frame_rates[i]==value)return 1;
    return 0;
}
static int set_frame_rate(uint32_t value){
    pthread_mutex_lock(&rates_mutex);
    int valid=valid_frame_rate(value);
    if(valid)atomic_store(&frame_limit,value);
    pthread_mutex_unlock(&rates_mutex);
    return valid;
}
static int compare_frame_rates(const void *a,const void *b){
    return (int)*(const uint32_t*)a-(int)*(const uint32_t*)b;
}
void fp_port_frame_rates_configure(const uint32_t *rates,uint32_t count){
    if(!rates||!count||count>FP_MAX_FRAME_LIMITS/2)return;
    for(uint32_t i=0;i<count;++i)if(!rates[i]||rates[i]>1000||(i&&rates[i]<=rates[i-1]))return;
    uint32_t limits[FP_MAX_FRAME_LIMITS],length=0;
    for(uint32_t i=0;i<count;++i){
        limits[length++]=rates[i];
        if(rates[i]>1)limits[length++]=rates[i]/2;
    }
    qsort(limits,length,sizeof(limits[0]),compare_frame_rates);
    pthread_mutex_lock(&rates_mutex);
    frame_rate_count=0;
    for(uint32_t i=0;i<length;++i)
        if(!i||limits[i]!=limits[i-1])frame_rates[frame_rate_count++]=limits[i];
    atomic_store(&display_limit,rates[count-1]);
    if(!valid_frame_rate(atomic_load(&frame_limit)))atomic_store(&frame_limit,0);
    pthread_mutex_unlock(&rates_mutex);
}
__attribute__((visibility("default"))) uint32_t frostpunk_android_frame_limit(void){
    uint32_t limit=atomic_load(&frame_limit);return limit?limit:atomic_load(&display_limit);
}
/* debug.fp.set=ID=VALUE applies one port row from the render thread. Empty does nothing.
 * Output resolution and mobile post-processing also need the graphics transaction, which the
 * runtime hook performs; every other row is complete in fp_port_setting_set. */
extern void fp_port_debug_apply(void) __attribute__((weak));
void frostpunk_android_frame_pace(void){
    static _Thread_local uint64_t deadline;
    static _Thread_local uint32_t previous;
    static unsigned debug_tick;
    if(fp_port_debug_apply && !(debug_tick++ & 31u)) fp_port_debug_apply();
    int saved_errno=errno;uint32_t hz=frostpunk_android_frame_limit();struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)){errno=saved_errno;return;}
    uint64_t ns=(uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
    uint64_t interval=(UINT64_C(1000000000)+hz-1)/hz;
    if(hz!=previous||!deadline||ns>deadline+interval)deadline=ns;
    else{
        deadline+=interval;struct timespec target={.tv_sec=(time_t)(deadline/UINT64_C(1000000000)),.tv_nsec=(long)(deadline%UINT64_C(1000000000))};
        int status;do{status=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&target,NULL);}while(status==EINTR);
        if(status)deadline=0;
    }
    previous=hz;errno=saved_errno;
}

static int valid_scale(uint32_t value) {
    for (size_t i = 0; i < sizeof(output_scales) / sizeof(output_scales[0]); ++i)
        if (value == output_scales[i]) return 1;
    return 0;
}

void fp_output_display_size(uint32_t width, uint32_t height) {
    if (width < 2 || height < 2 || width > 16384 || height > 16384) return;
    atomic_store(&output_native_w, width);
    atomic_store(&output_native_h, height);
}

int fp_output_resolution_label(uint32_t percent, char *text, size_t text_n) {
    uint32_t width, height, out_w, out_h;
    int wrote;
    if (!text || text_n < 8 || !valid_scale(percent)) return 0;
    width = atomic_load(&output_native_w);
    height = atomic_load(&output_native_h);
    if (width < 2 || height < 2) return 0;
    out_w = (width * percent / 100u) & ~1u;
    out_h = (height * percent / 100u) & ~1u;
    if (out_w < 2 || out_h < 2) return 0;
    wrote = snprintf(text, text_n, "%ux%u (%u%%)", out_w, out_h, percent);
    return wrote > 0 && (size_t)wrote < text_n;
}

/* Same rungs as the graphics quality rows: Ultra low, Low, Medium, High, Very high.
 * Ultra low is the default mobile mesh budget when model detail is Auto. */
static int building_mesh_level(uint32_t quality) {
    static const int levels[] = {5, 4, 3, 2, 1};
    return quality < 5 ? levels[quality] : 3;
}

static uint32_t building_quality_shown(void) {
    uint32_t model;
    if (building_quality_chosen) return atomic_load(&building_quality);
    model = atomic_load(&model_detail);
    if (model >= 1 && model <= 5) return 5u - model;
    if (!atomic_load(&mobile_effects) || frostpunk_mobile_graphics_pc(FP_GFX_BUILDINGS)) return 4;
    return 0;
}

static void apply_meshes(void) {
    if (building_quality_chosen) frostpunk_geometry_detail(building_mesh_level(atomic_load(&building_quality)));
    else {
        int level = (int)atomic_load(&model_detail);
        if (!level && atomic_load(&mobile_effects) && !frostpunk_mobile_graphics_pc(FP_GFX_BUILDINGS))
            level = building_mesh_level(0);
        frostpunk_geometry_detail(level);
    }
}

static int resource_available(const char *name) {
    const char *value = getenv(name);
    return !value || strcmp(value, "0");
}

/* auto: under 5 GiB minimal, under 7 GiB reduced. The cap stays in memory; load does not save. */
static void apply_memory_mode(void) {
#if defined(__ANDROID__)
    const char *requested = getenv("PORT_OPTION_MEMORY");
    const char *mode = requested && requested[0] ? requested : "auto";
    if (!strcmp(mode, "auto")) {
        unsigned long total_kb = 0;
        FILE *mem = fopen("/proc/meminfo", "re");
        if (mem) {
            char line[96];
            while (fgets(line, sizeof(line), mem))
                if (sscanf(line, "MemTotal: %lu", &total_kb) == 1) break;
            fclose(mem);
        }
        mode = total_kb && total_kb < 5ul * 1024ul * 1024ul ? "minimal"
             : total_kb && total_kb < 7ul * 1024ul * 1024ul ? "reduced" : "normal";
        setenv("PORT_OPTION_MEMORY", mode, 1);
    }
    fprintf(stderr, "FP_MEMORY_MODE %s\n", mode);
    if (strcmp(mode, "reduced") && strcmp(mode, "minimal")) return;
    {
        uint32_t cap = !strcmp(mode, "minimal") ? 20u : 30u;
        uint32_t scale = atomic_load(&output_scale);
        if (scale > cap) atomic_store(&output_scale, cap);
    }
    setenv("PORT_OPTION_SWITCH_GRAPHICS", "1", 1);
    atomic_store(&switch_graphics, 1);
    setenv("PORT_OPTION_SNOW", "switch", 1);
    setenv("PORT_OPTION_BUILDINGS", "switch", 1);
    setenv("PORT_OPTION_TEXTURES", "switch", 1);
    setenv("PORT_OPTION_SHADOWS", "switch", 1);
    setenv("PORT_OPTION_EFFECTS", "switch", 1);
    atomic_store(&upscaler, 0);
    if (!getenv("DXVK_CONFIG"))
        setenv("DXVK_CONFIG", "dxgi.maxFrameLatency = 1\ndxvk.numCompilerThreads = 1", 0);
    {
        /* Under 6 GiB a developed city still filled the DXVK heap after one extra mip.
         * 256 is the longest edge of world textures; 6–7 GiB keeps the 512 edge. */
        unsigned long total_kb = 0;
        FILE *mem = fopen("/proc/meminfo", "re");
        if (mem) {
            char line[96];
            while (fgets(line, sizeof(line), mem))
                if (sscanf(line, "MemTotal: %lu", &total_kb) == 1) break;
            fclose(mem);
        }
        const char *edge = !strcmp(mode, "minimal") || (total_kb && total_kb < 6ul * 1024ul * 1024ul) ? "256" : "512";
        setenv("PORT_OPTION_TEXTURE_LIMIT", edge, 1);
        fprintf(stderr, "FP_MEMORY_BUDGET textures=%s\n", edge);
    }
#endif
}

void fp_port_settings_load(const char *directory) {
    const char *graphics = getenv("PORT_OPTION_SWITCH_GRAPHICS");
    atomic_store(&switch_graphics, !graphics || strcmp(graphics, "0"));
    /* The Switch interface is opt-in since 2026-11-14.  Earlier builds wrote "switch_ui=1" with every
     * save even when nobody chose it, so that key is ignored; a choice is stored as switch_interface. */
    atomic_store(&switch_ui, 0);
    atomic_store(&switch_ui_active, 0);
    building_quality_chosen = 0;
    free_building_chosen = 0;
    atomic_store(&free_building, 1);
    atomic_store(&model_detail, 5);
    /* The initial 50% output needs a 200% interface on phones and tablets. */
    atomic_store(&ui_scale, resource_available("FROSTPUNK_PORT_DATA") ? 200 : 100);
    int size = directory ? snprintf(settings_path, sizeof(settings_path), "%s/port-settings.txt", directory) : -1;
    if (size < 0 || (size_t)size >= sizeof(settings_path)) { settings_path[0] = 0; apply_memory_mode(); apply_meshes(); return; }
    FILE *file = fopen(settings_path, "re");
    if (!file) { apply_memory_mode(); apply_meshes(); return; }
    char line[128];
    unsigned value;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "output_scale=%u", &value) == 1 && valid_scale(value)) atomic_store(&output_scale, value);
        else if (sscanf(line, "mobile_effects=%u", &value) == 1 && value <= 1) atomic_store(&mobile_effects, value);
        else if (sscanf(line, "object_highlight=%u", &value) == 1 && value <= 1) atomic_store(&object_highlight, value);
        else if (sscanf(line, "ui_scale=%u", &value) == 1 && value >= 100 && value <= 200 && value % 10 == 0) atomic_store(&ui_scale, value);
        else if (sscanf(line, "touch_offset=%u", &value) == 1 && value <= 128 && value % 16 == 0) atomic_store(&touch_offset, value);
        else if (sscanf(line, "upscaler=%u", &value) == 1 && value <= 2) atomic_store(&upscaler, value);
        else if (sscanf(line, "frame_limit=%u", &value) == 1) set_frame_rate(value);
        else if (sscanf(line, "switch_interface=%u", &value) == 1 && value <= 1) atomic_store(&switch_ui, value);
        else if (sscanf(line, "model_detail=%u", &value) == 1 && value <= 5) atomic_store(&model_detail, value);
        else if (sscanf(line, "building_quality=%u", &value) == 1 && value <= 4) {
            atomic_store(&building_quality, value);
            building_quality_chosen = 1;
        }
        else if (sscanf(line, "free_building=%u", &value) == 1 && value <= 1) {
            atomic_store(&free_building, value);
            free_building_chosen = 1;
        }
    }
    fclose(file);
    if (!resource_available("FROSTPUNK_PORT_DATA")) atomic_store(&ui_scale, 100);
    if (!resource_available("FROSTPUNK_SWITCH_DATA")) atomic_store(&switch_ui, 0);
    atomic_store(&switch_ui_active, atomic_load(&switch_ui));
    apply_memory_mode();
    apply_meshes();
}

static void save(void) {
    char temporary[PATH_MAX + 8];
    if (!settings_path[0] || snprintf(temporary, sizeof(temporary), "%s.tmp", settings_path) >= (int)sizeof(temporary)) return;
    pthread_mutex_lock(&save_mutex);
    FILE *file = fopen(temporary, "we");
    int ok = file != NULL;
    if (ok) {
        ok = fprintf(file, "output_scale=%u\nmobile_effects=%u\nobject_highlight=%u\nui_scale=%u\ndebug_menu=%u\ntouch_offset=%u\nupscaler=%u\nframe_limit=%u\nswitch_interface=%u\nmodel_detail=%u\n",
                     (unsigned)atomic_load(&output_scale), (unsigned)atomic_load(&mobile_effects),
                     (unsigned)atomic_load(&object_highlight), (unsigned)atomic_load(&ui_scale),
                     (unsigned)atomic_load(&debug_menu), (unsigned)atomic_load(&touch_offset),
                     (unsigned)atomic_load(&upscaler),(unsigned)atomic_load(&frame_limit),
                     (unsigned)atomic_load(&switch_ui), (unsigned)atomic_load(&model_detail)) > 0;
        if (ok && building_quality_chosen)
            ok = fprintf(file, "building_quality=%u\n", (unsigned)atomic_load(&building_quality)) > 0;
        if (ok && free_building_chosen)
            ok = fprintf(file, "free_building=%u\n", (unsigned)atomic_load(&free_building)) > 0;
        ok = fflush(file) == 0 && ok;
        ok = fsync(fileno(file)) == 0 && ok;
        ok = fclose(file) == 0 && ok;
    }
    if (!ok || rename(temporary, settings_path)) {
        fprintf(stderr, "FP_PORT_SETTINGS save_failed errno=%d\n", errno);
        unlink(temporary);
    }
    pthread_mutex_unlock(&save_mutex);
}

uint32_t fp_port_setting_get(uint32_t id) {
    if (id == FP_SETTING_OUTPUT_SCALE) return atomic_load(&output_scale);
    if (id == FP_SETTING_MOBILE_EFFECTS) return atomic_load(&mobile_effects);
    if (id == FP_SETTING_OBJECT_HIGHLIGHT) return atomic_load(&object_highlight);
    if (id == FP_SETTING_UI_SCALE) return atomic_load(&ui_scale);
    if (id == FP_SETTING_DEBUG_MENU) return atomic_load(&debug_menu);
    if (id == FP_SETTING_TOUCH_OFFSET) return atomic_load(&touch_offset);
    if (id == FP_SETTING_UPSCALER) return atomic_load(&upscaler);
    if (id == FP_SETTING_FRAME_LIMIT) return atomic_load(&frame_limit);
    if (id == FP_SETTING_SWITCH_UI) return atomic_load(&switch_ui);
    if (id == FP_SETTING_MODEL_DETAIL) return atomic_load(&model_detail);
    if (id == FP_SETTING_BUILDING_MODELS) return building_quality_shown();
    if (id == FP_SETTING_FREE_BUILDING) return atomic_load(&free_building);
    return 0;
}

int fp_port_free_building_pending(void) { return !free_building_chosen; }

void fp_port_setting_set(uint32_t id, uint32_t value) {
    if (id == FP_SETTING_UI_SCALE && value > 100 && !resource_available("FROSTPUNK_PORT_DATA")) value = 100;
    if (id == FP_SETTING_SWITCH_UI && value && !resource_available("FROSTPUNK_SWITCH_DATA")) value = 0;
    if (id == FP_SETTING_OUTPUT_SCALE && valid_scale(value)) atomic_store(&output_scale, value);
    else if (id == FP_SETTING_MOBILE_EFFECTS && value <= 1) atomic_store(&mobile_effects, value);
    else if (id == FP_SETTING_OBJECT_HIGHLIGHT && value <= 1) atomic_store(&object_highlight, value);
    else if (id == FP_SETTING_UI_SCALE && value >= 100 && value <= 200 && value % 10 == 0) atomic_store(&ui_scale, value);
    else if (id == FP_SETTING_DEBUG_MENU && value <= 1) {
        atomic_store(&debug_menu, 0);
        if(value && fp_mobile_debug_menu_request)fp_mobile_debug_menu_request();
    }
    /* The translated setter opens help with the game's active localization. */
    else if (id == FP_SETTING_GESTURE_HELP && value <= 1) return;
    else if (id == FP_SETTING_TOUCH_OFFSET && value <= 128 && value % 16 == 0) atomic_store(&touch_offset, value);
    else if (id == FP_SETTING_UPSCALER && value <= 2) atomic_store(&upscaler, value);
    else if (id == FP_SETTING_SWITCH_UI && value <= 1) atomic_store(&switch_ui, value);
    else if (id == FP_SETTING_FRAME_LIMIT && set_frame_rate(value)){
        if(frostpunk_android_request_frame_rate)frostpunk_android_request_frame_rate(frostpunk_android_frame_limit());
    }
    else if (id == FP_SETTING_MODEL_DETAIL && value <= 5) {
        atomic_store(&model_detail, value);
        building_quality_chosen = 0;
    }
    else if (id == FP_SETTING_BUILDING_MODELS && value <= 4) {
        atomic_store(&building_quality, value);
        building_quality_chosen = 1;
    }
    else if (id == FP_SETTING_FREE_BUILDING && value <= 1) {
        atomic_store(&free_building, value);
        free_building_chosen = 1;
    }
    else { fprintf(stderr, "FP_PORT_SETTINGS rejected id=%u value=%u\n", id, value); return; }
    fprintf(stderr, "FP_PORT_SETTINGS set id=%u value=%u\n", id, value);
    if (id == FP_SETTING_MODEL_DETAIL || id == FP_SETTING_BUILDING_MODELS || id == FP_SETTING_MOBILE_EFFECTS) apply_meshes();
    save();
}

/* The bridge replaces this. Host tests keep the stored value and do not rewrite meshes. */
__attribute__((weak)) void frostpunk_geometry_detail(int level) { (void)level; }

__attribute__((visibility("default"))) int frostpunk_switch_ui(void) {
    return (int)atomic_load(&switch_ui_active);
}

__attribute__((visibility("default"))) int frostpunk_mobile_graphics_pc(unsigned group) {
    static const char *const keys[FP_GFX_COUNT] = {"PORT_OPTION_SNOW", "PORT_OPTION_BUILDINGS", "PORT_OPTION_TEXTURES",
                                                   "PORT_OPTION_SHADOWS", "PORT_OPTION_EFFECTS"};
    static const unsigned char defaults[FP_GFX_COUNT] = {1, 0, 0, 0, 0};
    static _Atomic int cached[FP_GFX_COUNT] = {-1, -1, -1, -1, -1};
    if (group >= FP_GFX_COUNT) return 0;
    if (!atomic_load(&switch_graphics)) return 1;
    {
        const char *memory = getenv("PORT_OPTION_MEMORY");
        if (memory && (!strcmp(memory, "reduced") || !strcmp(memory, "minimal"))) return 0;
    }
    int value = atomic_load_explicit(&cached[group], memory_order_relaxed);
    if (value < 0) {
        const char *text = getenv(keys[group]);
        value = text && !strcmp(text, "pc") ? 1 : text && !strcmp(text, "switch") ? 0 : defaults[group];
        atomic_store_explicit(&cached[group], value, memory_order_relaxed);
        fprintf(stderr, "FP_GRAPHICS_GROUP %s=%s\n", keys[group] + 12, value ? "pc" : "switch");
    }
    return value;
}

__attribute__((visibility("default"))) int frostpunk_mobile_graphics_reduced(unsigned group) {
    return atomic_load(&switch_graphics) && atomic_load(&mobile_effects) && !frostpunk_mobile_graphics_pc(group);
}

__attribute__((visibility("default"))) int frostpunk_dxvk_mobile_effects(void) {
    return (int)(atomic_load(&mobile_effects) && atomic_load(&switch_graphics));
}

__attribute__((visibility("default"))) int frostpunk_dxvk_upscaler(void) {
    return (int)atomic_load(&upscaler);
}

__attribute__((visibility("default"))) void frostpunk_dxvk_upscaler_failed(uint32_t error) {
    uint32_t expected = 2;
    if (!atomic_compare_exchange_strong(&upscaler, &expected, 1)) return;
    fprintf(stderr, "FP_PORT_SETTINGS fsr3_failed=%u fallback=FSR1\n", error);
    save();
}

__attribute__((visibility("default"))) int frostpunk_dxvk_object_highlight(void) {
    return (int)atomic_load(&object_highlight);
}
