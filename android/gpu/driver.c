#include "driver.h"
#include "../launcher-base/native/panvk_identity.h"
#ifndef FP_GPU_SYSTEM
#include <adrenotools/driver.h>
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

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

static PFN_vkGetInstanceProcAddr open_custom(const char *runtime_dir, const char *driver_dir,
    const char *driver_name, char *error, size_t error_size) {
    char folder[PATH_MAX];
    if (driver_name[0]=='/') {
        const char *slash=strrchr(driver_name,'/');
        size_t size=(size_t)(slash-driver_name)+1;
        if (size>=sizeof(folder) || !slash[1]) {
            snprintf(error,error_size,"Invalid custom Vulkan driver path");
            return NULL;
        }
        memcpy(folder,driver_name,size);
        folder[size]=0;
        driver_dir=folder;
        driver_name=slash+1;
    }
    typedef void *(*open_libvulkan)(int,int,const char *,const char *,const char *,const char *,const char *,void **);
    dlerror();
    void *tools=dlopen("libadrenotools.so",RTLD_NOW|RTLD_LOCAL);
    open_libvulkan open=tools?(open_libvulkan)dlsym(tools,"adrenotools_open_libvulkan"):NULL;
    /* ADRENOTOOLS_DRIVER_CUSTOM only: no KGSL import or file-redirection hooks. */
    void *loader=open?open(RTLD_NOW|RTLD_LOCAL,1,NULL,runtime_dir,driver_dir,driver_name,NULL,NULL):NULL;
    PFN_vkGetInstanceProcAddr get=loader?(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr"):NULL;
    if (!get && error && error_size) {
        const char *detail=dlerror();
        snprintf(error,error_size,"adrenotools custom loader: %s",detail?detail:"no loader/proc returned");
    }
    return get;
}

PFN_vkGetInstanceProcAddr frostpunk_gpu_open_turnip(const char *runtime_dir,
    const char *driver_dir, char *error, size_t error_size) {
    if (!runtime_dir || !driver_dir || !*driver_dir || driver_dir[strlen(driver_dir)-1]!='/') {
        if (error && error_size) snprintf(error,error_size,"driver directory must end with slash");
        return NULL;
    }
    const char *custom=getenv("PORT_TURNIP_DRIVER");
    return open_custom(runtime_dir,driver_dir,custom && custom[0]=='/' ? custom : "vulkan.ad07xx.so",error,error_size);
}

static VkResult panvk_device_check(PFN_vkGetInstanceProcAddr get,VkInstance instance,VkPhysicalDevice physical) {
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)get(instance,"vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create=(PFN_vkCreateDevice)get(instance,"vkCreateDevice");
    PFN_vkDestroyDevice destroy=(PFN_vkDestroyDevice)get(instance,"vkDestroyDevice");
    if(!queues||!create||!destroy)return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count=0;queues(physical,&count,NULL);
    if(!count)return VK_ERROR_INITIALIZATION_FAILED;
    VkQueueFamilyProperties *families=calloc(count,sizeof(*families));
    if(!families)return VK_ERROR_OUT_OF_HOST_MEMORY;
    queues(physical,&count,families);
    uint32_t family=0;
    const VkQueueFlags required=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;
    for(;family<count;++family)
        if(families[family].queueCount&&(families[family].queueFlags&required)==required)break;
    free(families);
    if(family==count)return VK_ERROR_INITIALIZATION_FAILED;
    float priority=1.0f;
    VkDeviceQueueCreateInfo queue={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVulkan13Features features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2=VK_TRUE,.dynamicRendering=VK_TRUE};
    const char *swapchain=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue,.enabledExtensionCount=1,.ppEnabledExtensionNames=&swapchain};
    VkDevice device=VK_NULL_HANDLE;
    VkResult result=create(physical,&info,NULL,&device);
    if(result==VK_SUCCESS)destroy(device,NULL);
    return result;
}

PFN_vkGetInstanceProcAddr frostpunk_gpu_open_panvk(const char *runtime_dir, char *error, size_t error_size) {
    char hooks[PATH_MAX], bundled[PATH_MAX];
    const char *custom=getenv("PORT_PANVK_DRIVER");
    int has_custom=custom && *custom;
    const char *page_error=port_panvk_page_size_error(!has_custom);
    if (page_error) {
        snprintf(error,error_size,"%s",page_error);
        return NULL;
    }
    int n=snprintf(hooks,sizeof(hooks),"%s/",runtime_dir);
    int m=snprintf(bundled,sizeof(bundled),"%s/" PORT_PANVK_DRIVER,runtime_dir);
    const char *path=has_custom ? custom : bundled;
    if (n<0 || (size_t)n>=sizeof(hooks) || m<0 || (size_t)m>=sizeof(bundled) || path[0]!='/' || access(path,R_OK)) {
        snprintf(error,error_size,"Cannot read PanVK driver at %s",path);
        return NULL;
    }
    PFN_vkGetInstanceProcAddr get=open_custom(hooks,hooks,path,error,error_size);
    if(!get)return NULL;
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(NULL,"vkCreateInstance");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    const char *extensions[]={VK_KHR_SURFACE_EXTENSION_NAME,"VK_KHR_android_surface"};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .enabledExtensionCount=2,.ppEnabledExtensionNames=extensions};
    VkInstance instance=VK_NULL_HANDLE;
    VkResult result=create?create(&info,NULL,&instance):VK_ERROR_INITIALIZATION_FAILED;
    if(result!=VK_SUCCESS){
        snprintf(error,error_size,"PanVK vkCreateInstance failed (%d)",result);
        return NULL;
    }
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 properties=(PFN_vkGetPhysicalDeviceProperties2)get(instance,"vkGetPhysicalDeviceProperties2");
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
    uint32_t count=1;VkPhysicalDevice device=VK_NULL_HANDLE;
    result=enumerate?enumerate(instance,&count,&device):VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};
    if((result==VK_SUCCESS||result==VK_INCOMPLETE)&&count&&device&&properties)properties(device,&props);
    int mapped=port_panvk_mapped(path);
    int valid=mapped&&props.properties.vendorID==0x13b5&&driver.driverID==VK_DRIVER_ID_MESA_PANVK;
    if(valid)result=panvk_device_check(get,instance,device);
    if(destroy)destroy(instance,NULL);
    if(!valid){
        snprintf(error,error_size,"PanVK driver not active (driver ID %d, library mapped %d)",driver.driverID,mapped);
        return NULL;
    }
    fprintf(stderr,"PANVK_DEVICE_CHECK device=%s vkCreateDevice=%d\n",props.properties.deviceName,result);
    if(result!=VK_SUCCESS){
        snprintf(error,error_size,"PanVK vkCreateDevice failed (%d)",result);
        return NULL;
    }
    return get;
}
