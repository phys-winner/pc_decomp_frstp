/* Independent native graphics-cache proof; no DXVK or game behavior is changed.
 * Reuse the hardware-tested Vulkan buffer allocator and ownership cleanup. */
#define main previous_robustness_main
#include "mali_robustness_probe.cpp"
#undef main
#include "pipeline_cache_shaders.h"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

static constexpr size_t HeaderBytes=88,MaxPayload=32*1024*1024;
static uint32_t word(const uint8_t *p){uint32_t v;std::memcpy(&v,p,4);return v;}
static void put(std::vector<uint8_t>&b,size_t at,uint32_t v){std::memcpy(b.data()+at,&v,4);}
static uint64_t digest(const void *p,size_t n){uint64_t v=UINT64_C(14695981039346656037);auto*b=static_cast<const uint8_t*>(p);for(size_t i=0;i<n;i++){v^=b[i];v*=UINT64_C(1099511628211);}return v;}
static uint64_t stamp(){timespec t={};if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0||uint64_t(t.tv_sec)>UINT64_MAX/1000000000)throw std::runtime_error("monotonic clock");return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);}
static uint64_t elapsed(uint64_t begin){uint64_t end=stamp();if(end<begin)throw std::runtime_error("clock moved backwards");return end-begin;}

static void checksum(std::vector<uint8_t>&b){uint64_t h=digest(b.data()+HeaderBytes,b.size()-HeaderBytes);std::memcpy(b.data()+80,&h,8);}
static bool valid(const std::vector<uint8_t>&b,const VkPhysicalDeviceProperties&p){
  if(b.size()<HeaderBytes||std::memcmp(b.data(),"FPVKPC01",8)||word(b.data()+8)!=1||word(b.data()+12)!=HeaderBytes)return false;
  size_t n=word(b.data()+28);
  if(n<32||n>MaxPayload||b.size()!=HeaderBytes+n)return false;
  if(word(b.data()+16)!=p.vendorID||word(b.data()+20)!=p.deviceID||word(b.data()+24)!=p.driverVersion||
     std::memcmp(b.data()+32,p.pipelineCacheUUID,VK_UUID_SIZE)||std::memcmp(b.data()+48,shader_identity,32))return false;
  uint64_t h;std::memcpy(&h,b.data()+80,8);if(h!=digest(b.data()+HeaderBytes,n))return false;
  const uint8_t *native=b.data()+HeaderBytes;
  return word(native)==32&&word(native+4)==VK_PIPELINE_CACHE_HEADER_VERSION_ONE&&word(native+8)==p.vendorID&&
    word(native+12)==p.deviceID&&!std::memcmp(native+16,p.pipelineCacheUUID,VK_UUID_SIZE);
}
static std::vector<uint8_t> readCacheFile(const char *path){
  int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)throw std::runtime_error("cache read-only open");
  struct stat before={},after={};if(fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<off_t(HeaderBytes)||uint64_t(before.st_size)>HeaderBytes+MaxPayload){close(fd);throw std::runtime_error("cache file bounds");}
  std::vector<uint8_t> readback(size_t(before.st_size));size_t done=0;
  while(done<readback.size()){ssize_t n=read(fd,readback.data()+done,readback.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);throw std::runtime_error("cache read");}done+=size_t(n);}
  bool changed=fstat(fd,&after)||before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec;
  if(close(fd)||changed)throw std::runtime_error("cache file changed during read");return readback;
}
static std::vector<uint8_t> roundtrip(const char *path,const std::vector<uint8_t>&b){
  int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)throw std::runtime_error("new cache output exists or cannot be created");
  size_t done=0;while(done<b.size()){ssize_t n=write(fd,b.data()+done,b.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);throw std::runtime_error("cache write");}done+=size_t(n);}
  if(fsync(fd)){close(fd);throw std::runtime_error("cache fsync");}if(close(fd))throw std::runtime_error("cache close");
  auto readback=readCacheFile(path);if(readback!=b)throw std::runtime_error("cache file differs");return readback;
}

