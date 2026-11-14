/* Native Vulkan proof. Unbound descriptors are never written, including null handles. */
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "mali_robustness_spv.h"

static void check(VkResult result,const char *operation) {
  if(result!=VK_SUCCESS) {
    std::fprintf(stderr,"MALI_ROBUSTNESS_STOP %s VkResult=%d\n",operation,int(result));
    throw std::runtime_error(operation);
  }
}
#define VK_CHECK(call) check((call),#call)
struct Buffer { VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void *mapped=nullptr; };
struct Probe {
  VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;
  VkQueue queue=VK_NULL_HANDLE;uint32_t family=0;
  Buffer buffers[3];VkImage image=VK_NULL_HANDLE;VkDeviceMemory imageMemory=VK_NULL_HANDLE;
  VkImageView view=VK_NULL_HANDLE;VkSampler sampler=VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkShaderModule shader=VK_NULL_HANDLE;
  VkPipeline pipeline=VK_NULL_HANDLE;VkCommandPool commandPool=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
  ~Probe() {
    if(device) {
      if(fence)vkDestroyFence(device,fence,nullptr);
      if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);
      if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
      if(shader)vkDestroyShaderModule(device,shader,nullptr);
      if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
      if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
      if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
      if(sampler)vkDestroySampler(device,sampler,nullptr);
      if(view)vkDestroyImageView(device,view,nullptr);
      if(image)vkDestroyImage(device,image,nullptr);
      if(imageMemory)vkFreeMemory(device,imageMemory,nullptr);
      for(auto &b:buffers) {
        if(b.mapped)vkUnmapMemory(device,b.memory);
        if(b.buffer)vkDestroyBuffer(device,b.buffer,nullptr);
        if(b.memory)vkFreeMemory(device,b.memory,nullptr);
      }
      vkDestroyDevice(device,nullptr);
    }
    if(instance)vkDestroyInstance(instance,nullptr);
  }
  uint32_t memoryType(uint32_t mask,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props={};vkGetPhysicalDeviceMemoryProperties(physical,&props);
    for(uint32_t i=0;i<props.memoryTypeCount;i++)
      if((mask&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags)return i;
    throw std::runtime_error("required memory type absent");
  }
  void makeBuffer(Buffer &b,VkDeviceSize size,VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=usage;
    VK_CHECK(vkCreateBuffer(device,&ci,nullptr,&b.buffer));
    VkMemoryRequirements req={};vkGetBufferMemoryRequirements(device,b.buffer,&req);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;
    ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&b.memory));
    VK_CHECK(vkBindBufferMemory(device,b.buffer,b.memory,0));
    VK_CHECK(vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped));
    std::memset(b.mapped,0xcd,size);
  }
  void run() {
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Frostpunk bounded descriptor proof";
    app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;
    VK_CHECK(vkCreateInstance(&ci,nullptr,&instance));
    uint32_t count=0;VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
    if(count!=1)throw std::runtime_error("expected one physical GPU");
    VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    VkPhysicalDeviceProperties props={};vkGetPhysicalDeviceProperties(physical,&props);
    if(props.vendorID!=0x13b5 || !std::strstr(props.deviceName,"Mali-G615"))
      throw std::runtime_error("expected native Mali-G615");
    VkPhysicalDeviceVulkan12Features supported={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&supported};
    vkGetPhysicalDeviceFeatures2(physical,&features);
    if(!supported.descriptorBindingPartiallyBound)throw std::runtime_error("partial binding unavailable");
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    if(count>64)throw std::runtime_error("too many queue families");
    VkQueueFamilyProperties families[64]={};vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    bool found=false;
    for(uint32_t i=0;i<count;i++)if(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=i;found=true;break;}
    if(!found)throw std::runtime_error("no compute queue");
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan12Features enabled={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled.descriptorBindingPartiallyBound=VK_TRUE;
    VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&enabled};
    di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    // No extension list and no core robustBufferAccess: guards carry all tested semantics.
    VK_CHECK(vkCreateDevice(physical,&di,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
    makeBuffer(buffers[0],1216*8,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    makeBuffer(buffers[1],16384,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    makeBuffer(buffers[2],64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto meta=static_cast<uint32_t*>(buffers[0].mapped);std::memset(meta,0,1216*8);
    meta[2*2]=1;meta[2*2+1]=12;meta[5*2]=1;meta[7*2]=1;
    auto input=static_cast<uint32_t*>(buffers[2].mapped);
    input[0]=0x11223344;input[1]=0x55667788;input[2]=0x99aabbcc;
    VkFormatProperties format={};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32_UINT,&format);
    const auto required=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((format.optimalTilingFeatures&required)!=required)throw std::runtime_error("R32_UINT image operations unavailable");
    VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;
    ii.format=VK_FORMAT_R32_UINT;ii.extent={2,2,1};ii.mipLevels=1;ii.arrayLayers=1;
    ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateImage(device,&ii,nullptr,&image));
    VkMemoryRequirements req={};vkGetImageMemoryRequirements(device,image,&req);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;
    ai.memoryTypeIndex=memoryType(req.memoryTypeBits,0);
    VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&imageMemory));VK_CHECK(vkBindImageMemory(device,image,imageMemory,0));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=VK_FORMAT_R32_UINT;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
    VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device,&si,nullptr,&sampler));
    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding bindings[9]={};VkDescriptorBindingFlags bindingFlags[9]={};
    for(uint32_t i=0;i<9;i++) {
      bindings[i]={i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
      bindingFlags[i]=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagInfo.bindingCount=9;flagInfo.pBindingFlags=bindingFlags;
    VkDescriptorSetLayoutCreateInfo li={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,&flagInfo};li.bindingCount=9;li.pBindings=bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device,&li,nullptr,&setLayout));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=3;pi.pPoolSizes=sizes;
    VK_CHECK(vkCreateDescriptorPool(device,&pi,nullptr,&descriptorPool));
    VkDescriptorSetAllocateInfo sa={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=descriptorPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
    VkDescriptorSet set;VK_CHECK(vkAllocateDescriptorSets(device,&sa,&set));
    VkDescriptorBufferInfo bi[]={{buffers[0].buffer,0,1216*8},{buffers[1].buffer,0,16384},{buffers[2].buffer,0,12}};
    VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL},{sampler,view,VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]={};
    for(uint32_t i=0;i<5;i++) {
      writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].descriptorCount=1;
      writes[i].dstBinding=i<3?i:(i==3?5:7);writes[i].descriptorType=types[writes[i].dstBinding];
      if(i<3)writes[i].pBufferInfo=&bi[i];else writes[i].pImageInfo=&images[i-3];
    }
    vkUpdateDescriptorSets(device,5,writes,0,nullptr);
    VkPipelineLayoutCreateInfo pli={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;
    VK_CHECK(vkCreatePipelineLayout(device,&pli,nullptr,&pipelineLayout));
    VkShaderModuleCreateInfo sci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sci.codeSize=sizeof(shader_words);sci.pCode=shader_words;
    VK_CHECK(vkCreateShaderModule(device,&sci,nullptr,&shader));
    VkComputePipelineCreateInfo pci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.layout=pipelineLayout;
    pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
    VkCommandPoolCreateInfo cp={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;
    VK_CHECK(vkCreateCommandPool(device,&cp,nullptr,&commandPool));
    VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;VK_CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkImageMemoryBarrier ib={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.image=image;ib.subresourceRange=vi.subresourceRange;
    ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    VkClearColorValue clear={};for(auto &value:clear.uint32)value=77;
    vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&vi.subresourceRange);
    VkMemoryBarrier mb={VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);vkCmdDispatch(cmd,1,1,1);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    VkBufferImageCopy copy={};copy.bufferOffset=8192;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={2,2,1};
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_GENERAL,buffers[1].buffer,1,&copy);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK_CHECK(vkCreateFence(device,&fi,nullptr,&fence));
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
    VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(10000000000)));
    auto output=static_cast<uint32_t*>(buffers[1].mapped);
    const uint32_t initial[]={0x11223344,0x55667788,0x99aabbcc};
    unsigned checks=0;
    auto expect=[&](uint32_t actual,uint32_t expected,unsigned where) {
      ++checks;if(actual!=expected) {
        std::fprintf(stderr,"MALI_ROBUSTNESS_MISMATCH index=%u actual=%08x expected=%08x\n",where,actual,expected);
        throw std::runtime_error("readback mismatch");
      }
    };
    for(unsigned i=0;i<64;i++)for(unsigned c=0;c<16;c++) {
      uint32_t expected=0;
      if(c==0)expected=i<3?initial[i]:0;
      else if(c<=4)expected=i+c-1<3?initial[i+c-1]:0;
      else if(c==11||c==13)expected=(i==1||i==2)?77:0;
      else if(c==14)expected=0x51afe000+i;
      expect(output[i*16+c],expected,i*16+c);
    }
    expect(output[1024],initial[0],1024);expect(output[1025],77,1025);
    for(unsigned i=0;i<16;i++)expect(input[i],i<3?initial[i]+(i==0?7:0):0xcdcdcdcd,2000+i);
    for(unsigned i=0;i<4;i++)expect(output[2048+i],i==0?82:77,3000+i);
    std::printf("{\"success\":true,\"device\":\"%s\",\"checks\":%u,\"unwritten_descriptors\":4,"
      "\"partially_bound\":true,\"robust_buffer_access\":false,\"robustness2\":false,"
      "\"descriptor_bytes\":12,\"allocation_bytes\":64,\"cross_boundary_vectors\":64,"
      "\"unbound_and_oob_zero_noop\":true,\"positive_atomic_paths\":2}\n",props.deviceName,checks);
  }
};
int main() {
  try { Probe probe;probe.run();return 0; }
  catch(const std::exception &e) {std::fprintf(stderr,"MALI_ROBUSTNESS_STOP %s\n",e.what());return 73;}
}
