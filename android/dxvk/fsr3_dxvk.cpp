#include "dxvk_fsr3.h"
#include "dxvk_device.h"
#include "dxvk_context.h"
#include "dxvk_instance.h"
#include <dxvk_fsr3_prepare.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <time.h>

extern "C" void frostpunk_dxvk_upscaler_failed(uint32_t) __attribute__((weak));

namespace dxvk {
const FpFsr3Api *DxvkFsr3::load(){
    static const FpFsr3Api *api=[](){
        void *library=dlopen("libfrostpunk_fsr3.so",RTLD_NOW|RTLD_LOCAL);
        if(!library){Logger::warn(str::format("FP_FSR3 unavailable: ",dlerror()));return static_cast<const FpFsr3Api*>(nullptr);}
        auto get=reinterpret_cast<const FpFsr3Api*(*)()>(dlsym(library,"frostpunk_fsr3_api"));
        const FpFsr3Api *result=get?get():nullptr;
        if(!result||result->version!=1||result->bytes!=sizeof(FpFsr3Api)){dlclose(library);return static_cast<const FpFsr3Api*>(nullptr);}
        return result;
    }();
    return api;
}
DxvkFsr3::DxvkFsr3(const Rc<DxvkDevice>& device,VkExtent2D size,VkExtent2D target,bool reversed)
 :input(size),output(target),inverted(reversed),m_device(device){
    if(!device->features().vk12.vulkanMemoryModelDeviceScope){
        Logger::err("FP_FSR3 requires vulkanMemoryModelDeviceScope");fail(1);return;
    }
    api=load();if(!api){fail(1);return;}
    const VkFormat formats[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16_SINT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R32_UINT};
    for(unsigned i=0;i<images.size();++i){
        VkExtent2D extent=i==Output?output:i==Flow?VkExtent2D{(input.width+7)/8,(input.height+7)/8}:i==SceneChange?VkExtent2D{3,1}:input;
        DxvkImageCreateInfo info{};info.type=VK_IMAGE_TYPE_2D;info.format=formats[i];info.sampleCount=VK_SAMPLE_COUNT_1_BIT;
        info.extent={extent.width,extent.height,1};info.numLayers=info.mipLevels=1;
        info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
        info.access=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL;info.layout=VK_IMAGE_LAYOUT_GENERAL;
        Rc<DxvkImage> image=device->createImage(info,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        DxvkImageViewCreateInfo view{};view.format=formats[i];view.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
        view.aspect=VK_IMAGE_ASPECT_COLOR_BIT;view.numLevels=view.numLayers=1;
        images[i]=device->createImageView(image,view);
    }
    DxvkSamplerCreateInfo samplerInfo{};samplerInfo.magFilter=samplerInfo.minFilter=VK_FILTER_NEAREST;
    samplerInfo.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU=samplerInfo.addressModeV=samplerInfo.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy=1;samplerInfo.compareOp=VK_COMPARE_OP_ALWAYS;samplerInfo.reductionMode=VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    sampler=device->createSampler(samplerInfo);
    std::array<DxvkBindingInfo,8> bindings{};
    for(unsigned i=0;i<bindings.size();++i)bindings[i]={i<4?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        i,VK_IMAGE_VIEW_TYPE_2D,VK_SHADER_STAGE_COMPUTE_BIT,VkAccessFlags(i<4?VK_ACCESS_SHADER_READ_BIT:i==7?VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_SHADER_WRITE_BIT)};
    DxvkShaderCreateInfo shader{};shader.stage=VK_SHADER_STAGE_COMPUTE_BIT;shader.bindingCount=bindings.size();shader.bindings=bindings.data();
    shader.pushConstStages=VK_SHADER_STAGE_COMPUTE_BIT;shader.pushConstSize=sizeof(uint32_t);
    prepare=new DxvkShader(shader,SpirvCodeBuffer(dxvk_fsr3_prepare));
    uint32_t error=1;
    auto resolve=reinterpret_cast<PFN_vkGetDeviceProcAddr>(device->instance()->vki()->sym("vkGetDeviceProcAddr"));
    sdk=api->create(device->handle(),device->adapter()->handle(),resolve,input.width,input.height,output.width,output.height,inverted,&error);
    if(!sdk){Logger::err(str::format("FP_FSR3 create input=",input.width,"x",input.height," output=",output.width,"x",output.height));fail(error);return;}
    Logger::info(str::format("FP_FSR3 created ",input.width,"x",input.height," -> ",output.width,"x",output.height," optical_flow=1"));
}
DxvkFsr3::~DxvkFsr3(){retire();}
void DxvkFsr3::retire(){if(sdk){api->destroy(sdk);sdk=nullptr;}}
bool DxvkFsr3::matches(VkExtent2D size,VkExtent2D target,bool reversed)const{
    return size==input&&target==output&&reversed==inverted;
}
void DxvkFsr3::nextJitter(uint32_t index,float& x,float& y)const{
    x=y=0;if(sdk&&!failed)api->jitter(index,input.width,output.width,&x,&y);
}
void DxvkFsr3::fail(uint32_t error){
    failed=true;Logger::err(str::format("FP_FSR3 failed status=",error,"; selecting FSR1"));
    if(frostpunk_dxvk_upscaler_failed)frostpunk_dxvk_upscaler_failed(error);
}
static FpFsr3Image image(const Rc<DxvkImageView>& view){
    auto size=view->mipLevelExtent(0);return {view->image()->handle(),view->info().format,size.width,size.height};
}
FpFsr3Frame DxvkFsr3::frame(const FpTemporalFrame& metadata,const Rc<DxvkImageView>& source)const{
    FpFsr3Frame result{};result.color=image(source);result.depth=image(images[Depth]);result.motion=image(images[Motion]);
    result.reactive=image(images[Reactive]);result.output=image(images[Output]);result.flow=image(images[Flow]);result.sceneChange=image(images[SceneChange]);
    result.dilatedDepth=image(images[DilatedDepth]);result.dilatedMotion=image(images[DilatedMotion]);result.previousDepth=image(images[PreviousDepth]);
    result.jitterX=metadata.jitterX;result.jitterY=metadata.jitterY;result.nearPlane=metadata.nearPlane;
    result.farPlane=metadata.farPlane;result.verticalFov=metadata.verticalFov;return result;
}

bool DxvkFsr3::projection(const float *vp,const float *view,FpTemporalFrame& frame){
    double inverse[4][8]{};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j){
        if(!std::isfinite(vp[i*4+j])||!std::isfinite(view[i*4+j]))return false;
        inverse[i][j]=view[i*4+j];inverse[i][j+4]=i==j;
    }
    for(unsigned i=0;i<4;++i){
        unsigned pivot=i;for(unsigned j=i+1;j<4;++j)if(std::fabs(inverse[j][i])>std::fabs(inverse[pivot][i]))pivot=j;
        if(std::fabs(inverse[pivot][i])<1e-10)return false;
        for(unsigned j=0;j<8;++j)std::swap(inverse[i][j],inverse[pivot][j]);
        double divisor=inverse[i][i];for(double& v:inverse[i])v/=divisor;
        for(unsigned j=0;j<4;++j)if(j!=i){double v=inverse[j][i];for(unsigned k=0;k<8;++k)inverse[j][k]-=v*inverse[i][k];}
    }
    for(unsigned order=0;order<2;++order){
        double p[4][4]{};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
            p[i][j]+=order?inverse[i][k+4]*vp[k*4+j]:vp[i*4+k]*inverse[k][j+4];
        double other=0;for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            if(!((i==j&&i<3)||(i==2&&j==3)||(i==3&&j==2)))other=std::max(other,std::fabs(p[i][j]));
        if(other>.001||std::fabs(p[0][0])<.01||std::fabs(p[1][1])<.01)continue;
        bool row=std::fabs(std::fabs(p[2][3])-1)<std::fabs(std::fabs(p[3][2])-1);
        double a=p[2][2],s=row?p[2][3]:p[3][2],b=row?p[3][2]:p[2][3];
        if(std::fabs(std::fabs(s)-1)>.001||std::fabs(a)<1e-10||std::fabs(s-a)<1e-10)continue;
        double z0=std::fabs(-b/a),z1=std::fabs(b/(s-a));
        double nearPlane=std::min(z0,z1),farPlane=std::max(z0,z1);
        if(!std::isfinite(nearPlane)||!std::isfinite(farPlane)||nearPlane<=0||farPlane<=nearPlane)continue;
        frame.nearPlane=nearPlane;frame.farPlane=farPlane;frame.verticalFov=2*std::atan(1/std::fabs(p[1][1]));frame.inverted=z1<z0;
        std::copy(view,view+16,frame.view.begin());return true;
    }
    return false;
}

uint32_t DxvkContext::fpFsr3External(const Rc<DxvkFsr3>& state,FpFsr3Frame& frame,bool optical){
    spillRenderPass(true);
    m_execBarriers.recordCommands(m_cmd);
    std::array<Rc<DxvkImageView>,10> views{};views[0]=m_fpFsr3Color;
    for(unsigned i=0;i<state->images.size();++i)views[i+1]=state->images[i];
    for(const auto& view:views){
        prepareImage(view->image(),view->imageSubresources());
        m_execAcquires.accessImage(view->image(),view->imageSubresources(),view->imageInfo().layout,
            view->imageInfo().stages,view->imageInfo().access,VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        m_cmd->trackResource<DxvkAccess::Write>(view->image());m_cmd->trackResource<DxvkAccess::None>(view);
    }
    m_execAcquires.recordCommands(m_cmd);
    m_cmd->trackResource<DxvkAccess::Write>(state);
    m_cmd->m_cmd.usedFlags.set(DxvkCmdBuffer::ExecBuffer);
    VkCommandBuffer command=m_cmd->getCmdBuffer(DxvkCmdBuffer::ExecBuffer);
    uint32_t status=optical?state->api->flow(state->sdk,command,&frame):state->api->upscale(state->sdk,command,&frame);
    invalidateState();
    for(const auto& view:views)m_execBarriers.accessImage(view->image(),view->imageSubresources(),VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        view->imageInfo().layout,view->imageInfo().stages,view->imageInfo().access);
    m_execBarriers.recordCommands(m_cmd);return status;
}

Rc<DxvkImageView> DxvkContext::fpFsr3Upscale(const Rc<DxvkFsr3>& state,const FpTemporalFrame& metadata,const Rc<DxvkImageView>& source){
    if(state==nullptr||state->failed||!state->sdk||!metadata.valid||metadata.depth==nullptr||metadata.opaque==nullptr)return nullptr;
    if(!state->initialized){
        for(const auto& view:state->images)initImage(view->image(),view->imageSubresources(),VK_IMAGE_LAYOUT_UNDEFINED);
        state->initialized=true;
    }
    auto sceneExtent=metadata.opaque->mipLevelExtent(0);
    if(sceneExtent.width!=state->input.width||sceneExtent.height!=state->input.height){state->reset=true;return nullptr;}
    if(metadata.opaque->info().format!=source->info().format){state->fail(1);return nullptr;}
    m_fpFsr3Color=metadata.opaque;
    FpFsr3Frame frame=state->frame(metadata,metadata.opaque);
    bool first=!state->lastNanoseconds;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);uint64_t nanos=uint64_t(now.tv_sec)*1000000000+now.tv_nsec;
    frame.milliseconds=state->lastNanoseconds?std::clamp(float(nanos-state->lastNanoseconds)/1000000.f,.1f,1000.f):16.6667f;
    frame.reset=state->reset||frame.milliseconds>250;
    if(!state->reset){float change=0;for(unsigned i=0;i<16;++i)change=std::max(change,std::fabs(metadata.view[i]-state->previousView[i]));if(change>32)frame.reset=1;}
    state->previousView=metadata.view;state->lastNanoseconds=nanos;state->reset=false;
    uint32_t status=fpFsr3External(state,frame,true);
    if(status){Logger::err("FP_FSR3 optical flow dispatch failed");state->fail(status);m_fpFsr3Color=nullptr;return nullptr;}
    bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(Rc<DxvkShader>(state->prepare));
    const Rc<DxvkImageView> inputs[]={metadata.depth,state->images[DxvkFsr3::Flow],source,metadata.opaque,
        state->images[DxvkFsr3::Depth],state->images[DxvkFsr3::Motion],state->images[DxvkFsr3::Reactive],state->images[DxvkFsr3::Output]};
    for(unsigned i=0;i<8;++i){bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT,i,Rc<DxvkImageView>(inputs[i]));if(i<4)bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT,i,Rc<DxvkSampler>(state->sampler));}
    uint32_t composite=0;pushConstants(0,sizeof(composite),&composite);
    dispatch((state->input.width+7)/8,(state->input.height+7)/8,1);
    status=fpFsr3External(state,frame,false);m_fpFsr3Color=nullptr;
    if(status){Logger::err("FP_FSR3 upscale dispatch failed");state->fail(status);return nullptr;}
    bindShader<VK_SHADER_STAGE_COMPUTE_BIT>(Rc<DxvkShader>(state->prepare));
    for(unsigned i=0;i<8;++i){bindResourceImageView(VK_SHADER_STAGE_COMPUTE_BIT,i,Rc<DxvkImageView>(inputs[i]));if(i<4)bindResourceSampler(VK_SHADER_STAGE_COMPUTE_BIT,i,Rc<DxvkSampler>(state->sampler));}
    composite=1;pushConstants(0,sizeof(composite),&composite);
    dispatch((state->output.width+7)/8,(state->output.height+7)/8,1);
    if(first)Logger::info(str::format("FP_FSR3 dispatch input=",state->input.width,"x",state->input.height,
        " near=",metadata.nearPlane," far=",metadata.farPlane," fov=",metadata.verticalFov,
        " color_format=",source->info().format," opaque_format=",metadata.opaque->info().format," ui=spatial status=0"));
    return state->images[DxvkFsr3::Output];
}
}
