/* Real asset uploads use only D3D11. Vulkan is queried solely to identify hardware. */
#include <d3d11.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include <dlfcn.h>
#include "driver.h"
#include "dxgi_astc_formats.h"
#include "texture_manifest.h"
#include "texture_shaders.h"

static PFN_vkGetInstanceProcAddr provider;
extern "C" __attribute__((visibility("default")))
PFN_vkGetInstanceProcAddr frostpunk_dxvk_get_instance_proc_addr(){return provider;}
extern "C" __attribute__((visibility("default")))
int frostpunk_dxvk_get_display_mode(uint32_t*,uint32_t*,uint32_t*){return 0;}
template<typename T>struct Ref {
    T *p=nullptr;Ref()=default;Ref(const Ref&)=delete;Ref&operator=(const Ref&)=delete;
    ~Ref(){if(p)p->Release();}T *operator->()const{return p;}
};
static std::string last_step,location;
static HRESULT last_hr=S_OK;
static void check(const char *step,HRESULT hr){last_step=step;last_hr=hr;if(FAILED(hr))throw std::runtime_error(step);}
static std::string json(const std::string &s){
    std::string out="\"";for(unsigned char c:s){
        if(c=='"'||c=='\\'){out+='\\';out+=c;}
        else if(c<32){char text[8];std::snprintf(text,sizeof(text),"\\u%04x",c);out+=text;}
        else out+=c;
    }return out+'"';
}
static std::vector<unsigned char> read_file(const std::string &path,size_t bytes){
    FILE *file=std::fopen(path.c_str(),"rb");if(!file)throw std::runtime_error("open calibrated input");
    if(std::fseek(file,0,SEEK_END)||std::ftell(file)<0||size_t(std::ftell(file))!=bytes||std::fseek(file,0,SEEK_SET)){
        std::fclose(file);throw std::runtime_error("calibrated input length mismatch");
    }
    std::vector<unsigned char> data(bytes);size_t read=std::fread(data.data(),1,bytes,file);std::fclose(file);
    if(read!=bytes)throw std::runtime_error("short calibrated input");return data;
}
static VkPhysicalDeviceProperties identify(){
    auto create=reinterpret_cast<PFN_vkCreateInstance>(provider(nullptr,"vkCreateInstance"));
    if(!create)throw std::runtime_error("system Vulkan create missing");
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;
    VkInstance instance=VK_NULL_HANDLE;if(create(&ci,nullptr,&instance)!=VK_SUCCESS)throw std::runtime_error("system Vulkan1.3 instance failed");
    auto destroy=reinterpret_cast<PFN_vkDestroyInstance>(provider(instance,"vkDestroyInstance"));
    auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(provider(instance,"vkEnumeratePhysicalDevices"));
    auto getprops=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(provider(instance,"vkGetPhysicalDeviceProperties2"));
    if(!destroy||!enumerate||!getprops)throw std::runtime_error("system Vulkan identity functions missing");
    uint32_t count=0;VkResult vr=enumerate(instance,&count,nullptr);
    if(vr!=VK_SUCCESS||count!=1){destroy(instance,nullptr);throw std::runtime_error("expected one native GPU");}
    VkPhysicalDevice physical=VK_NULL_HANDLE;vr=enumerate(instance,&count,&physical);
    if(vr!=VK_SUCCESS||count!=1){destroy(instance,nullptr);throw std::runtime_error("native GPU enumerate failed");}
    VkPhysicalDeviceDriverProperties driver={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&driver};getprops(physical,&props);destroy(instance,nullptr);
    if(props.properties.vendorID!=0x13b5||driver.driverID!=VK_DRIVER_ID_ARM_PROPRIETARY||
       !std::strstr(props.properties.deviceName,"Mali-G615")||props.properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)
        throw std::runtime_error("expected native system Mali-G615");
    return props.properties;
}
struct CaseResult {
    const char *variant;const char *shape;unsigned layers;bool distinct;
    uint64_t pixels=0,components=0,bad=0,deltaAboveOne=0;unsigned maxDelta[4]={};unsigned draws=0;
};
struct Constants {unsigned level,layer,width,height;};
static unsigned char face_mask(unsigned layer,unsigned channel){
    return static_cast<unsigned char>(layer*(channel==0?109u:channel==1?73u:37u));
}

