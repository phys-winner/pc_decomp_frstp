/* Cold native OpenAL device enumeration. No guest CPU, device open or audio playback. */
#define _GNU_SOURCE
#include <AL/alc.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "driver.h"
#include "openal_got.inc"

static void mark(const char *phase) {
    fprintf(stderr, "COLD_ENUM pid=%ld phase=%s\n", (long)getpid(), phase);
    fflush(stderr);
}
static void binding(const char *symbol) {
    void *p = dlsym(RTLD_DEFAULT, symbol);
    Dl_info info = {0};
    if (p && dladdr(p, &info))
        fprintf(stderr, "BINDING symbol=%s library=%s offset=0x%llx\n", symbol,
            info.dli_fname, (unsigned long long)((uintptr_t)p - (uintptr_t)info.dli_fbase));
    else fprintf(stderr, "BINDING symbol=%s unavailable=1\n", symbol);
    fflush(stderr);
}
static int gpu_open(const char *helper, const char *directory) {
    mark("before-actual-GPU-provider");
    void *h = dlopen(helper, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!h) { fprintf(stderr, "GPU helper: %s\n", dlerror()); return 0; }
    PFN_vkGetInstanceProcAddr (*open_gpu)(const char *, const char *, char *, size_t);
    void *p = dlsym(h, "frostpunk_gpu_open"); memcpy(&open_gpu, &p, sizeof(p));
    if (!open_gpu) return 0;
    char path[4096], error[512] = {0};
    int n = snprintf(path, sizeof(path), "%s/", directory);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    PFN_vkGetInstanceProcAddr get = open_gpu(directory, path, error, sizeof(error));
    if (!get) { fprintf(stderr, "GPU provider: %s\n", error); return 0; }
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Cold enumeration control", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    if (!create || create(&ci, NULL, &instance) != VK_SUCCESS) return 0;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)get(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)get(instance, "vkGetPhysicalDeviceProperties");
    uint32_t count = 1; VkPhysicalDevice physical;
    if (!enumerate || !properties || enumerate(instance, &count, &physical) != VK_SUCCESS || count != 1) return 0;
    VkPhysicalDeviceProperties values; properties(physical, &values);
    fprintf(stderr, "GPU_PROVIDER device=%s api=%u\n", values.deviceName, values.apiVersion);
    if (!strstr(values.deviceName, "Turnip")) return 0;
    mark("after-actual-GPU-provider");
    /* Keep actual loader/instance alive to retain the provider namespace. */
    return 1;
}
static void got_bindings(void *lib) {
    void *function = dlsym(lib, "alcGetString"); Dl_info base = {0};
    void *vtt_point = NULL, *vtable = NULL;
    if (!function || !dladdr(function, &base)) _Exit(11);
    for (size_t i = 0; i < sizeof(openal_got) / sizeof(openal_got[0]); ++i) {
        void *resolved; Dl_info info = {0};
        memcpy(&resolved, (const char *)base.dli_fbase + openal_got[i].offset, sizeof(resolved));
        if (resolved && dladdr(resolved, &info)) {
            fprintf(stderr, "OPENAL_GOT symbol=%s relocation=%u library=%s offset=0x%llx\n", openal_got[i].name,
                openal_got[i].type, info.dli_fname, (unsigned long long)((uintptr_t)resolved - (uintptr_t)info.dli_fbase));
            if (openal_got[i].type == 1025 && !strstr(info.dli_fname, "/libc++_shared.so")) _Exit(12);
            if (!strncmp(openal_got[i].name, "_ZTT", 4)) {
                Dl_info point = {0}; memcpy(&vtt_point, resolved, sizeof(vtt_point));
                if (!vtt_point || !dladdr(vtt_point, &point)) _Exit(12);
                fprintf(stderr, "OPENAL_VTT first_addresspoint_library=%s offset=0x%llx\n", point.dli_fname,
                    (unsigned long long)((uintptr_t)vtt_point - (uintptr_t)point.dli_fbase));
                if (!strstr(point.dli_fname, "/libc++_shared.so")) _Exit(12);
            }
            if (!strncmp(openal_got[i].name, "_ZTV", 4)) vtable = resolved;
        } else { fprintf(stderr, "OPENAL_GOT symbol=%s unavailable=1\n", openal_got[i].name); _Exit(12); }
    }
    if (!vtable || vtt_point != (char *)vtable + 24) _Exit(12);
    fflush(stderr);
}
static size_t list_size(const char *s) {
    if (!s) return 0;
    if (!s[0]) return 1; /* Empty native lists need only their first NUL read. */
    for (size_t n = 0; n + 1 < 65536; ++n)
        if (!s[n] && !s[n + 1]) return n + 2;
    return SIZE_MAX;
}
int main(int argc, char **argv) {
    if (argc != 8) return 2;
    if (!strcmp(argv[2], "baseline")) {
        if (unsetenv("ALSOFT_CONF")) return 3;
    } else if (!strcmp(argv[2], "explicit") || !strcmp(argv[2], "adreno-global") || !strcmp(argv[2], "adreno-local") || !strcmp(argv[2], "gpu")) {
        if (setenv("ALSOFT_CONF", argv[3], 1)) return 3;
    } else return 2;
    if (!strcmp(argv[2], "gpu") && !gpu_open(argv[6], argv[7])) return 10;
    if (!strncmp(argv[2], "adreno-", 7)) {
        mark("before-adrenotools");
        int visibility = !strcmp(argv[2], "adreno-global") ? RTLD_GLOBAL : RTLD_LOCAL;
        if (!dlopen(argv[4], RTLD_NOW | visibility | RTLD_NODELETE)) {
            fprintf(stderr, "adrenotools dlopen: %s\n", dlerror()); return 4;
        }
    }
    mark("before-explicit-shared-STL");
    if (!dlopen(argv[5], RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE)) {
        fprintf(stderr, "shared STL dlopen: %s\n", dlerror()); return 4;
    }
    binding("_ZNSt6__ndk16localeD1Ev");
    binding("_ZNKSt6__ndk18ios_base6getlocEv");
    mark("before-dlopen");
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 4; }
    LPALCISEXTENSIONPRESENT extension;
    LPALCGETSTRING get_string;
    LPALCGETERROR get_error;
    void *p = dlsym(lib, "alcIsExtensionPresent"); memcpy(&extension, &p, sizeof(p));
    p = dlsym(lib, "alcGetString"); memcpy(&get_string, &p, sizeof(p));
    p = dlsym(lib, "alcGetError"); memcpy(&get_error, &p, sizeof(p));
    if (!extension || !get_string || !get_error) return 5;
    got_bindings(lib);
    mark("before-enumerate-all-extension");
    ALCboolean supported = extension(NULL, "ALC_ENUMERATE_ALL_EXT");
    mark("before-all-devices-1013");
    const char *list = get_string(NULL, 0x1013);
    mark("after-all-devices-1013");
    size_t bytes = list_size(list);
    if (!list || bytes == SIZE_MAX) return 6;
    mark("before-default-device-1012");
    const char *name = get_string(NULL, 0x1012);
    mark("after-default-device-1012");
    if (!name || strnlen(name, 65536) == 65536) return 7;
    size_t default_bytes = strlen(name) + 1;
    for (unsigned i = 0; i < 16; ++i) {
        if (!get_string(NULL, 0x1013) || !get_string(NULL, 0x1012)) return 8;
    }
    ALCenum error = get_error(NULL);
    printf("{\"success\":true,\"pid\":%ld,\"enumerate_all_extension\":%u,\"list_bytes\":%zu,\"default_bytes\":%zu,\"ALC_error\":%d,\"enumeration_calls\":34,\"shared_VTT_vtable_ctype_data\":true,\"VTT_first_equals_vtable_plus24\":true,\"device_opened\":false,\"original_game_executed\":false}\n",
        (long)getpid(), (unsigned char)supported, bytes, default_bytes, error);
    fflush(stdout);
    /* Avoid unrelated provider/global cleanup as this test isolates cold init. */
    _Exit(error ? 9 : 0);
}
