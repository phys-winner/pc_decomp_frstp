/* Runtime side of the native Vulkan rendering device (see rdvk_abi.h).
 *
 * The library is selected once, before guest code runs: the launcher mode (PORT_RENDERER=native_vulkan),
 * FROSTPUNK_RENDERER=vulkan or debug.fp.renderer=vulkan
 * (development; =dxvk forces the old backend).  Without it every hook stub calls its original
 * lifted body and the game keeps the D3D11 backend on DXVK.
 *
 * Development builds load files/rdvk-dev/libfrostpunk_rdvk.so instead of the APK copy when
 * debug.fp.rdvk_dev=1, so the backend can be replaced with `adb push` + `run-as cp`.
 */
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include "rdvk_abi.h"
#include "win64.h"
#include "../launcher-base/native/replacements/renderer_failure.h"

/* Generated with the stubs (tools/build_rdvk.py hooks). */
extern const uint64_t fp_rdvk_hook_vas[];
extern rdvk_fn const fp_rdvk_originals[];
extern const unsigned fp_rdvk_hook_count;
__attribute__((visibility("hidden"))) rdvk_fn fp_rdvk_table[RDVK_HOOK_MAX];

void frostpunk_render_log(const char *line);
void frostpunk_render_present_result(int32_t result);
void *frostpunk_dxvk_get_instance_proc_addr(void);
int frostpunk_dxvk_get_display_mode(uint32_t *width, uint32_t *height, uint32_t *refresh);
void *frostpunk_window_acquire(uint64_t hwnd, uint64_t *generation);
void frostpunk_window_release(void *window);
void *frostpunk_window_current(void);
void (*frostpunk_static_target(uint64_t target))(CPU *);
void dispatch(CPU *c, uint64_t target);
extern void frostpunk_android_frame_pace(void) __attribute__((weak));
extern int frostpunk_dxvk_mobile_effects(void) __attribute__((weak));
extern int frostpunk_mobile_graphics_pc(unsigned group) __attribute__((weak));
extern int frostpunk_geometry_detail_level(void) __attribute__((weak));
extern int frostpunk_dxvk_object_highlight(void) __attribute__((weak));

static const rdvk_library *library;
static int active;
static rdvk_host host;

static void host_log(const char *line) { fprintf(stderr, "%s\n", line); fflush(stderr); }
static void host_render_log(const char *line) { frostpunk_render_log(line); }
static rdvk_fn host_static_target(uint64_t va) { return frostpunk_static_target(va); }
static void host_dispatch(CPU *c, uint64_t va) { dispatch(c, va); }
static void host_frame_pace(void) { if (frostpunk_android_frame_pace) frostpunk_android_frame_pace(); }
static void host_present_result(int32_t result) { frostpunk_render_present_result(result); }

/* The older launcher toggle is also accepted by the runtime-only development APK recipe. */
static int wanted(void) {
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.fp.renderer", value) > 0 && value[0]) return !strcmp(value, "vulkan");
    const char *env = getenv("FROSTPUNK_RENDERER");
    if (env && *env) return !strcmp(env, "vulkan");
    const char *renderer = getenv("PORT_RENDERER");
    /* The native Vulkan renderer is the only one of the APK: the launcher ids vulkan, vulkan-turnip and
     * vulkan-panvk, their earlier names and the removed DXVK ids all start it. debug.fp.renderer=dxvk
     * (above) still selects the D3D11 bridge for a development build that carries DXVK. */
    (void)renderer;
    return 1;
}

static const rdvk_library *entry_of(void *handle) {
    const rdvk_library *(*entry)(void) = handle ? (const rdvk_library *(*)(void))dlsym(handle, "frostpunk_rdvk_library") : NULL;
    const rdvk_library *candidate = entry ? entry() : NULL;
    return candidate && candidate->abi == RDVK_ABI ? candidate : NULL;
}

/* The development copy (debug.fp.rdvk_dev=1) when it loads and matches this runtime's ABI,
 * otherwise the APK's library. */
static const rdvk_library *open_library(char *where, size_t size) {
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.fp.rdvk_dev", value) > 0 && value[0] == '1') {
        const char *files = getenv("FROSTPUNK_FILES_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/rdvk-dev/libfrostpunk_rdvk.so", files && *files ? files : "/data/data/com.frostpunk.staticrecomp/files");
        if (!access(path, R_OK)) {
            void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            const rdvk_library *candidate = entry_of(handle);
            if (candidate) { snprintf(where, size, "%s", path); return candidate; }
            fprintf(stderr, "RDVK_DEV_LIBRARY_UNUSABLE %s %s; trying the APK library\n", path, handle ? "abi/entry" : dlerror());
            if (handle) dlclose(handle);
        }
    }
    snprintf(where, size, "libfrostpunk_rdvk.so");
    void *handle = dlopen("libfrostpunk_rdvk.so", RTLD_NOW | RTLD_LOCAL);
    const rdvk_library *candidate = entry_of(handle);
    if (!candidate) fprintf(stderr, "RDVK_UNAVAILABLE library=%s error=%s; this build has no other renderer\n", where, handle ? "abi/entry" : dlerror());
    return candidate;
}

