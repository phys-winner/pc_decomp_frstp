/* Query the actual system Vulkan provider, then try the unmodified DXVK DSO. */
#define VK_NO_PROTOTYPES
#include <d3d11.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include "driver.h"

static PFN_vkGetInstanceProcAddr provider;
extern "C" __attribute__((visibility("default")))
PFN_vkGetInstanceProcAddr frostpunk_dxvk_get_instance_proc_addr() { return provider; }
extern "C" __attribute__((visibility("default")))
int frostpunk_dxvk_get_display_mode(uint32_t*,uint32_t*,uint32_t*) { return 0; }

int main(int argc,char **argv) {
    if(argc!=2) { std::fprintf(stderr,"Usage: mali_device_probe <DXVK D3D11 library>\n");return 64; }
#ifdef FP_GPU_SYSTEM
    char detail[512]={};provider=frostpunk_gpu_open(nullptr,nullptr,detail,sizeof(detail));
    if(!provider){std::fprintf(stderr,"SYSTEM_VULKAN_STOP %s\n",detail);return 70;}
#else
    void *loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(!loader) { std::fprintf(stderr,"SYSTEM_VULKAN_STOP %s\n",dlerror());return 70; }
    provider=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader,"vkGetInstanceProcAddr"));
#endif
    if(!provider)return 71;
    auto create_instance=reinterpret_cast<PFN_vkCreateInstance>(provider(nullptr,"vkCreateInstance"));
    if(!create_instance)return 72;
    VkApplicationInfo app={};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName="Frostpunk system Mali capability proof";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci={};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&app;
    VkInstance instance=nullptr;VkResult vr=create_instance(&ci,nullptr,&instance);
    if(vr!=VK_SUCCESS) { std::fprintf(stderr,"INSTANCE_STOP %d\n",vr);return 73; }
#define LOAD(type,name) auto name=reinterpret_cast<type>(provider(instance,#name));if(!name)return 74
    LOAD(PFN_vkDestroyInstance,vkDestroyInstance);
    LOAD(PFN_vkEnumeratePhysicalDevices,vkEnumeratePhysicalDevices);
    LOAD(PFN_vkGetPhysicalDeviceProperties2,vkGetPhysicalDeviceProperties2);
    LOAD(PFN_vkGetPhysicalDeviceFeatures,vkGetPhysicalDeviceFeatures);
    LOAD(PFN_vkGetPhysicalDeviceFormatProperties,vkGetPhysicalDeviceFormatProperties);
#undef LOAD
    uint32_t count=0;vr=vkEnumeratePhysicalDevices(instance,&count,nullptr);
    if(vr!=VK_SUCCESS||count!=1)return 75;
    VkPhysicalDevice physical=nullptr;vr=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if(vr!=VK_SUCCESS||count!=1)return 76;
    VkPhysicalDeviceDriverProperties driver={};driver.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props={};props.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;props.pNext=&driver;
    vkGetPhysicalDeviceProperties2(physical,&props);
    VkPhysicalDeviceFeatures f={};vkGetPhysicalDeviceFeatures(physical,&f);
    if(props.properties.vendorID!=0x13b5||props.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU||
       !std::strstr(props.properties.deviceName,"Mali-G615")||
       std::strpbrk(props.properties.deviceName,"\"\\\n\r"))return 77;
    std::printf("{\"system_vulkan\":true,\"device\":\"%s\",\"vendor_id\":%u,\"driver_id\":%u,\"api_version\":%u,\"features\":{",
        props.properties.deviceName,props.properties.vendorID,driver.driverID,props.properties.apiVersion);
#define FEATURE(name) std::printf("\"" #name "\":%u,",f.name)
    FEATURE(textureCompressionBC);FEATURE(textureCompressionASTC_LDR);FEATURE(textureCompressionETC2);
    FEATURE(dualSrcBlend);FEATURE(fillModeNonSolid);FEATURE(multiViewport);
    FEATURE(shaderClipDistance);FEATURE(shaderCullDistance);FEATURE(geometryShader);FEATURE(tessellationShader);
#undef FEATURE
    std::printf("\"vertexPipelineStoresAndAtomics\":%u},\"formats\":[",f.vertexPipelineStoresAndAtomics);
    const VkFormat formats[]={VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_ASTC_4x4_UNORM_BLOCK,VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
        VK_FORMAT_ASTC_6x6_UNORM_BLOCK,VK_FORMAT_ASTC_6x6_SRGB_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK};
    for(unsigned i=0;i<sizeof(formats)/sizeof(formats[0]);++i) {
        VkFormatProperties p={};vkGetPhysicalDeviceFormatProperties(physical,formats[i],&p);
        std::printf("%s{\"format\":%u,\"optimal_features\":%u}",i?",":"",formats[i],p.optimalTilingFeatures);
    }
    std::printf("],\"d3d11_attempts\":[");std::fflush(stdout);
    vkDestroyInstance(instance,nullptr);
    setenv("DXVK_WSI_DRIVER","Android",1);setenv("DXVK_LOG_PATH","none",1);
    setenv("DXVK_STATE_CACHE","disable",1);setenv("DXVK_LOG_LEVEL","debug",1);
    void *d3d=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!d3d) { std::fprintf(stderr,"DXVK_LOAD_STOP %s\n",dlerror());return 78; }
    using Create=HRESULT(*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,
        ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
    auto create=reinterpret_cast<Create>(dlsym(d3d,"D3D11CreateDevice"));if(!create)return 79;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
    for(unsigned i=0;i<3;++i) {
        ID3D11Device *device=nullptr;ID3D11DeviceContext *context=nullptr;D3D_FEATURE_LEVEL selected={};
        HRESULT hr=create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&levels[i],1,D3D11_SDK_VERSION,&device,&selected,&context);
        std::printf("%s{\"requested\":%u,\"hresult\":%u,\"selected\":%u,\"device_created\":%s}",
            i?",":"",levels[i],static_cast<uint32_t>(hr),selected,SUCCEEDED(hr)&&device?"true":"false");
        std::fflush(stdout);if(context)context->Release();if(device)device->Release();
    }
    std::printf("],\"original_game_rendered\":false}\n");
    return 0;
}
