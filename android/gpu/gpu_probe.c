/* Actual BC3 texture sampling by a GPU compute shader and host readback.
 * All Vulkan entrypoints come from our pinned native adrenotools provider. */
#define VK_NO_PROTOTYPES
#include "driver.h"
#include "bc3_sample.spv.h"
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
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(EndCommandBuffer) X(CreateFence) X(QueueSubmit) X(WaitForFences) \
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

int main(int argc,char **argv) {
    if(argc!=3) { fprintf(stderr,"Usage: frostpunk_gpu_probe <runtime-dir> <driver-dir/>\n"); return 64; }
    char error[512]={0};
    PFN_vkGetInstanceProcAddr get=frostpunk_gpu_open(argv[1],argv[2],error,sizeof(error));
    if(!get) { fprintf(stderr,"GPU_STOP %s\n",error); return 70; }
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    if(!create) fail("vkCreateInstance proc missing",0);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Frostpunk native BC3 sampling proof",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance; CHECK(create(&instance_info,NULL,&instance));
#define LOAD_INSTANCE(name) name=(PFN_vk##name)get(instance,"vk"#name); if(!name)fail("vk"#name" missing",0);
    INSTANCE_PROCS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    VkPhysicalDevice physical[8]; uint32_t count=8; CHECK(EnumeratePhysicalDevices(instance,&count,physical));
    if(count!=1)fail("expected one physical device",(int)count);
    VkPhysicalDeviceProperties properties; GetPhysicalDeviceProperties(physical[0],&properties);
    VkPhysicalDeviceFeatures features; GetPhysicalDeviceFeatures(physical[0],&features);
    VkFormatProperties bc3; GetPhysicalDeviceFormatProperties(physical[0],VK_FORMAT_BC3_UNORM_BLOCK,&bc3);
    if(!strstr(properties.deviceName,"Turnip") || properties.apiVersion<VK_API_VERSION_1_3 ||
       !features.textureCompressionBC || !(bc3.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        fail("Turnip Vulkan1.3 BC3 feature gate",0);
    if(strpbrk(properties.deviceName,"\"\\\n\r"))fail("device name encoding",0);
    GetPhysicalDeviceMemoryProperties(physical[0],&memory);
    VkQueueFamilyProperties families[32]; uint32_t families_count=32;
    GetPhysicalDeviceQueueFamilyProperties(physical[0],&families_count,families);
    uint32_t family=UINT32_MAX;
    for(uint32_t i=0;i<families_count;++i) if(families[i].queueCount && (families[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) {family=i;break;}
    if(family==UINT32_MAX)fail("no compute queue",0);
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceFeatures enabled={.textureCompressionBC=VK_TRUE};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,.pEnabledFeatures=&enabled};
    CHECK(CreateDevice(physical[0],&device_info,NULL,&device));
#define LOAD_DEVICE(name) name=(PFN_vk##name)GetDeviceProcAddr(device,"vk"#name); if(!name)fail("vk"#name" missing",0);
    DEVICE_PROCS(LOAD_DEVICE)
#undef LOAD_DEVICE
    VkQueue queue; GetDeviceQueue(device,family,0,&queue);
    VkDeviceMemory upload_memory,output_memory;
    VkBuffer upload=make_buffer(16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,&upload_memory);
    VkBuffer output=make_buffer(16*4*sizeof(uint32_t),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,&output_memory);
    /* One DXT5/BC3 block: first two rows opaque red, final two rows blue with
     * alpha85. Alpha indices select endpoint0 for pixels0..7 and endpoint1
     * for8..15; colour indices likewise select red/blue endpoints. */
    const unsigned char block[16]={255,85,0,0,0,0x49,0x92,0x24,0x00,0xf8,0x1f,0x00,0x00,0x00,0x55,0x55};
    void *mapped; CHECK(MapMemory(device,upload_memory,0,VK_WHOLE_SIZE,0,&mapped)); memcpy(mapped,block,16); UnmapMemory(device,upload_memory);
    CHECK(MapMemory(device,output_memory,0,VK_WHOLE_SIZE,0,&mapped)); memset(mapped,0xa5,16*4*sizeof(uint32_t)); UnmapMemory(device,output_memory);
    VkImageCreateInfo image_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_BC3_UNORM_BLOCK,
        .extent={4,4,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image; CHECK(CreateImage(device,&image_info,NULL,&image));
    VkMemoryRequirements req; GetImageMemoryRequirements(device,image,&req);
    VkDeviceMemory image_memory=allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); CHECK(BindImageMemory(device,image,image_memory,0));
    VkImageSubresourceRange range={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.baseMipLevel=0,.levelCount=1,.baseArrayLayer=0,.layerCount=1};
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=VK_FORMAT_BC3_UNORM_BLOCK,.subresourceRange=range};
    VkImageView view; CHECK(CreateImageView(device,&view_info,NULL,&view));
    VkSamplerCreateInfo sampler_info={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0};
    VkSampler sampler; CHECK(CreateSampler(device,&sampler_info,NULL,&sampler));
    VkDescriptorSetLayoutBinding bindings[2]={{.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    VkDescriptorSetLayout layout; CHECK(CreateDescriptorSetLayout(device,&layout_info,NULL,&layout));
    VkPipelineLayoutCreateInfo pipeline_layout_info={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&layout};
    VkPipelineLayout pipeline_layout; CHECK(CreatePipelineLayout(device,&pipeline_layout_info,NULL,&pipeline_layout));
    VkShaderModuleCreateInfo shader_info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(bc3_sample_spv),.pCode=bc3_sample_spv};
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
    VkDescriptorBufferInfo descriptor_buffer={output,0,16*4*sizeof(uint32_t)};
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
    VkBufferImageCopy copy={.imageSubresource={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.mipLevel=0,.baseArrayLayer=0,.layerCount=1},.imageExtent={4,4,1}};
    CmdCopyBufferToImage(command,upload,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    VkImageMemoryBarrier to_sample=to_copy; to_sample.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; to_sample.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    to_sample.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; to_sample.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,NULL,0,NULL,1,&to_sample);
    CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,NULL); CmdDispatch(command,1,1,1);
    VkBufferMemoryBarrier to_host={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=output,.offset=0,.size=VK_WHOLE_SIZE};
    CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&to_host,0,NULL); CHECK(EndCommandBuffer(command));
    VkFenceCreateInfo fence_info={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(CreateFence(device,&fence_info,NULL,&fence));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command}; CHECK(QueueSubmit(queue,1,&submit,fence));
    CHECK(WaitForFences(device,1,&fence,VK_TRUE,UINT64_C(5000000000)));
    CHECK(MapMemory(device,output_memory,0,VK_WHOLE_SIZE,0,&mapped));
    uint32_t pixels[16][4]; memcpy(pixels,mapped,sizeof(pixels)); UnmapMemory(device,output_memory);
    unsigned mismatches=0;
    for(unsigned i=0;i<16;++i) {
        uint32_t expected[4]={i<8?255:0,0,i<8?0:255,i<8?255:85};
        for(unsigned c=0;c<4;++c) if(pixels[i][c]!=expected[c]) ++mismatches;
    }
    printf("{\"success\":%s,\"device\":\"%s\",\"api_version\":\"%u.%u.%u\",\"independent_adrenotools\":true,"
        "\"vk_device_created\":true,\"queue_submitted\":true,\"bc3_sampled_by_compute_shader\":true,\"pixels_checked\":16,\"components_checked\":64,"
        "\"mismatches\":%u,\"pixels\":[",mismatches?"false":"true",properties.deviceName,VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),mismatches);
    for(unsigned i=0;i<16;++i)printf("%s[%u,%u,%u,%u]",i?",":"",pixels[i][0],pixels[i][1],pixels[i][2],pixels[i][3]);
    printf("],\"original_game_rendered\":false,\"fps_measured\":false}\n");
    DestroyFence(device,fence,NULL); DestroyCommandPool(device,command_pool,NULL); DestroyDescriptorPool(device,pool,NULL);
    DestroyPipeline(device,pipeline,NULL); DestroyShaderModule(device,shader,NULL); DestroyPipelineLayout(device,pipeline_layout,NULL);
    DestroyDescriptorSetLayout(device,layout,NULL); DestroySampler(device,sampler,NULL); DestroyImageView(device,view,NULL);
    DestroyImage(device,image,NULL); FreeMemory(device,image_memory,NULL); DestroyBuffer(device,upload,NULL); FreeMemory(device,upload_memory,NULL);
    DestroyBuffer(device,output,NULL); FreeMemory(device,output_memory,NULL); DestroyDevice(device,NULL); DestroyInstance(instance,NULL);
    return mismatches?1:0;
}