static CaseResult run_case(ID3D11Device *device,ID3D11DeviceContext *context,
    const TextureVariant &variant,unsigned shape,const std::string &directory,
    ID3D11VertexShader *vertex,ID3D11PixelShader *pixel,ID3D11SamplerState *sampler,
    ID3D11RasterizerState *raster,ID3D11Buffer *constants){
    const unsigned layers=shape==0?1:shape==1?2:6;
    const bool distinct=shape!=0&&std::strcmp(variant.name,"bgra8-unorm")==0;
    CaseResult result={variant.name,shape==0?"2D":shape==1?"2DArray":"Cube",layers,distinct};
    auto payload=read_file(directory+"/"+variant.payload,variant.payloadBytes);
    auto reference=read_file(directory+"/"+variant.reference,variant.referenceBytes);
    std::vector<std::vector<unsigned char>> uploads(layers,payload);
    if(distinct)for(unsigned layer=0;layer<layers;layer++)for(const auto &mip:variant.mips){
        for(size_t i=0;i<size_t(mip.width)*mip.height;i++){
            auto p=uploads[layer].data()+mip.payloadOffset+i*4;
            p[0]^=face_mask(layer,2);p[1]^=face_mask(layer,1);p[2]^=face_mask(layer,0);
        }
    }
    D3D11_SUBRESOURCE_DATA init[54]={};
    for(unsigned layer=0;layer<layers;layer++)for(unsigned mip=0;mip<9;mip++){
        const auto &m=variant.mips[mip];init[D3D11CalcSubresource(mip,layer,9)]={uploads[layer].data()+m.payloadOffset,m.rowPitch,UINT(m.payloadBytes)};
    }
    location=std::string(variant.name)+"/"+result.shape;
    UINT support=0;check("CheckFormatSupport",device->CheckFormatSupport(variant.format,&support));
    const UINT required=D3D11_FORMAT_SUPPORT_TEXTURE2D|D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if((support&required)!=required)throw std::runtime_error("calibrated texture format not sampleable");
    Ref<ID3D11Texture2D> texture;Ref<ID3D11ShaderResourceView> srv;
    D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=256;td.MipLevels=9;td.ArraySize=layers;td.Format=variant.format;
    td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags=shape==2?D3D11_RESOURCE_MISC_TEXTURECUBE:0;
    check("CreateTexture2D_all9mips_initial_upload",device->CreateTexture2D(&td,init,&texture.p));
    D3D11_SHADER_RESOURCE_VIEW_DESC sv={};sv.Format=variant.format;
    if(shape==0){sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=9;}
    else if(shape==1){sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;sv.Texture2DArray.MipLevels=9;sv.Texture2DArray.ArraySize=layers;}
    else {sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE;sv.TextureCube.MipLevels=9;}
    check("CreateShaderResourceView_all9mips",device->CreateShaderResourceView(texture.p,&sv,&srv.p));
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex,nullptr,0);context->PSSetShader(pixel,nullptr,0);
    context->PSSetShaderResources(shape,1,&srv.p);context->PSSetSamplers(0,1,&sampler);
    context->PSSetConstantBuffers(0,1,&constants);context->RSSetState(raster);
    for(unsigned layer=0;layer<layers;layer++)for(unsigned level=0;level<9;level++){
        const auto &m=variant.mips[level];location=std::string(variant.name)+"/"+result.shape+"/layer"+std::to_string(layer)+"/mip"+std::to_string(level);
        Ref<ID3D11Texture2D> target,staging;Ref<ID3D11RenderTargetView> rtv;
        D3D11_TEXTURE2D_DESC out={};out.Width=m.width;out.Height=m.height;out.MipLevels=out.ArraySize=1;
        out.Format=DXGI_FORMAT_R8G8B8A8_UNORM;out.SampleDesc.Count=1;out.Usage=D3D11_USAGE_DEFAULT;out.BindFlags=D3D11_BIND_RENDER_TARGET;
        check("CreateTexture2D_RGBA8_target",device->CreateTexture2D(&out,nullptr,&target.p));
        check("CreateRenderTargetView",device->CreateRenderTargetView(target.p,nullptr,&rtv.p));
        out.Usage=D3D11_USAGE_STAGING;out.BindFlags=0;out.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        check("CreateTexture2D_staging",device->CreateTexture2D(&out,nullptr,&staging.p));
        D3D11_VIEWPORT viewport={0,0,float(m.width),float(m.height),0,1};context->RSSetViewports(1,&viewport);
        const float clear[]={1,0,1,0};context->ClearRenderTargetView(rtv.p,clear);context->OMSetRenderTargets(1,&rtv.p,nullptr);
        Constants params={level,layer,m.width,m.height};context->UpdateSubresource(constants,0,nullptr,&params,0,0);
        context->Draw(3,0);result.draws++;context->CopyResource(staging.p,target.p);
        D3D11_MAPPED_SUBRESOURCE map={};check("Map_staging_after_Draw_CopyResource",context->Map(staging.p,0,D3D11_MAP_READ,0,&map));
        if(!map.pData||map.RowPitch<m.width*4){context->Unmap(staging.p,0);throw std::runtime_error("invalid staging map span");}
        for(unsigned y=0;y<m.height;y++)for(unsigned x=0;x<m.width;x++){
            const auto actual=static_cast<unsigned char*>(map.pData)+size_t(y)*map.RowPitch+x*4;
            const auto expected=reference.data()+m.referenceOffset+(size_t(y)*m.width+x)*4;
            result.pixels++;
            for(unsigned channel=0;channel<4;channel++){
                unsigned value=expected[channel];if(distinct&&channel<3)value^=face_mask(layer,channel);
                unsigned delta=unsigned(std::abs(int(actual[channel])-int(value)));result.components++;
                result.maxDelta[channel]=std::max(result.maxDelta[channel],delta);result.deltaAboveOne+=delta>1;
                if(delta>2){
                    if(result.bad<8)std::fprintf(stderr,"D3D11_TEXTURE_MISMATCH %s x=%u y=%u channel=%u actual=%u expected=%u delta=%u\n",location.c_str(),x,y,channel,actual[channel],value,delta);
                    result.bad++;
                }
            }
        }
        context->Unmap(staging.p,0);check("GetDeviceRemovedReason",device->GetDeviceRemovedReason());
        ID3D11RenderTargetView *none=nullptr;context->OMSetRenderTargets(1,&none,nullptr);
        if(result.bad)throw std::runtime_error("D3D11 real asset exceeds calibrated two-LSB tolerance");
    }
    ID3D11ShaderResourceView *none=nullptr;context->PSSetShaderResources(shape,1,&none);
    return result;
}

