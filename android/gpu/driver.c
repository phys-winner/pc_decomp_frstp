#include "driver.h"
#ifndef FP_GPU_SYSTEM
#include <adrenotools/driver.h>
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

PFN_vkGetInstanceProcAddr frostpunk_gpu_open(const char *runtime_dir,
    const char *driver_dir, char *error, size_t error_size) {
#ifdef FP_GPU_SYSTEM
    (void)runtime_dir;(void)driver_dir;
    return frostpunk_gpu_open_system(error,error_size);
#else
    if (!runtime_dir || !driver_dir || !*driver_dir || driver_dir[strlen(driver_dir)-1]!='/') {
        if (error && error_size) snprintf(error,error_size,"driver directory must end with slash");
        return NULL;
    }
    dlerror();
    void *loader=adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL,
        ADRENOTOOLS_DRIVER_CUSTOM,NULL,runtime_dir,driver_dir,"vulkan.ad07xx.so",NULL,NULL);
    PFN_vkGetInstanceProcAddr get=loader?(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr"):NULL;
    if (!get && error && error_size) {
        const char *detail=dlerror();
        snprintf(error,error_size,"adrenotools custom loader: %s",detail?detail:"no loader/proc returned");
    }
    return get;
#endif
}

PFN_vkGetInstanceProcAddr frostpunk_gpu_open_system(char *error, size_t error_size) {
    dlerror();
    void *loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr get=loader?(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr"):NULL;
    if(!get && error && error_size) {
        const char *detail=dlerror();
        snprintf(error,error_size,"system Vulkan loader: %s",detail?detail:"no loader/proc returned");
    }
    return get;
}

PFN_vkGetInstanceProcAddr frostpunk_gpu_open_turnip(const char *runtime_dir,
    const char *driver_dir, char *error, size_t error_size) {
    typedef void *(*open_libvulkan)(int,int,const char *,const char *,const char *,const char *,const char *,void **);
    if (!runtime_dir || !driver_dir || !*driver_dir || driver_dir[strlen(driver_dir)-1]!='/') {
        if (error && error_size) snprintf(error,error_size,"driver directory must end with slash");
        return NULL;
    }
    const char *driver_name="vulkan.ad07xx.so",*custom=getenv("PORT_TURNIP_DRIVER");
    char custom_dir[PATH_MAX];
    if(custom && custom[0]=='/' && strrchr(custom,'/')) {
        const char *slash=strrchr(custom,'/');size_t n=(size_t)(slash-custom)+1;
        if(n>=sizeof(custom_dir) || !slash[1])return NULL;
        memcpy(custom_dir,custom,n);custom_dir[n]=0;driver_dir=custom_dir;driver_name=slash+1;
    }
    /* libadrenotools is loaded only for this choice, so the system path never maps it. */
    dlerror();
    void *tools=dlopen("libadrenotools.so",RTLD_NOW|RTLD_LOCAL);
    open_libvulkan open=tools?(open_libvulkan)dlsym(tools,"adrenotools_open_libvulkan"):NULL;
    /* 1 = ADRENOTOOLS_DRIVER_CUSTOM: vulkan.ad07xx.so from driver_dir, hooks from runtime_dir. */
    void *loader=open?open(RTLD_NOW|RTLD_LOCAL,1,NULL,runtime_dir,driver_dir,driver_name,NULL,NULL):NULL;
    PFN_vkGetInstanceProcAddr get=loader?(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr"):NULL;
    if (!get && error && error_size) {
        const char *detail=dlerror();
        snprintf(error,error_size,"adrenotools custom loader: %s",detail?detail:"no loader/proc returned");
    }
    return get;
}
