/* Bounded custom-driver capability query through an existing exported
 * adrenotools API. No CPU translation, rendering, turbo or system replacement. */
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(*open_vulkan_fn)(int,int,const char *,const char *,const char *,const char *,const char *,void **);
static int failed(const char *stage) {
    const char *error=dlerror();
    fprintf(stderr,"STOP stage=%s detail=%s\n",stage,error?error:"no dlerror supplied");
    return 2;
}

static void *load(const char *dir, const char *name) {
    char path[1024];
    int n=snprintf(path,sizeof(path),"%s/%s",dir,name);
    if(n<0 || (size_t)n>=sizeof(path)) return NULL;
    return dlopen(path,RTLD_NOW|RTLD_GLOBAL);
}

int main(int argc,char **argv) {
    if(argc!=4) { fprintf(stderr,"Usage: turnip_probe <existing-hook-dir> <custom-driver-dir/> <driver-soname>\n"); return 64; }
    if(!load(argv[1],"libwinlator.so")) return failed("dlopen existing libwinlator dependency");
    void *helper=load(argv[1],"libvortekrenderer.so");
    if(!helper) return failed("dlopen existing adrenotools provider");
    open_vulkan_fn open_vulkan=(open_vulkan_fn)dlsym(helper,"adrenotools_open_libvulkan");
    if(!open_vulkan) return failed("dlsym adrenotools_open_libvulkan");
    dlerror();
    /* ADRENOTOOLS_DRIVER_CUSTOM=1; no file redirection, mapping or turbo. */
    void *loader=open_vulkan(RTLD_NOW|RTLD_LOCAL,1,NULL,argv[1],argv[2],argv[3],NULL,NULL);
    if(!loader) return failed("adrenotools_open_libvulkan custom");
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr");
    if(!get) return failed("custom loader vkGetInstanceProcAddr");
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    if(!create) return failed("custom loader vkCreateInstance");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk Turnip capability query",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance=VK_NULL_HANDLE;
    VkResult result=create(&ci,NULL,&instance);
    if(result!=VK_SUCCESS) { fprintf(stderr,"STOP stage=vkCreateInstance result=%d\n",result); return 2; }
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)get(instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures features=(PFN_vkGetPhysicalDeviceFeatures)get(instance,"vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFormatProperties formats=(PFN_vkGetPhysicalDeviceFormatProperties)get(instance,"vkGetPhysicalDeviceFormatProperties");
    PFN_vkGetPhysicalDeviceImageFormatProperties images=(PFN_vkGetPhysicalDeviceImageFormatProperties)get(instance,"vkGetPhysicalDeviceImageFormatProperties");
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
    if(!enumerate||!properties||!features||!formats||!images||!destroy) return failed("custom instance entrypoints");
    VkPhysicalDevice devices[8]; uint32_t count=8;
    result=enumerate(instance,&count,devices);
    if(result!=VK_SUCCESS || count!=1) { fprintf(stderr,"STOP stage=vkEnumeratePhysicalDevices result=%d count=%u\n",result,count); destroy(instance,NULL); return 2; }
    VkPhysicalDeviceProperties p; VkPhysicalDeviceFeatures f; VkFormatProperties bc;
    VkImageFormatProperties image={0};
    properties(devices[0],&p); features(devices[0],&f);
    formats(devices[0],VK_FORMAT_BC3_UNORM_BLOCK,&bc);
    result=images(devices[0],VK_FORMAT_BC3_UNORM_BLOCK,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                  VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&image);
    /* deviceName is an ASCII driver label; reject JSON metacharacters rather
     * than emitting ambiguous output from an unexpected provider. */
    if(strpbrk(p.deviceName,"\"\\\n\r")) { destroy(instance,NULL); return failed("device label encoding"); }
    int turnip=strstr(p.deviceName,"Turnip")!=NULL;
    int gate=turnip && p.apiVersion>=VK_API_VERSION_1_3 && f.textureCompressionBC &&
        (bc.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && result==VK_SUCCESS;
    printf("{\"success\":true,\"system_loader_only\":false,\"provider\":\"existing Winlator exported adrenotools API, CUSTOM flag only\","
           "\"device_name\":\"%s\",\"api_version_raw\":%u,\"api_version\":\"%u.%u.%u\",\"driver_version_raw\":%u,"
           "\"turnip_name_verified\":%s,\"textureCompressionBC\":%s,\"BC3_optimal_features\":%u,\"BC3_sampled_image\":%s,"
           "\"BC3_image_query_result\":%d,\"BC3_image_sample_counts\":%u,\"vulkan13_and_bc3_gate\":%s,"
           "\"original_game_rendered\":false,\"fps_measured\":false,\"scope\":\"No VkDevice, queue, shader, surface or rendering.\"}\n",
           p.deviceName,p.apiVersion,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion),p.driverVersion,
           turnip?"true":"false",f.textureCompressionBC?"true":"false",bc.optimalTilingFeatures,
           (bc.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)?"true":"false",result,image.sampleCounts,gate?"true":"false");
    destroy(instance,NULL); return gate?0:3;
}