struct CacheProbe:Probe {
  static constexpr uint32_t Width=64,Height=64,Bytes=Width*Height*4,Rounds=8;
  VkPhysicalDeviceProperties properties={};VkShaderModule vertex=VK_NULL_HANDLE;
  VkImage target=VK_NULL_HANDLE,dummy=VK_NULL_HANDLE;VkDeviceMemory targetMemory=VK_NULL_HANDLE,dummyMemory=VK_NULL_HANDLE;
  VkImageView targetView=VK_NULL_HANDLE,dummyView=VK_NULL_HANDLE;VkRenderPass renderPass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
  VkCommandBuffer command=VK_NULL_HANDLE;VkDescriptorSet descriptors=VK_NULL_HANDLE;VkPipelineCache populated=VK_NULL_HANDLE;
  std::array<uint8_t,Bytes> pixels={};unsigned draws=0,guardChecks=0,rejected=0,fallbacks=0,driverFallbacks=0;
  uint64_t renderDigest=0;
  ~CacheProbe(){if(device){vkDeviceWaitIdle(device);if(populated)vkDestroyPipelineCache(device,populated,nullptr);if(framebuffer)vkDestroyFramebuffer(device,framebuffer,nullptr);if(renderPass)vkDestroyRenderPass(device,renderPass,nullptr);
    if(vertex)vkDestroyShaderModule(device,vertex,nullptr);if(targetView)vkDestroyImageView(device,targetView,nullptr);if(target)vkDestroyImage(device,target,nullptr);if(targetMemory)vkFreeMemory(device,targetMemory,nullptr);
    if(dummyView)vkDestroyImageView(device,dummyView,nullptr);if(dummy)vkDestroyImage(device,dummy,nullptr);if(dummyMemory)vkFreeMemory(device,dummyMemory,nullptr);}}
  void imageResource(VkImageType type,uint32_t w,uint32_t h,VkImageUsageFlags usage,VkImage &im,VkDeviceMemory &mem,VkImageView &iv){
    VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=type;ii.format=VK_FORMAT_R8G8B8A8_UNORM;ii.extent={w,h,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=usage;
    VK_CHECK(vkCreateImage(device,&ii,nullptr,&im));VkMemoryRequirements req={};vkGetImageMemoryRequirements(device,im,&req);
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,0);VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&mem));VK_CHECK(vkBindImageMemory(device,im,mem,0));
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=im;vi.viewType=type==VK_IMAGE_TYPE_1D?VK_IMAGE_VIEW_TYPE_1D:VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK_CHECK(vkCreateImageView(device,&vi,nullptr,&iv));
  }
  void begin(){VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));VK_CHECK(vkResetCommandPool(device,commandPool,0));VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK_CHECK(vkBeginCommandBuffer(command,&bi));}
  void finish(){VK_CHECK(vkEndCommandBuffer(command));VK_CHECK(vkResetFences(device,1,&fence));VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;VK_CHECK(vkQueueSubmit(queue,1,&submit,fence));VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull));}
  void initialize(){
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Frostpunk independent native pipeline cache proof";app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;VK_CHECK(vkCreateInstance(&ci,nullptr,&instance));
    uint32_t count=0;VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));if(count!=1)throw std::runtime_error("expected one physical GPU");VK_CHECK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    VkPhysicalDeviceDriverProperties driver={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};VkPhysicalDeviceProperties2 prop2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&driver};vkGetPhysicalDeviceProperties2(physical,&prop2);properties=prop2.properties;
    if(properties.vendorID!=0x13b5||driver.driverID!=VK_DRIVER_ID_ARM_PROPRIETARY||properties.apiVersion<VK_API_VERSION_1_3||!std::strstr(properties.deviceName,"Mali-G615")||std::strpbrk(properties.deviceName,"\"\\\r\n"))throw std::runtime_error("expected system Mali-G615 Vulkan1.3");
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);if(count>64)throw std::runtime_error("queue count bounds");VkQueueFamilyProperties qs[64]={};vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,qs);bool found=false;
    for(uint32_t i=0;i<count;i++)if(qs[i].queueCount&&(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){family=i;found=true;break;}if(!found)throw std::runtime_error("no graphics queue");
    VkFormatProperties fmt={};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&fmt);if((fmt.optimalTilingFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))!=(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT))throw std::runtime_error("RGBA8 feature profile");
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;VK_CHECK(vkCreateDevice(physical,&di,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
    for(uint32_t y=0;y<Height;y++)for(uint32_t x=0;x<Width;x++){size_t i=(y*Width+x)*4;pixels[i]=uint8_t(x*3+y*5);pixels[i+1]=uint8_t(x*11+y*7);pixels[i+2]=uint8_t((x^y)*13);pixels[i+3]=255;}renderDigest=digest(pixels.data(),pixels.size());
    makeBuffer(buffers[0],Bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);makeBuffer(buffers[1],Bytes+128,VK_BUFFER_USAGE_TRANSFER_DST_BIT);std::memcpy(buffers[0].mapped,pixels.data(),Bytes);
    imageResource(VK_IMAGE_TYPE_2D,Width,Height,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,image,imageMemory,view);
    imageResource(VK_IMAGE_TYPE_1D,1,1,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,dummy,dummyMemory,dummyView);
    imageResource(VK_IMAGE_TYPE_2D,Width,Height,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,target,targetMemory,targetView);
    VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;VK_CHECK(vkCreateSampler(device,&si,nullptr,&sampler));
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo li={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=2;li.pBindings=bindings;VK_CHECK(vkCreateDescriptorSetLayout(device,&li,nullptr,&setLayout));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2};VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=1;pi.poolSizeCount=1;pi.pPoolSizes=&ps;VK_CHECK(vkCreateDescriptorPool(device,&pi,nullptr,&descriptorPool));
    VkDescriptorSetAllocateInfo da={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptorPool;da.descriptorSetCount=1;da.pSetLayouts=&setLayout;VK_CHECK(vkAllocateDescriptorSets(device,&da,&descriptors));
    VkDescriptorImageInfo images[]={{sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{sampler,dummyView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};VkWriteDescriptorSet writes[2]={};for(unsigned i=0;i<2;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=descriptors;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];}vkUpdateDescriptorSets(device,2,writes,0,nullptr);
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};VkPipelineLayoutCreateInfo pl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;VK_CHECK(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
    VkShaderModuleCreateInfo sm={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=sizeof(cache_vertex_words);sm.pCode=cache_vertex_words;VK_CHECK(vkCreateShaderModule(device,&sm,nullptr,&vertex));sm.codeSize=sizeof(cache_fragment_words);sm.pCode=cache_fragment_words;VK_CHECK(vkCreateShaderModule(device,&sm,nullptr,&shader));
    VkAttachmentDescription at={};at.format=VK_FORMAT_R8G8B8A8_UNORM;at.samples=VK_SAMPLE_COUNT_1_BIT;at.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;at.storeOp=VK_ATTACHMENT_STORE_OP_STORE;at.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;at.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;at.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;at.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub={};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    VkSubpassDependency deps[]={{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0},{0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,0}};
    VkRenderPassCreateInfo rp={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&at;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=deps;VK_CHECK(vkCreateRenderPass(device,&rp,nullptr,&renderPass));
    VkFramebufferCreateInfo fb={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;fb.attachmentCount=1;fb.pAttachments=&targetView;fb.width=Width;fb.height=Height;fb.layers=1;VK_CHECK(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer));
    VkCommandPoolCreateInfo cp={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;VK_CHECK(vkCreateCommandPool(device,&cp,nullptr,&commandPool));VkCommandBufferAllocateInfo ca={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=commandPool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VK_CHECK(vkAllocateCommandBuffers(device,&ca,&command));VkFenceCreateInfo fc={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;VK_CHECK(vkCreateFence(device,&fc,nullptr,&fence));
    begin();VkMemoryBarrier host={VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkImageMemoryBarrier ib[2]={};for(unsigned i=0;i<2;i++){ib[i].sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;ib[i].image=i?dummy:image;ib[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib[i].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;ib[i].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ib[i].srcQueueFamilyIndex=ib[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,ib);
    VkBufferImageCopy upload={};upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};upload.imageExtent={Width,Height,1};vkCmdCopyBufferToImage(command,buffers[0].buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload);VkClearColorValue white={{1,1,1,1}};vkCmdClearColorImage(command,dummy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&white,1,&ib[1].subresourceRange);
    for(auto &b:ib){b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;}vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,2,ib);finish();
  }
  VkPipelineCache emptyCache(){VkPipelineCacheCreateInfo ci={VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};VkPipelineCache c=VK_NULL_HANDLE;VK_CHECK(vkCreatePipelineCache(device,&ci,nullptr,&c));return c;}
  VkPipelineCache loadCache(const std::vector<uint8_t>&blob,bool &loaded){
    loaded=false;if(valid(blob,properties)){VkPipelineCacheCreateInfo ci={VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};ci.initialDataSize=blob.size()-HeaderBytes;ci.pInitialData=blob.data()+HeaderBytes;VkPipelineCache c=VK_NULL_HANDLE;VkResult r=vkCreatePipelineCache(device,&ci,nullptr,&c);if(r==VK_SUCCESS){loaded=true;return c;}/* Failed create does not transfer ownership of its output. */++driverFallbacks;}
    else ++rejected;++fallbacks;return emptyCache();
  }
  uint64_t createPipeline(VkPipelineCache cache,uint32_t &flags){
    VkPipelineShaderStageCreateInfo stages[]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vertex,"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,shader,"main",nullptr}};
    VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport={0,0,float(Width),float(Height),0,1};VkRect2D rect={{0,0},{Width,Height}};VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&rect;
    VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment={};attachment.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo blend={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
    VkPipelineCreationFeedback feedback={};VkPipelineCreationFeedbackCreateInfo fi={VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};fi.pPipelineCreationFeedback=&feedback;
    VkGraphicsPipelineCreateInfo gp={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,&fi};gp.stageCount=2;gp.pStages=stages;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;gp.pMultisampleState=&ms;gp.pColorBlendState=&blend;gp.layout=pipelineLayout;gp.renderPass=renderPass;gp.basePipelineIndex=-1;
    uint64_t t=stamp();VK_CHECK(vkCreateGraphicsPipelines(device,cache,1,&gp,nullptr,&pipeline));uint64_t ns=elapsed(t);flags=feedback.flags;return ns;
  }
  void draw(){
    std::memset(buffers[1].mapped,0xcd,Bytes+128);begin();VkMemoryBarrier mb={VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    VkClearValue clear={};clear.color.float32[0]=clear.color.float32[2]=clear.color.float32[3]=1;VkRenderPassBeginInfo rp={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=renderPass;rp.framebuffer=framebuffer;rp.renderArea={{0,0},{Width,Height}};rp.clearValueCount=1;rp.pClearValues=&clear;vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptors,0,nullptr);uint32_t offsets[4]={};vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(offsets),offsets);vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
    VkBufferImageCopy copy={};copy.bufferOffset=64;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={Width,Height,1};vkCmdCopyImageToBuffer(command,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffers[1].buffer,1,&copy);
    VkBufferMemoryBarrier bb={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb.buffer=buffers[1].buffer;bb.size=VK_WHOLE_SIZE;vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&bb,0,nullptr);finish();
    auto*out=static_cast<const uint8_t*>(buffers[1].mapped);if(std::memcmp(out+64,pixels.data(),Bytes)||digest(out+64,Bytes)!=renderDigest)throw std::runtime_error("actual draw pixels differ");for(unsigned i=0;i<64;i++){if(out[i]!=0xcd||out[Bytes+64+i]!=0xcd)throw std::runtime_error("readback canary changed");guardChecks+=2;}++draws;vkDestroyPipeline(device,pipeline,nullptr);pipeline=VK_NULL_HANDLE;
  }
  std::vector<uint8_t> serialize(){
    size_t n=0;VK_CHECK(vkGetPipelineCacheData(device,populated,&n,nullptr));if(n<32||n>MaxPayload)throw std::runtime_error("native cache size bounds");std::vector<uint8_t>b(HeaderBytes+n);size_t actual=n;VK_CHECK(vkGetPipelineCacheData(device,populated,&actual,b.data()+HeaderBytes));if(actual<32||actual>n)throw std::runtime_error("native cache changed size");b.resize(HeaderBytes+actual);
    std::memcpy(b.data(),"FPVKPC01",8);put(b,8,1);put(b,12,HeaderBytes);put(b,16,properties.vendorID);put(b,20,properties.deviceID);put(b,24,properties.driverVersion);put(b,28,uint32_t(actual));std::memcpy(b.data()+32,properties.pipelineCacheUUID,16);std::memcpy(b.data()+48,shader_identity,32);checksum(b);if(!valid(b,properties))throw std::runtime_error("serialized native cache identity");return b;
  }
  void negativeCases(const std::vector<uint8_t>&good){
    for(unsigned mode=0;mode<17;mode++){auto bad=good;
      if(mode<10){const size_t offsets[]={0,8,12,16,20,24,32,48,80,28};bad[offsets[mode]]^=1;}
      else if(mode<15){const size_t offsets[]={0,4,8,12,16};bad[HeaderBytes+offsets[mode-10]]^=1;checksum(bad);}
      else if(mode==15)bad.pop_back();else bad.push_back(0);
      if(valid(bad,properties))throw std::runtime_error("invalid cache was accepted");bool loaded=false;VkPipelineCache cache=loadCache(bad,loaded);if(loaded)throw std::runtime_error("invalid data reached Vulkan cache initialData");uint32_t feedback=0;createPipeline(cache,feedback);draw();vkDestroyPipelineCache(device,cache,nullptr);
    }
  }
  void runCache(const char *path){
    initialize();populated=emptyCache();uint32_t coldFlags=0;uint64_t cold=createPipeline(populated,coldFlags);draw();auto blob=roundtrip(path,serialize());if(!valid(blob,properties))throw std::runtime_error("reload envelope identity");
    uint64_t timings[4][Rounds]={},loadTimes[Rounds]={};uint32_t feedback[4][Rounds]={};unsigned reloadAccepted=0;
    for(unsigned round=0;round<Rounds;round++)for(unsigned step=0;step<4;step++){unsigned kind=(round+step)%4;VkPipelineCache cache=VK_NULL_HANDLE;
      if(kind==1)cache=emptyCache();if(kind==2)cache=populated;if(kind==3){bool loaded=false;uint64_t t=stamp();cache=loadCache(blob,loaded);loadTimes[round]=elapsed(t);reloadAccepted+=loaded;}
      timings[kind][round]=createPipeline(cache,feedback[kind][round]);draw();if(kind!=0&&kind!=2)vkDestroyPipelineCache(device,cache,nullptr);
    }
    negativeCases(blob);if(rejected!=17)throw std::runtime_error("negative cache rejection count");
    char uuid[33]={};for(unsigned i=0;i<16;i++)std::snprintf(uuid+i*2,3,"%02x",properties.pipelineCacheUUID[i]);
    std::printf("{\"success\":true,\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"driver_version\":%u,\"pipeline_cache_uuid\":\"%s\",\"rounds\":%u,\"draws\":%u,\"pixels_checked\":%u,\"guard_bytes_checked\":%u,\"pixel_digest_fnv1a64\":\"%016" PRIx64 "\",\"native_payload_bytes\":%zu,\"file_bytes\":%zu,\"cache_identity_and_bounds_verified\":true,\"negative_cache_cases\":%u,\"empty_fallbacks\":%u,\"driver_initial_data_fallbacks\":%u,\"reloads_accepted\":%u,\"all_draws_identical\":true,\"native_pipeline_cache_benefit_proven\":false,\"implicit_driver_cache_controlled\":false,\"game_rendered\":false,\"cold_empty_cache_pipeline_ns\":%" PRIu64 ",\"cold_feedback_flags\":%u,\"pipeline_create_ns\":{",properties.deviceName,properties.vendorID,properties.deviceID,properties.driverVersion,uuid,Rounds,draws,draws*Width*Height,guardChecks,renderDigest,blob.size()-HeaderBytes,blob.size(),rejected,fallbacks,driverFallbacks,reloadAccepted,cold,coldFlags);
    const char *names[]={"null_cache","fresh_empty_cache","populated_cache","file_reloaded_cache"};for(unsigned k=0;k<4;k++){std::printf("%s\"%s\":[",k?",":"",names[k]);for(unsigned i=0;i<Rounds;i++)std::printf("%s%" PRIu64,i?",":"",timings[k][i]);std::printf("]");}std::printf("},\"cache_reload_ns\":[");for(unsigned i=0;i<Rounds;i++)std::printf("%s%" PRIu64,i?",":"",loadTimes[i]);std::printf("],\"pipeline_feedback_flags\":{");for(unsigned k=0;k<4;k++){std::printf("%s\"%s\":[",k?",":"",names[k]);for(unsigned i=0;i<Rounds;i++)std::printf("%s%u",i?",":"",feedback[k][i]);std::printf("]");}std::printf("}}\n");
  }
  void runExisting(const char *path){
    initialize();auto blob=readCacheFile(path);if(!valid(blob,properties))throw std::runtime_error("existing cache identity/bounds mismatch");
    bool loaded=false;uint64_t t=stamp();VkPipelineCache cache=loadCache(blob,loaded);uint64_t firstLoad=elapsed(t);if(draws)throw std::runtime_error("first cached pipeline was preceded by a draw");
    uint32_t firstFlags=0;uint64_t firstCreate=createPipeline(cache,firstFlags);draw();vkDestroyPipelineCache(device,cache,nullptr);unsigned reloadAccepted=loaded;
    uint64_t timings[3][Rounds]={},loadTimes[Rounds]={};uint32_t feedback[3][Rounds]={};
    for(unsigned round=0;round<Rounds;round++)for(unsigned step=0;step<3;step++){unsigned kind=(round+step)%3;cache=VK_NULL_HANDLE;
      if(kind==1)cache=emptyCache();if(kind==2){bool accepted=false;t=stamp();cache=loadCache(blob,accepted);loadTimes[round]=elapsed(t);reloadAccepted+=accepted;}
      timings[kind][round]=createPipeline(cache,feedback[kind][round]);draw();if(cache)vkDestroyPipelineCache(device,cache,nullptr);
    }
    negativeCases(blob);if(rejected!=17||readCacheFile(path)!=blob)throw std::runtime_error("existing cache changed or negative count differs");
    std::printf("{\"success\":true,\"mode\":\"fresh-process-load-existing\",\"device\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"driver_version\":%u,\"rounds\":%u,\"draws\":%u,\"pixels_checked\":%u,\"guard_bytes_checked\":%u,\"pixel_digest_fnv1a64\":\"%016" PRIx64 "\",\"native_payload_bytes\":%zu,\"file_bytes\":%zu,\"cache_identity_and_bounds_verified\":true,\"existing_cache_file_read_only\":true,\"existing_cache_file_unchanged\":true,\"first_pipeline_used_existing_cache\":%s,\"first_cached_creation_ns\":%" PRIu64 ",\"first_cache_load_ns\":%" PRIu64 ",\"first_creation_feedback_flags\":%u,\"negative_cache_cases\":%u,\"empty_fallbacks\":%u,\"driver_initial_data_fallbacks\":%u,\"reloads_accepted\":%u,\"all_draws_identical\":true,\"native_pipeline_cache_benefit_proven\":false,\"implicit_driver_cache_controlled\":false,\"game_rendered\":false,\"pipeline_create_ns\":{",properties.deviceName,properties.vendorID,properties.deviceID,properties.driverVersion,Rounds,draws,draws*Width*Height,guardChecks,renderDigest,blob.size()-HeaderBytes,blob.size(),loaded?"true":"false",firstCreate,firstLoad,firstFlags,rejected,fallbacks,driverFallbacks,reloadAccepted);
    const char *names[]={"null_cache","fresh_empty_cache","file_reloaded_cache"};for(unsigned k=0;k<3;k++){std::printf("%s\"%s\":[",k?",":"",names[k]);for(unsigned i=0;i<Rounds;i++)std::printf("%s%" PRIu64,i?",":"",timings[k][i]);std::printf("]");}std::printf("},\"cache_reload_ns\":[");for(unsigned i=0;i<Rounds;i++)std::printf("%s%" PRIu64,i?",":"",loadTimes[i]);std::printf("],\"pipeline_feedback_flags\":{");for(unsigned k=0;k<3;k++){std::printf("%s\"%s\":[",k?",":"",names[k]);for(unsigned i=0;i<Rounds;i++)std::printf("%s%u",i?",":"",feedback[k][i]);std::printf("]");}std::printf("}}\n");
  }
};
int main(int argc,char **argv){bool existing=argc==3&&!std::strcmp(argv[1],"--load-existing");if(argc!=2&&!existing){std::fprintf(stderr,"Usage: mali_pipeline_cache NEW_CACHE_FILE | --load-existing EXISTING_CACHE_FILE\n");return 64;}try{CacheProbe probe;if(existing)probe.runExisting(argv[2]);else probe.runCache(argv[1]);return 0;}catch(const std::exception&e){std::fprintf(stderr,"MALI_PIPELINE_CACHE_STOP %s\n",e.what());return 73;}}
