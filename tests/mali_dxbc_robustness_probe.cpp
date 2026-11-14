/* Reuse the already hardware-tested allocator/cleanup, without changing its proof. */
#define main minimal_robustness_fixture_main
#include "mali_robustness_probe.cpp"
#undef main
#include "compiler_proof.h"

struct CompilerProbe:Probe {
  Buffer additional[2];VkBufferView texelView=VK_NULL_HANDLE;
  ~CompilerProbe() {
    if(device) {
      if(texelView)vkDestroyBufferView(device,texelView,nullptr);
      for(auto &b:additional) {
        if(b.mapped)vkUnmapMemory(device,b.memory);
        if(b.buffer)vkDestroyBuffer(device,b.buffer,nullptr);
        if(b.memory)vkFreeMemory(device,b.memory,nullptr);
      }
    }
  }
  void execute() {
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Frostpunk actual DXBC robustness proof";app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;VK_CHECK(vkCreateInstance(&ci,nullptr,&instance));
    uint32_t count=0;VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));if(count!=1)throw std::runtime_error("expected one GPU");
    VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    VkPhysicalDeviceProperties props={};vkGetPhysicalDeviceProperties(physical,&props);
    if(props.vendorID!=0x13b5||!std::strstr(props.deviceName,"Mali-G615"))throw std::runtime_error("expected native Mali-G615");
    VkPhysicalDeviceVulkan12Features supported={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&supported};vkGetPhysicalDeviceFeatures2(physical,&features);
    if(!supported.descriptorBindingPartiallyBound)throw std::runtime_error("partial binding absent");
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);if(count>64)throw std::runtime_error("too many queues");
    VkQueueFamilyProperties queues[64]={};vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues);
    bool found=false;for(uint32_t i=0;i<count;i++)if(queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT){family=i;found=true;break;}
    if(!found)throw std::runtime_error("no compute queue");
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan12Features enabled={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};enabled.descriptorBindingPartiallyBound=VK_TRUE;
    VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&enabled};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
    VK_CHECK(vkCreateDevice(physical,&di,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
    makeBuffer(buffers[0],9728,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    makeBuffer(buffers[1],512,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    makeBuffer(buffers[2],64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    makeBuffer(additional[0],64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    makeBuffer(additional[1],64,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Buffer *resources[]={&buffers[0],&buffers[1],&buffers[2],&additional[0],&additional[1]};
    std::memset(buffers[0].mapped,0,9728);auto meta=static_cast<uint32_t*>(buffers[0].mapped);
    auto output=static_cast<uint32_t*>(buffers[1].mapped);for(unsigned i=0;i<128;i++)output[i]=0xfeedface;
    std::memcpy(buffers[2].mapped,initial_input,sizeof(initial_input));std::memcpy(additional[0].mapped,initial_writable,sizeof(initial_writable));
    std::memcpy(additional[1].mapped,initial_ubo,sizeof(initial_ubo));
    VkBufferViewCreateInfo bvi={VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};bvi.buffer=buffers[2].buffer;bvi.format=VK_FORMAT_R32_UINT;bvi.range=12;
    VK_CHECK(vkCreateBufferView(device,&bvi,nullptr,&texelView));
    VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R32G32B32A32_UINT;
    ii.extent={2,2,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;VK_CHECK(vkCreateImage(device,&ii,nullptr,&image));
    VkMemoryRequirements req={};vkGetImageMemoryRequirements(device,image,&req);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,0);
    VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&imageMemory));VK_CHECK(vkBindImageMemory(device,image,imageMemory,0));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
    constexpr unsigned bindingCount=sizeof(proof_bindings)/sizeof(proof_bindings[0]);
    VkDescriptorSetLayoutBinding bindings[bindingCount]={};VkDescriptorBindingFlags flags[bindingCount]={};
    for(unsigned i=0;i<bindingCount;i++){bindings[i]={proof_bindings[i].slot,proof_bindings[i].type,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};flags[i]=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;}
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};fi.bindingCount=bindingCount;fi.pBindingFlags=flags;
    VkDescriptorSetLayoutCreateInfo li={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,&fi};li.bindingCount=bindingCount;li.pBindings=bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(device,&li,nullptr,&setLayout));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,9},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,2}};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=4;pi.pPoolSizes=sizes;VK_CHECK(vkCreateDescriptorPool(device,&pi,nullptr,&descriptorPool));
    VkDescriptorSetAllocateInfo sa={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=descriptorPool;sa.descriptorSetCount=1;sa.pSetLayouts=&setLayout;
    VkDescriptorSet set;VK_CHECK(vkAllocateDescriptorSets(device,&sa,&set));
    VkWriteDescriptorSet writes[bindingCount]={};VkDescriptorBufferInfo bufferInfos[bindingCount]={};VkDescriptorImageInfo imageInfo={VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    unsigned writeCount=0,unwritten=0;
    for(unsigned i=0;i<bindingCount;i++) {
      const auto &b=proof_bindings[i];if(!b.bound){unwritten++;continue;}
      if(b.slot<1216){meta[b.slot*2]=1;meta[b.slot*2+1]=uint32_t(b.bytes);}
      auto &w=writes[writeCount];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.dstBinding=b.slot;w.descriptorCount=1;w.descriptorType=b.type;
      if(b.resource<5){bufferInfos[writeCount]={resources[b.resource]->buffer,0,b.bytes};w.pBufferInfo=&bufferInfos[writeCount];}
      else if(b.resource==5)w.pImageInfo=&imageInfo;
      else if(b.resource==6)w.pTexelBufferView=&texelView;
      else throw std::runtime_error("invalid populated role");
      writeCount++;
    }
    vkUpdateDescriptorSets(device,writeCount,writes,0,nullptr);
    VkPipelineLayoutCreateInfo pli={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pli.setLayoutCount=1;pli.pSetLayouts=&setLayout;
    VK_CHECK(vkCreatePipelineLayout(device,&pli,nullptr,&pipelineLayout));
    VkShaderModuleCreateInfo sm={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(compiler_shader_words);sm.pCode=compiler_shader_words;
    VK_CHECK(vkCreateShaderModule(device,&sm,nullptr,&shader));
    VkComputePipelineCreateInfo pc={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pc.layout=pipelineLayout;
    pc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,nullptr,&pipeline));
    VkCommandPoolCreateInfo cp={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=family;VK_CHECK(vkCreateCommandPool(device,&cp,nullptr,&commandPool));
    VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.commandBufferCount=1;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd;VK_CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkImageMemoryBarrier ib={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.image=image;ib.subresourceRange=vi.subresourceRange;ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;ib.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);
    VkClearColorValue clear={};for(unsigned i=0;i<4;i++)clear.uint32[i]=9+i;vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&vi.subresourceRange);
    VkMemoryBarrier mb={VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);vkCmdDispatch(cmd,1,1,1);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);VK_CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fc={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK_CHECK(vkCreateFence(device,&fc,nullptr,&fence));
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));
    VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_C(10000000000)));
    unsigned checks=0;
    auto expect=[&](uint32_t actual,uint32_t expected,unsigned index){checks++;if(actual!=expected){
      std::fprintf(stderr,"MALI_DXBC_ROBUSTNESS_MISMATCH index=%u actual=%08x expected=%08x\n",index,actual,expected);throw std::runtime_error("DXBC output mismatch");}};
    for(unsigned i=0;i<128;i++)expect(output[i],expected_output[i],i);
    for(unsigned i=0;i<16;i++) {
      expect(static_cast<uint32_t*>(buffers[2].mapped)[i],initial_input[i],1000+i);
      expect(static_cast<uint32_t*>(additional[0].mapped)[i],expected_writable[i],2000+i);
      expect(static_cast<uint32_t*>(additional[1].mapped)[i],initial_ubo[i],3000+i);
    }
    std::printf("{\"success\":true,\"device\":\"%s\",\"checks\":%u,\"actual_dxbc_compiler\":true,"
      "\"output_vectors\":25,\"canary_words\":28,\"unwritten_descriptors\":%u,\"partially_bound\":true,"
      "\"robustness2\":false,\"robust_buffer_access\":false,\"metadata_abi\":1}\n",props.deviceName,checks,unwritten);
  }
};
int main(){try{CompilerProbe p;p.execute();return 0;}catch(const std::exception &e){std::fprintf(stderr,"MALI_DXBC_ROBUSTNESS_STOP %s\n",e.what());return 73;}}
