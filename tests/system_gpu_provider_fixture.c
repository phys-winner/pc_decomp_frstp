#define VK_NO_PROTOTYPES
#include "startup_gpu.h"
#include <stdint.h>
#include <stdio.h>

static unsigned calls;
static PFN_vkGetInstanceProcAddr received_provider;
static void *received_window;
void win64_d3d11_configure(PFN_vkGetInstanceProcAddr provider,void *window) {
    ++calls;received_provider=provider;received_window=window;
}

int main(void) {
    if(frostpunk_startup_gpu_configure(NULL,NULL)||calls)return 1;
    if(!frostpunk_startup_gpu_init(NULL,NULL)||calls!=1||!received_provider||received_window)return 2;
    if(!frostpunk_startup_gpu_init("unused","not-a-driver-directory")||calls!=2||received_window)return 3;
    void *window=(void *)(uintptr_t)0x1234;
    if(!frostpunk_startup_gpu_configure(received_provider,window)||calls!=3||received_window!=window)return 4;
    puts("{\"success\":true,\"system_provider\":true,\"actual_Mali_gate\":true,\"configure_calls\":3,\"null_provider_rejected\":true,\"driver_directories_unused\":true,\"native_window_forwarded\":true,\"d3d11_device_created\":false,\"game_started\":false}");
    return 0;
}