int main(int argc,char **argv){
    bool success=false;std::string error;std::vector<CaseResult> cases;VkPhysicalDeviceProperties hardware={};
    D3D_FEATURE_LEVEL selected=D3D_FEATURE_LEVEL();
    try{
        if(argc!=3)throw std::runtime_error("usage: mali_d3d11_texture <native-DXVK-D3D11.so> <fixture-directory>");
        char detail[512]={};provider=frostpunk_gpu_open("","",detail,sizeof(detail));if(!provider)throw std::runtime_error(detail);
        hardware=identify();setenv("DXVK_WSI_DRIVER","Android",1);setenv("DXVK_LOG_PATH","none",1);setenv("DXVK_STATE_CACHE","disable",1);
        void *module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!module)throw std::runtime_error(dlerror());
        using Create=HRESULT(*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
        auto create=reinterpret_cast<Create>(dlsym(module,"D3D11CreateDevice"));if(!create)throw std::runtime_error("D3D11CreateDevice export missing");
        Ref<ID3D11Device> device;Ref<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_0;
        check("D3D11CreateDevice_FL11",create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&requested,1,D3D11_SDK_VERSION,&device.p,&selected,&context.p));
        if(selected!=requested)throw std::runtime_error("requested D3D11 level not returned");
        Ref<ID3D11VertexShader> vertex;Ref<ID3D11PixelShader> pixel[3];Ref<ID3D11SamplerState> sampler;
        Ref<ID3D11RasterizerState> raster;Ref<ID3D11Buffer> constants;
        check("CreateVertexShader",device->CreateVertexShader(vs_main_dxbc,sizeof(vs_main_dxbc),nullptr,&vertex.p));
        check("CreatePixelShader_2D",device->CreatePixelShader(ps_2d_dxbc,sizeof(ps_2d_dxbc),nullptr,&pixel[0].p));
        check("CreatePixelShader_Array",device->CreatePixelShader(ps_array_dxbc,sizeof(ps_array_dxbc),nullptr,&pixel[1].p));
        check("CreatePixelShader_Cube",device->CreatePixelShader(ps_cube_dxbc,sizeof(ps_cube_dxbc),nullptr,&pixel[2].p));
        D3D11_SAMPLER_DESC sd={};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc=D3D11_COMPARISON_NEVER;sd.MaxAnisotropy=1;sd.MaxLOD=D3D11_FLOAT32_MAX;
        check("CreateSamplerState",device->CreateSamplerState(&sd,&sampler.p));
        D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=true;
        check("CreateRasterizerState",device->CreateRasterizerState(&rd,&raster.p));
        D3D11_BUFFER_DESC bd={};bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        check("CreateBuffer_sampling_constants",device->CreateBuffer(&bd,nullptr,&constants.p));
        for(const auto &variant:texture_variants){
            cases.push_back(run_case(device.p,context.p,variant,0,argv[2],vertex.p,pixel[0].p,sampler.p,raster.p,constants.p));
            if(variant.block>1||std::strcmp(variant.name,"bgra8-unorm")==0)for(unsigned shape=1;shape<=2;shape++)
                cases.push_back(run_case(device.p,context.p,variant,shape,argv[2],vertex.p,pixel[shape].p,sampler.p,raster.p,constants.p));
        }
        context->ClearState();context->Flush();check("FinalGetDeviceRemovedReason",device->GetDeviceRemovedReason());
        success=cases.size()==16;
    }catch(const std::exception &e){error=e.what();}
    std::printf("{\"success\":%s,\"asset_key\":53614496,\"provider\":\"system\",\"device\":%s,\"feature_level\":%u,"
        "\"D3D11_texture_upload\":true,\"direct_Vulkan_texture_upload\":false,\"mips_per_image\":9,\"tolerance_LSB\":2,\"cases\":[",
        success?"true":"false",json(hardware.deviceName).c_str(),unsigned(selected));
    uint64_t total=0;unsigned draws=0;
    for(size_t i=0;i<cases.size();i++){
        const auto &c=cases[i];total+=c.pixels;draws+=c.draws;
        std::printf("%s{\"variant\":%s,\"shape\":%s,\"layers\":%u,\"distinct_layer_payloads\":%s,\"pixels\":%llu,"
          "\"draws\":%u,\"bad_components\":%llu,\"components_delta_gt1\":%llu,\"max_delta_RGBA\":[%u,%u,%u,%u]}",
          i?",":"",json(c.variant).c_str(),json(c.shape).c_str(),c.layers,c.distinct?"true":"false",(unsigned long long)c.pixels,
          c.draws,(unsigned long long)c.bad,(unsigned long long)c.deltaAboveOne,c.maxDelta[0],c.maxDelta[1],c.maxDelta[2],c.maxDelta[3]);
    }
    std::printf("],\"pixels_checked\":%llu,\"draws\":%u,\"completed_cases\":%zu,\"last_step\":%s,\"last_hresult\":%u,\"location\":%s,\"error\":%s,\"game_rendered\":false}\n",
        (unsigned long long)total,draws,cases.size(),json(last_step).c_str(),unsigned(last_hr),json(location).c_str(),json(error).c_str());
    return success?0:73;
}