/* The native renderer is the only one of the APK: when it cannot start the game stops and the launcher
 * offers the next renderer of its list (renderer failure policy, 24 November 2026) instead of the D3D11
 * bridge running into its missing-DXVK trap. Without the launcher's record path it ends as before. */
static void renderer_declined(const char *reason) {
    int recorded = port_renderer_failed(reason, 0);
    fprintf(stderr, "RDVK_RENDERER_FAILURE kind=start recorded=%d %s\n", recorded, reason);
    if (recorded != 0) return;
    fprintf(stderr, "STOP rdvk %s\n", reason);
    fflush(stderr);
    _Exit(73);
}

__attribute__((constructor)) static void rdvk_select(void) {
    if (fp_rdvk_hook_count > RDVK_HOOK_MAX) { fprintf(stderr, "STOP rdvk hook table too large\n"); _Exit(73); }
    if (!wanted()) return;
    char where[600] = {0};
    const rdvk_library *candidate = open_library(where, sizeof(where));
    if (!candidate) { renderer_declined("the native Vulkan renderer library could not be loaded (libfrostpunk_rdvk.so)"); return; }
    host.abi = RDVK_ABI;
    host.size = sizeof(host);
    host.hook_count = fp_rdvk_hook_count;
    for (unsigned i = 0; i < fp_rdvk_hook_count; ++i) { host.original[i] = fp_rdvk_originals[i]; host.hook_va[i] = fp_rdvk_hook_vas[i]; }
    host.log = host_log;
    host.render_log = host_render_log;
    host.vk_get_instance_proc_addr = frostpunk_dxvk_get_instance_proc_addr;
    host.window_acquire = frostpunk_window_acquire;
    host.window_release = frostpunk_window_release;
    host.window_native = frostpunk_window_current;
    host.static_target = host_static_target;
    host.dispatch = host_dispatch;
    host.guest_alloc = win64_alloc;
    host.guest_free = win64_free;
    const char *files = getenv("FROSTPUNK_FILES_DIR");
    host.files_dir = files && *files ? files : "/data/data/com.frostpunk.staticrecomp/files";
    host.display_mode = frostpunk_dxvk_get_display_mode;
    host.frame_pace = host_frame_pace;
    host.present_result = host_present_result;
    host.mobile_effects = frostpunk_dxvk_mobile_effects;
    host.mobile_graphics_pc = frostpunk_mobile_graphics_pc;
    host.geometry_detail = frostpunk_geometry_detail_level;
    host.object_highlight = frostpunk_dxvk_object_highlight;
    rdvk_fn table[RDVK_HOOK_MAX] = {0};
    if (!candidate->init(&host, table)) {
        fprintf(stderr, "RDVK_DECLINED library=%s; this build has no other renderer\n", where);
        renderer_declined("the native Vulkan renderer declined to start (shader library or library version, see RDVK lines)");
        return;
    }
    unsigned hooked = 0;
    for (unsigned i = 0; i < fp_rdvk_hook_count; ++i) if (table[i]) { fp_rdvk_table[i] = table[i]; ++hooked; }
    library = candidate;
    active = 1;
    fprintf(stderr, "RDVK_ACTIVE library=%s hooks=%u/%u\n", where, hooked, fp_rdvk_hook_count);
    fflush(stderr);
}

int frostpunk_rdvk_active(void) { return active; }

uint32_t frostpunk_rdvk_read_texture(uint64_t texture, uint32_t *width, uint32_t *height, uint32_t *format, uint8_t **rgba) {
    *rgba = NULL;
    if (!library || !library->read_texture) return 0x80004001u; /* E_NOTIMPL */
    return library->read_texture(texture, width, height, format, rgba);
}

int frostpunk_rdvk_dispatch(CPU *c, uint64_t target) {
    if (target < RDVK_SENTINEL || target - RDVK_SENTINEL >= RDVK_SENTINEL_BYTES) return 0;
    if (!library) {
        fprintf(stderr, "STOP rdvk COM call without the library target=0x%" PRIx64 "\n", target);
        _Exit(73);
    }
    return library->com_dispatch(c, target);
}
