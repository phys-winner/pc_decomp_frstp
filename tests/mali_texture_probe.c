/* Real system-Mali texelFetch/readback of one verified converted asset mip. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "mali_sample.spv.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Noreturn void fail(const char *stage, int result) {
    fprintf(stderr,"GPU_STOP stage=%s result=%d\n",stage,result); exit(70);
}
#define CHECK(expression) do { VkResult r=(expression); if(r!=VK_SUCCESS)fail(#expression,r); } while(0)
#define INSTANCE_PROCS(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_PROCS(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(GetBufferMemoryRequirements) \
 X(AllocateMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateImage) X(GetImageMemoryRequirements) \
 X(BindImageMemory) X(CreateImageView) X(CreateSampler) X(CreateDescriptorSetLayout) X(CreatePipelineLayout) \
 X(CreateShaderModule) X(CreateComputePipelines) X(CreateDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(EndCommandBuffer) X(CreateFence) X(QueueSubmit) X(WaitForFences) \
 X(DestroyFence) X(DestroyCommandPool) X(DestroyDescriptorPool) X(DestroyPipeline) X(DestroyShaderModule) \
 X(DestroyPipelineLayout) X(DestroyDescriptorSetLayout) X(DestroySampler) X(DestroyImageView) X(DestroyImage) \
 X(DestroyBuffer) X(FreeMemory)
#define DECLARE(name) static PFN_vk##name name;
INSTANCE_PROCS(DECLARE)
DEVICE_PROCS(DECLARE)
#undef DECLARE

static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory.memoryTypeCount;++i)
        if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
    fail("memory type unavailable",(int)flags);
}

static VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo info={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(req.memoryTypeBits,flags)};
    VkDeviceMemory allocation; CHECK(AllocateMemory(device,&info,NULL,&allocation)); return allocation;
}

static VkBuffer make_buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,VkDeviceMemory *allocation) {
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bytes,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer; CHECK(CreateBuffer(device,&info,NULL,&buffer));
    VkMemoryRequirements req; GetBufferMemoryRequirements(device,buffer,&req);
    *allocation=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(BindBufferMemory(device,buffer,*allocation,0)); return buffer;
}

static unsigned number(const char *text) {
    char *end=NULL;unsigned long value=strtoul(text,&end,10);
    if(!*text||*end||value>UINT32_MAX)fail("invalid numeric argument",0);
    return (unsigned)value;
}
static void read_span(const char *path,unsigned offset,void *data,size_t bytes) {
    FILE *file=fopen(path,"rb");if(!file)fail("open fixture input",0);
    if(fseek(file,0,SEEK_END)||ftell(file)<0||(uint64_t)offset+bytes>(uint64_t)ftell(file)||
       fseek(file,(long)offset,SEEK_SET)||fread(data,1,bytes,file)!=bytes)fail("fixture input bounds",0);
    fclose(file);
}
int main(int argc,char **argv) {
    if(argc!=9&&argc!=10) { fprintf(stderr,"Usage: mali_texture_probe format width height payload offset bytes reference offset [new-readback-file]\n");return 64; }
    unsigned format=number(argv[1]),width=number(argv[2]),height=number(argv[3]);
    unsigned offset=number(argv[5]),bytes=number(argv[6]),reference_offset=number(argv[8]);
    if(!width||!height||width>2048||height>2048)fail("fixture dimensions",0);
    unsigned block=0,block_bytes=16;
    if(format==44||format==50){block=1;block_bytes=4;}
    else if(format==157||format==158)block=4;
    else if(format==165||format==166)block=6;
    else fail("fixture Vulkan format",0);
    if(bytes!=((width+block-1)/block)*((height+block-1)/block)*block_bytes)fail("fixture block geometry",0);
    size_t pixel_count=(size_t)width*height,output_bytes=pixel_count*4*sizeof(uint32_t);
    void *loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);if(!loader)fail("system Vulkan loader",0);
    PFN_vkGetInstanceProcAddr get=(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr");
    if(!get)fail("system Vulkan entrypoint",0);
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    if(!create) fail("vkCreateInstance proc missing",0);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk native Mali converted texture proof",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance; CHECK(create(&instance_info,NULL,&instance));
#define LOAD_INSTANCE(name) name=(PFN_vk##name)get(instance,"vk"#name); if(!name)fail("vk"#name" missing",0);
    INSTANCE_PROCS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    VkPhysicalDevice physical[8]; uint32_t count=8; CHECK(EnumeratePhysicalDevices(instance,&count,physical));
    if(count!=1)fail("expected one physical device",(int)count);
    VkPhysicalDeviceProperties properties; GetPhysicalDeviceProperties(physical[0],&properties);
    VkPhysicalDeviceFeatures features; GetPhysicalDeviceFeatures(physical[0],&features);
    VkFormatProperties props;GetPhysicalDeviceFormatProperties(physical[0],(VkFormat)format,&props);
    if(properties.vendorID!=0x13b5||!strstr(properties.deviceName,"Mali-G615")||properties.apiVersion<VK_API_VERSION_1_3||
       (block>1&&!features.textureCompressionASTC_LDR)||!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        fail("Mali Vulkan format feature gate",0);
    if(strpbrk(properties.deviceName,"\"\\\n\r"))fail("device name encoding",0);
    GetPhysicalDeviceMemoryProperties(physical[0],&memory);
    VkQueueFamilyProperties families[32]; uint32_t families_count=32;
    GetPhysicalDeviceQueueFamilyProperties(physical[0],&families_count,families);
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families_count;++i) if(families[i].queueCount && (families[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) {family=i;break;}
    if(family==UINT32_MAX)fail("no compute queue",0);
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled={.textureCompressionASTC_LDR=block>1};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,.pEnabledFeatures=&enabled};
    CHECK(CreateDevice(physical[0],&device_info,NULL,&device));
#define LOAD_DEVICE(name) name=(PFN_vk##name)GetDeviceProcAddr(device,"vk"#name); if(!name)fail("vk"#name" missing",0);
    DEVICE_PROCS(LOAD_DEVICE)
#undef LOAD_DEVICE
    VkQueue queue; GetDeviceQueue(device,family,0,&queue);
    VkDeviceMemory upload_memory,output_memory;
    VkBuffer upload=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&upload_memory);
    VkBuffer output=make_buffer(output_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&output_memory);
    void *mapped;CHECK(MapMemory(device,upload_memory,0,VK_WHOLE_SIZE,0,&mapped));read_span(argv[4],offset,mapped,bytes);UnmapMemory(device,upload_memory);
    CHECK(MapMemory(device,output_memory,0,VK_WHOLE_SIZE,0,&mapped));memset(mapped,0xa5,output_bytes);UnmapMemory(device,output_memory);
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=(VkFormat)format,
        .extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image; CHECK(CreateImage(device,&image_info,NULL,&image));
    VkMemoryRequirements req; GetImageMemoryRequirements(device,image,&req);
    VkDeviceMemory image_memory=allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CHECK(BindImageMemory(device,image,image_memory,0));
    VkImageSubresourceRange range={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.baseMipLevel=0,.levelCount=1,.baseArrayLayer=0,.layerCount=1};
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=(VkFormat)format,.subresourceRange=range};
    VkImageView view; CHECK(CreateImageView(device,&view_info,NULL,&view));
    VkSamplerCreateInfo sampler_info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    VkSampler sampler; CHECK(CreateSampler(device,&sampler_info,NULL,&sampler));
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout layout; CHECK(CreateDescriptorSetLayout(device,&layout_info,NULL,&layout));
    VkPushConstantRange push_range={.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT,.size=8};
    VkPipelineLayoutCreateInfo pipeline_layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push_range};
    VkPipelineLayout pipeline_layout; CHECK(CreatePipelineLayout(device,&pipeline_layout_info,NULL,&pipeline_layout));
    VkShaderModuleCreateInfo shader_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(mali_sample_spv),.pCode=mali_sample_spv};
    VkShaderModule shader; CHECK(CreateShaderModule(device,&shader_info,NULL,&shader));
    VkComputePipelineCreateInfo pipeline_info={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=pipeline_layout};
    VkPipeline pipeline; CHECK(CreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,NULL,&pipeline));
    VkDescriptorPoolSize pool_sizes[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=2,.pPoolSizes=pool_sizes};
    VkDescriptorPool pool; CHECK(CreateDescriptorPool(device,&pool_info,NULL,&pool));
    VkDescriptorSetAllocateInfo set_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set; CHECK(AllocateDescriptorSets(device,&set_info,&set));
    VkDescriptorImageInfo descriptor_image={sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo descriptor_buffer={output,0,output_bytes};
    VkWriteDescriptorSet writes[2]={{.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&descriptor_image},
        {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=1,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&descriptor_buffer}};
    UpdateDescriptorSets(device,2,writes,0,NULL);
    VkCommandPoolCreateInfo command_pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool command_pool; CHECK(CreateCommandPool(device,&command_pool_info,NULL,&command_pool));
    VkCommandBufferAllocateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command; CHECK(AllocateCommandBuffers(device,&command_info,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; CHECK(BeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier to_copy={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange=range};
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&to_copy);
    VkBufferImageCopy copy={.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.mipLevel=0,.baseArrayLayer=0,.layerCount=1},.imageExtent={width,height,1}};
    CmdCopyBufferToImage(command,upload,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    VkImageMemoryBarrier to_sample=to_copy; to_sample.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; to_sample.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    to_sample.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_sample.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&to_sample);
    CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL);
    uint32_t dimensions[2]={width,height};CmdPushConstants(command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,dimensions);
    CmdDispatch(command,(width+7)/8,(height+7)/8,1);
    VkBufferMemoryBarrier to_host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=output,.offset=0,.size=VK_WHOLE_SIZE};
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&to_host,0,NULL); CHECK(EndCommandBuffer(command));
    VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(CreateFence(device,&fence_info,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command}; CHECK(QueueSubmit(queue,1,&submit,fence));
    CHECK(WaitForFences(device,1,&fence,VK_TRUE,UINT64_C(5000000000)));
    CHECK(MapMemory(device,output_memory,0,VK_WHOLE_SIZE,0,&mapped));
    unsigned char *reference=malloc(pixel_count*4);if(!reference)fail("reference allocation",0);
    read_span(argv[7],reference_offset,reference,pixel_count*4);
    unsigned mismatches=0,max_delta[4]={0};uint32_t *pixels=mapped;
    for(size_t i=0;i<pixel_count*4;++i) {
        unsigned actual=pixels[i],expected=reference[i],delta=actual>expected?actual-expected:expected-actual;
        if(delta>max_delta[i%4])max_delta[i%4]=delta;
        if(delta>2)++mismatches;
    }
    if(argc==10) {
        FILE *dump=fopen(argv[9],"wbx");if(!dump)fail("exclusive readback output",0);
        for(size_t i=0;i<pixel_count*4;++i){if(pixels[i]>255)fail("readback channel range",0);reference[i]=(unsigned char)pixels[i];}
        if(fwrite(reference,1,pixel_count*4,dump)!=pixel_count*4||fclose(dump))fail("readback output write",0);
    }
    free(reference);UnmapMemory(device,output_memory);
    printf("{\"success\":%s,\"device\":\"%s\",\"vk_format\":%u,\"width\":%u,\"height\":%u,\"pixels_checked\":%zu,\"mismatches_gt2\":%u,\"max_rgba_delta\":[%u,%u,%u,%u],\"gpu_texelFetch\":true,\"game_rendered\":false}\n",
        mismatches?"false":"true",properties.deviceName,format,width,height,pixel_count,mismatches,max_delta[0],max_delta[1],max_delta[2],max_delta[3]);
    DestroyFence(device,fence,NULL); DestroyCommandPool(device,command_pool,NULL); DestroyDescriptorPool(device,pool,NULL);
    DestroyPipeline(device,pipeline,NULL); DestroyShaderModule(device,shader,NULL); DestroyPipelineLayout(device,pipeline_layout,NULL);
    DestroyDescriptorSetLayout(device,layout,NULL); DestroySampler(device,sampler,NULL); DestroyImageView(device,view,NULL);
    DestroyImage(device,image,NULL); FreeMemory(device,image_memory,NULL); DestroyBuffer(device,upload,NULL); FreeMemory(device,upload_memory,NULL);
    DestroyBuffer(device,output,NULL); FreeMemory(device,output_memory,NULL); DestroyDevice(device,NULL); DestroyInstance(instance,NULL);
    return mismatches?1:0;
}
