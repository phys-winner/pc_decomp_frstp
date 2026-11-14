#define VK_NO_PROTOTYPES
#include "fsr3_api.h"
#include "driver.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <vector>

// Same explicit provider selection as android/gpu/gpu_probe.c.
#define VULKAN_PROCS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceFeatures2) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) \
 X(MapMemory) X(UnmapMemory) X(CreateImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) \
 X(CreateSampler) X(CreateDescriptorSetLayout) X(CreatePipelineLayout) X(CreateShaderModule) X(CreateComputePipelines) \
 X(CreateDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(AllocateCommandBuffers) \
 X(BeginCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdClearColorImage) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(EndCommandBuffer) X(QueueSubmit) \
 X(QueueWaitIdle) X(ResetCommandBuffer) X(DestroyCommandPool) X(DestroyDescriptorPool) X(DestroyPipeline) \
 X(DestroyShaderModule) X(DestroyPipelineLayout) X(DestroyDescriptorSetLayout) X(DestroySampler) X(DestroyImageView) \
 X(DestroyImage) X(DestroyBuffer) X(FreeMemory)
#define DECLARE(name) static PFN_vk##name vk##name;
VULKAN_PROCS(DECLARE)
#undef DECLARE

static void check(VkResult status){if(status){std::fprintf(stderr,"Vulkan status=%d\n",status);std::abort();}}
struct Image {FpFsr3Image api{};VkDeviceMemory memory{};uint32_t bytes;};
static float pattern(float x,float y,unsigned salt){
    int ix=int(std::floor(x*.5f)),iy=int(std::floor(y*.5f));float ax=x*.5f-ix,ay=y*.5f-iy;
    auto hash=[&](int a,int b){uint32_t v=uint32_t(a)*0x9e3779b9u^uint32_t(b)*0x85ebca6bu^salt;v^=v>>16;v*=0x7feb352du;v^=v>>15;return float(v&255u);};
    return ((1-ax)*hash(ix,iy)+ax*hash(ix+1,iy))*(1-ay)+((1-ax)*hash(ix,iy+1)+ax*hash(ix+1,iy+1))*ay;
}
int main(int argc,char **argv){
    assert(argc==3||argc==5);void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library){std::fprintf(stderr,"%s\n",dlerror());return 2;}
    auto get=reinterpret_cast<const FpFsr3Api*(*)()>(dlsym(library,"frostpunk_fsr3_api"));assert(get);
    auto api=get();assert(api&&api->version==1&&api->bytes==sizeof(FpFsr3Api));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;
    char errorText[512]{};
    auto resolve=argc==5?frostpunk_gpu_open_turnip(argv[3],argv[4],errorText,sizeof(errorText))
                       :frostpunk_gpu_open_system(errorText,sizeof(errorText));
    if(!resolve){std::fprintf(stderr,"%s\n",errorText);return 2;}
    auto create=reinterpret_cast<PFN_vkCreateInstance>(resolve(nullptr,"vkCreateInstance"));assert(create);
    VkInstance instance;check(create(&ic,nullptr,&instance));
#define LOAD(name) vk##name=reinterpret_cast<PFN_vk##name>(resolve(instance,"vk" #name));assert(vk##name);
    VULKAN_PROCS(LOAD)
#undef LOAD
    uint32_t count=0;check(vkEnumeratePhysicalDevices(instance,&count,nullptr));assert(count);
    std::vector<VkPhysicalDevice> physicals(count);check(vkEnumeratePhysicalDevices(instance,&count,physicals.data()));auto physical=physicals[0];
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,&f13};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&f12};vkGetPhysicalDeviceFeatures2(physical,&features);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());uint32_t family=0;
    while(family<count&&!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;assert(family<count);
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&f12};dc.pEnabledFeatures=&features.features;
    dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;
    VkDevice device;check(vkCreateDevice(physical,&dc,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags flags){
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
        std::abort();
    };
    std::vector<Image> images;
    auto image=[&](uint32_t w,uint32_t h,VkFormat format,uint32_t bytes){
        Image result;result.api={VK_NULL_HANDLE,format,w,h};result.bytes=bytes;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={w,h,1};
        info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if(format==VK_FORMAT_R8G8B8A8_SRGB)info.usage&=~VK_IMAGE_USAGE_STORAGE_BIT;
        check(vkCreateImage(device,&info,nullptr,&result.api.image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,result.api.image,&req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device,&alloc,nullptr,&result.memory));check(vkBindImageMemory(device,result.api.image,result.memory,0));
        images.push_back(result);return result;
    };
    const uint32_t w=512,h=384,ow=1024,oh=768;
    Image color=image(w,h,VK_FORMAT_R8G8B8A8_UNORM,4),depth=image(w,h,VK_FORMAT_R32_SFLOAT,4);
    Image motion=image(w,h,VK_FORMAT_R16G16_SFLOAT,4),reactive=image(w,h,VK_FORMAT_R8_UNORM,1),output=image(ow,oh,VK_FORMAT_R16G16B16A16_SFLOAT,8);
    Image flow=image((w+7)/8,(h+7)/8,VK_FORMAT_R16G16_SINT,4),scd=image(3,1,VK_FORMAT_R32_UINT,4);
    Image opaque=image(w,h,VK_FORMAT_R8G8B8A8_UNORM,4);
    Image opaqueHalf=image(w/2,h/2,VK_FORMAT_R8G8B8A8_UNORM,4),opaqueDouble=image(w*2,h*2,VK_FORMAT_R8G8B8A8_UNORM,4);
    Image sceneSrgb=image(w,h,VK_FORMAT_R8G8B8A8_SRGB,4);
    Image dd=image(w,h,VK_FORMAT_R32_SFLOAT,4),dm=image(w,h,VK_FORMAT_R16G16_SFLOAT,4),pd=image(w,h,VK_FORMAT_R32_UINT,4);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=ow*oh*8;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging;check(vkCreateBuffer(device,&bi,nullptr,&staging));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,staging,&req);
    VkMemoryAllocateInfo ba{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ba.allocationSize=req.size;ba.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stagingMemory;check(vkAllocateMemory(device,&ba,nullptr,&stagingMemory));check(vkBindBufferMemory(device,staging,stagingMemory,0));
    void *mapped;check(vkMapMemory(device,stagingMemory,0,VK_WHOLE_SIZE,0,&mapped));
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;check(vkCreateCommandPool(device,&pc,nullptr,&pool));VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer command;check(vkAllocateCommandBuffers(device,&ca,&command));
    auto begin=[&](){check(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};cb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&cb));};
    auto submit=[&](){check(vkEndCommandBuffer(command));VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&command;check(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE));check(vkQueueWaitIdle(queue));};
    auto barrier=[&](const Image& image,VkImageLayout oldLayout,VkImageLayout newLayout){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=oldLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.oldLayout=oldLayout;b.newLayout=newLayout;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image.api.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    auto copy=[&](const Image& image,bool upload){
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={image.api.width,image.api.height,1};
        if(upload)vkCmdCopyBufferToImage(command,staging,image.api.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        else vkCmdCopyImageToBuffer(command,image.api.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging,1,&region);
    };
    begin();for(const auto& i:images){barrier(i,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdClearColorImage(command,i.api.image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);}submit();
    auto upload=[&](const Image& image,const void *data){std::memcpy(mapped,data,image.api.width*image.api.height*image.bytes);begin();barrier(image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);copy(image,true);barrier(image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);submit();};
    uint32_t error=0;assert(!api->create(device,physical,vkGetDeviceProcAddr,0,h,ow,oh,0,&error)&&error);
    void *state=api->create(device,physical,vkGetDeviceProcAddr,w,h,ow,oh,0,&error);
    std::fprintf(stderr,"SDK create status=%u\n",error);assert(state&&error==0);
    for(unsigned size:{256u,1024u,512u}){
        void *resized=api->create(device,physical,vkGetDeviceProcAddr,size,size*3/4,ow,oh,0,&error);
        std::fprintf(stderr,"Concurrent SDK create %u status=%u\n",size,error);assert(resized&&error==0);api->destroy(resized);
    }
    FpFsr3Frame frame{};frame.color=color.api;frame.depth=depth.api;frame.motion=motion.api;frame.reactive=reactive.api;frame.output=output.api;
    frame.flow=flow.api;frame.sceneChange=scd.api;frame.dilatedDepth=dd.api;frame.dilatedMotion=dm.api;frame.previousDepth=pd.api;
    frame.nearPlane=.1f;frame.farPlane=100;frame.verticalFov=1;frame.milliseconds=16.6667f;
    std::vector<float> depths(w*h,.5f);upload(depth,depths.data());
    std::vector<uint32_t> pixels(w*h);std::vector<uint16_t> motions(w*h*2,0);std::vector<int> measured;
    uint64_t checksum=0;unsigned finite=0;float previousJitterX=0,previousJitterY=0;double mse=0,minPsnr=100;
    for(unsigned n=0;n<12;++n){
        api->jitter(n,w,ow,&frame.jitterX,&frame.jitterY);assert(std::isfinite(frame.jitterX)&&std::fabs(frame.jitterX)<=.5f);
        frame.reset=n==0;
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            float sx=float(x)-float(n*4)-frame.jitterX,sy=float(y)-frame.jitterY;
            unsigned r=unsigned(pattern(sx,sy,1)),g=unsigned(pattern(sx,sy,2)),b=unsigned(pattern(sx,sy,3));
            pixels[y*w+x]=0xff000000u|b<<16|g<<8|r;
        }
        upload(color,pixels.data());begin();assert(api->flow(state,command,&frame)==0);barrier(flow,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);copy(flow,false);barrier(flow,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);submit();
        if(n>=3){auto vectors=static_cast<int16_t*>(mapped);for(unsigned y=2;y<flow.api.height-2;++y)for(unsigned x=3;x<flow.api.width-3;++x)measured.push_back(vectors[(y*flow.api.width+x)*2]);
            std::fprintf(stderr,"Flow frame=%u center=%d,%d\n",n,vectors[((flow.api.height/2)*flow.api.width+flow.api.width/2)*2],vectors[((flow.api.height/2)*flow.api.width+flow.api.width/2)*2+1]);}
        begin();barrier(scd,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);copy(scd,false);barrier(scd,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);submit();
        std::fprintf(stderr,"SCD frame=%u values=%u,%u,%u\n",n,static_cast<uint32_t*>(mapped)[0],static_cast<uint32_t*>(mapped)[1],static_cast<uint32_t*>(mapped)[2]);
        // Exact analytical motion of the translating plane, including raster jitter.
        for(unsigned i=0;i<w*h;++i){__fp16 dx=n?-4.f+previousJitterX-frame.jitterX:0.f,dy=n?previousJitterY-frame.jitterY:0.f;std::memcpy(&motions[i*2],&dx,2);std::memcpy(&motions[i*2+1],&dy,2);}
        previousJitterX=frame.jitterX;previousJitterY=frame.jitterY;
        upload(motion,motions.data());begin();assert(api->upscale(state,command,&frame)==0);
        barrier(output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);copy(output,false);barrier(output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);submit();
        auto data=static_cast<uint16_t*>(mapped);finite=0;checksum=0;
        for(unsigned i=0;i<ow*oh*4;++i){assert((data[i]&0x7c00u)!=0x7c00u);finite+=data[i]!=0;checksum=checksum*131+data[i];}
        assert(finite>ow*oh);
        mse=0;
        for(unsigned y=16;y<oh-16;++y)for(unsigned x=16;x<ow-16;++x)for(unsigned c=0;c<3;++c){
            __fp16 value;std::memcpy(&value,&data[(y*ow+x)*4+c],2);
            float expected=pattern((x+.5f)*w/ow-.5f-n*4,(y+.5f)*h/oh-.5f,c+1)/255.f;
            double diff=float(value)-expected;mse+=diff*diff;
        }
        mse/=double((ow-32)*(oh-32)*3);
        std::fprintf(stderr,"Upscale frame=%u PSNR=%.3f dB\n",n,-10*std::log10(mse));
        if(n>=3)minPsnr=std::min(minPsnr,-10*std::log10(mse));
    }
    std::sort(measured.begin(),measured.end());int median=measured[measured.size()/2];
    std::fprintf(stderr,"Optical flow translated +4px, range=%d..%d median=%d\n",measured.front(),measured.back(),median);assert(median>=-6&&median<=-2&&minPsnr>30);
    std::ifstream shaderFile(argv[2],std::ios::binary|std::ios::ate);assert(shaderFile);
    size_t shaderBytes=shaderFile.tellg();assert(shaderBytes&&shaderBytes%4==0);
    std::vector<uint32_t> code(shaderBytes/4);shaderFile.seekg(0);shaderFile.read(reinterpret_cast<char*>(code.data()),shaderBytes);assert(shaderFile);
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=shaderBytes;sm.pCode=code.data();
    VkShaderModule module;check(vkCreateShaderModule(device,&sm,nullptr,&module));
    const Image resources[]={depth,flow,color,opaque,depth,motion,reactive,output};
    VkDescriptorSetLayoutBinding bindings[8]{};VkImageView views[8]{};VkDescriptorImageInfo infos[8]{};VkWriteDescriptorSet writes[8]{};
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;check(vkCreateSampler(device,&si,nullptr,&sampler));
    for(unsigned i=0;i<8;++i){
        bindings[i]={i,i<4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=resources[i].api.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=resources[i].api.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(vkCreateImageView(device,&vi,nullptr,&views[i]));
        infos[i]={i<4?sampler:VK_NULL_HANDLE,views[i],VK_IMAGE_LAYOUT_GENERAL};
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=8;li.pBindings=bindings;
    VkDescriptorSetLayout layout;check(vkCreateDescriptorSetLayout(device,&li,nullptr,&layout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&layout;pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&range;
    VkPipelineLayout pipelineLayout;check(vkCreatePipelineLayout(device,&pi,nullptr,&pipelineLayout));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=pipelineLayout;
    ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
    VkPipeline pipeline;check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;
    VkDescriptorPool descriptorPool;check(vkCreateDescriptorPool(device,&dpi,nullptr,&descriptorPool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptorPool;da.descriptorSetCount=1;da.pSetLayouts=&layout;
    VkDescriptorSet set;check(vkAllocateDescriptorSets(device,&da,&set));
    for(unsigned i=0;i<8;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,i,0,1,bindings[i].descriptorType,&infos[i],nullptr,nullptr};}
    vkUpdateDescriptorSets(device,8,writes,0,nullptr);
    std::vector<uint16_t> sentinel(ow*oh*4);for(unsigned i=0;i<ow*oh;++i){sentinel[4*i]=0x3400;sentinel[4*i+1]=0x3800;sentinel[4*i+2]=0x3a00;sentinel[4*i+3]=0x3c00;}
    for(const Image& snapshotImage:{opaque,opaqueHalf,opaqueDouble}){
    vkDestroyImageView(device,views[3],nullptr);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=snapshotImage.api.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=snapshotImage.api.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(vkCreateImageView(device,&vi,nullptr,&views[3]));
    infos[3].imageView=views[3];vkUpdateDescriptorSets(device,1,writes+3,0,nullptr);
    for(unsigned n=0;n<9;++n){
        std::fill(pixels.begin(),pixels.end(),0xff204060u+n*0x00010101u);upload(sceneSrgb,pixels.data());
        if(snapshotImage.api.image!=opaque.api.image){
            std::vector<uint32_t> snapshotPixels(snapshotImage.api.width*snapshotImage.api.height,pixels[0]);upload(snapshotImage,snapshotPixels.data());
        }else{
        begin();barrier(sceneSrgb,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);barrier(opaque,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy snapshot{};snapshot.srcSubresource=snapshot.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};snapshot.extent={w,h,1};
        vkCmdCopyImage(command,sceneSrgb.api.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,opaque.api.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&snapshot);
        barrier(sceneSrgb,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);barrier(opaque,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);submit();
        }
        if(n)for(unsigned y=h/4;y<h*3/4;++y)for(unsigned x=w/4;x<w*3/4;++x)pixels[y*w+x]=n%2?0xffffffffu:0xff000000u;
        if(n)pixels.front()=pixels.back()=n%2?0xffffffffu:0xff000000u;
        upload(color,pixels.data());upload(output,sentinel.data());begin();
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
        uint32_t phase=1;vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&phase);
        vkCmdDispatch(command,(ow+7)/8,(oh+7)/8,1);
        barrier(output,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);copy(output,false);barrier(output,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);submit();
        auto values=static_cast<uint16_t*>(mapped);
        if(n)for(unsigned c=0;c<3;++c){assert(values[c]==(n%2?0x3c00:0));assert(values[(ow*oh-1)*4+c]==(n%2?0x3c00:0));}
        if(!n)assert(!std::memcmp(values,sentinel.data(),sentinel.size()*2));
        else for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x){
            unsigned i=(y*ow+x)*4;
            if(x>ow/4+4&&x<ow*3/4-4&&y>oh/4+4&&y<oh*3/4-4){for(unsigned c=0;c<3;++c)assert(values[i+c]==(n%2?0x3c00:0));}
            if(x>8&&x<ow/4-4)assert(!std::memcmp(values+i,sentinel.data()+i,8));
        }
    }
    }
    vkDestroyDescriptorPool(device,descriptorPool,nullptr);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
    vkDestroyDescriptorSetLayout(device,layout,nullptr);vkDestroyShaderModule(device,module,nullptr);vkDestroySampler(device,sampler,nullptr);
    for(auto view:views)vkDestroyImageView(device,view,nullptr);
    frame.nearPlane=0;assert(api->upscale(state,command,&frame)!=0);
    api->destroy(state);vkUnmapMemory(device,stagingMemory);vkDestroyBuffer(device,staging,nullptr);vkFreeMemory(device,stagingMemory,nullptr);
    for(const auto& i:images){vkDestroyImage(device,i.api.image,nullptr);vkFreeMemory(device,i.memory,nullptr);}
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);dlclose(library);
    std::printf("{\"success\":true,\"frames\":12,\"ui_composite_frames\":27,\"ui_input_extents\":3,\"ui_spatial_stable\":true,\"scene_preserved\":true,\"flow_median_x\":%d,\"min_psnr_db\":%.3f,\"output_nonzero_half_values\":%u,\"checksum\":\"%016llx\",\"invalid_inputs_rejected\":true}\n",median,minPsnr,finite,static_cast<unsigned long long>(checksum));
}
