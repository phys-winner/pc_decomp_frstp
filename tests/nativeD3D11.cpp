/* Native Bionic/AArch64 DXVK validation. No PE execution, window or fake GPU. */
#include <d3d11.h>
#include <dxgi.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <dlfcn.h>
#include "driver.h"
#include "nativeD3D11_shaders.h"

static PFN_vkGetInstanceProcAddr provider;
static std::atomic<bool> display_enabled{false};
extern "C" __attribute__((visibility("default")))
PFN_vkGetInstanceProcAddr frostpunk_dxvk_get_instance_proc_addr() { return provider; }
extern "C" __attribute__((visibility("default")))
int frostpunk_dxvk_get_display_mode(uint32_t* width,uint32_t* height,uint32_t* refresh) {
    if(!width||!height||!refresh||!display_enabled.load(std::memory_order_acquire))return 0;
    *width=960;*height=540;*refresh=60;return 1;
}

template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
};
struct Step { const char* name; HRESULT hr; };
static std::vector<Step> steps;
static void check(const char* name, HRESULT hr) {
    steps.push_back({name, hr});
    if (FAILED(hr)) throw std::runtime_error(name);
}
static std::string json(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += c;
    }
    return out + "\"";
}

int main(int argc, char** argv) {
    bool success = false;
    std::string error, adapter_name;
    unsigned checked = 0, matched = 0;
    unsigned char pixels[64] = {};
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL();
    UINT sampled_format_support = 0;
#if defined(FP_GPU_SYSTEM)
    constexpr DXGI_FORMAT sampled_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    const unsigned char source_pixels[64] = {0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255,
        0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255,
        0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255,
        0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255};
#else
    constexpr DXGI_FORMAT sampled_format = DXGI_FORMAT_BC3_UNORM;
    // Alpha255 and RGB565 red; all indices0 produce opaque red.
    const unsigned char source_pixels[16] = {255,0,0,0,0,0,0,0,0,248,224,7,0,0,0,0};
#endif
    VkPhysicalDeviceProperties vkprops = {};
    uint32_t vk_driver_id = 0;
    bool no_policy_no_outputs=false,display_policy_pass=false;
    bool unsupported_operations_rejected=false;
    DXGI_OUTPUT_DESC output_desc={};DXGI_MODE_DESC output_mode={};std::string output_name;
    try {
        if (argc != 4) throw std::runtime_error("usage: nativeD3D11 runtimeDir driverDir/ dxvkLibraryPath");
        char detail[512] = {};
        provider = frostpunk_gpu_open(argv[1], argv[2], detail, sizeof(detail));
        if (!provider) throw std::runtime_error(detail);
        auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(provider(nullptr, "vkCreateInstance"));
        if (!create_instance) throw std::runtime_error("provider vkCreateInstance absent");
        // Identify the actual provider before DXVK selection; stock/CPU is rejected.
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        VkInstance vk = VK_NULL_HANDLE;
        VkResult vr = create_instance(&ci, nullptr, &vk);
        if (vr != VK_SUCCESS) throw std::runtime_error("provider Vulkan1.3 instance failed: " + std::to_string(vr));
        auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(provider(vk, "vkDestroyInstance"));
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(provider(vk, "vkEnumeratePhysicalDevices"));
        auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(provider(vk, "vkGetPhysicalDeviceProperties2"));
        if (!destroy_instance || !enumerate || !props) throw std::runtime_error("provider required Vulkan1.3 functions absent");
        uint32_t count = 0;
        vr = enumerate(vk, &count, nullptr);
        std::vector<VkPhysicalDevice> physical(count);
        if (vr != VK_SUCCESS || count != 1) { destroy_instance(vk, nullptr); throw std::runtime_error("expected one actual Turnip physical device"); }
        vr = enumerate(vk, &count, physical.data());
        if (vr != VK_SUCCESS) { destroy_instance(vk, nullptr); throw std::runtime_error("enumerate physical devices failed"); }
        VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        props(physical[0], &properties);
        vkprops = properties.properties;
        vk_driver_id = driver.driverID;
        destroy_instance(vk, nullptr);
#if defined(FP_GPU_SYSTEM)
        if (driver.driverID != VK_DRIVER_ID_ARM_PROPRIETARY || vkprops.vendorID != 0x13b5
            || !strstr(vkprops.deviceName,"Mali-G615") || vkprops.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            throw std::runtime_error("provider is not native Mali hardware");
#else
        if (driver.driverID != VK_DRIVER_ID_MESA_TURNIP || vkprops.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            throw std::runtime_error("provider is not native Turnip hardware");
#endif
        setenv("DXVK_WSI_DRIVER", "Android", 1);
        setenv("DXVK_LOG_PATH", "none", 1);
        setenv("DXVK_STATE_CACHE", "disable", 1);
        void* lib = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
        if (!lib) throw std::runtime_error(dlerror());
        using Create = HRESULT (*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
            const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        auto create = reinterpret_cast<Create>(dlsym(lib, "D3D11CreateDevice"));
        if (!create) throw std::runtime_error("D3D11CreateDevice export missing");
        Ref<ID3D11Device> device;
        Ref<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
        check("D3D11CreateDevice_hardware_FL11_0", create(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, &requested, 1, D3D11_SDK_VERSION, &device.p, &chosen, &context.p));
        if (chosen != requested) throw std::runtime_error("requested FL11_0 not returned");
#if defined(FP_GPU_SYSTEM)
        ID3D11GeometryShader* unsupported_gs=reinterpret_cast<ID3D11GeometryShader*>(uintptr_t(1));
        HRESULT unsupported_hr=device->CreateGeometryShaderWithStreamOutput(nullptr,0,nullptr,0,nullptr,0,
            D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&unsupported_gs);
        if(unsupported_hr!=DXGI_ERROR_INVALID_CALL || unsupported_gs)
            throw std::runtime_error("unsupported geometry streams escaped the device API gate");
        D3D11_RASTERIZER_DESC wire={};wire.FillMode=D3D11_FILL_WIREFRAME;wire.CullMode=D3D11_CULL_NONE;wire.DepthClipEnable=true;
        Ref<ID3D11RasterizerState> unavailable_raster;
        if(device->CreateRasterizerState(&wire,&unavailable_raster.p)!=E_INVALIDARG || unavailable_raster.p)
            throw std::runtime_error("unsupported wireframe escaped the device API gate");
        D3D11_BLEND_DESC blend={};auto& rt=blend.RenderTarget[0];rt.BlendEnable=true;
        rt.SrcBlend=D3D11_BLEND_SRC1_COLOR;rt.DestBlend=D3D11_BLEND_ZERO;rt.BlendOp=D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;Ref<ID3D11BlendState> unavailable_blend;
        if(device->CreateBlendState(&blend,&unavailable_blend.p)!=E_INVALIDARG || unavailable_blend.p)
            throw std::runtime_error("unsupported dual-source blend escaped the device API gate");
        unsupported_operations_rejected=true;
#endif
        Ref<IDXGIDevice> dxgi_device;
        Ref<IDXGIAdapter> adapter;
        check("QueryInterface_IDXGIDevice", device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device.p)));
        check("GetAdapter", dxgi_device->GetAdapter(&adapter.p));
        DXGI_ADAPTER_DESC desc = {};
        check("GetDesc", adapter->GetDesc(&desc));
        for (wchar_t c : desc.Description) { if (!c) break; adapter_name += c < 128 ? char(c) : '?'; }
        Ref<IDXGIOutput> absent,output;
        HRESULT no_policy=adapter->EnumOutputs(0,&absent.p);steps.push_back({"EnumOutputs_no_policy",no_policy});
        no_policy_no_outputs=no_policy==DXGI_ERROR_NOT_FOUND&&!absent.p;
        if(!no_policy_no_outputs)throw std::runtime_error("Offscreen no-policy caller exposed a monitor");
        display_enabled.store(true,std::memory_order_release);
        check("EnumOutputs_explicit_virtual_display",adapter->EnumOutputs(0,&output.p));
        check("GetDesc_virtual_display",output->GetDesc(&output_desc));
        for(WCHAR c:output_desc.DeviceName){if(!c)break;output_name+=c<128?char(c):'?';}
        if(output_name!="\\\\.\\DISPLAY1"||reinterpret_cast<uintptr_t>(output_desc.Monitor)!=1||!output_desc.AttachedToDesktop
           ||output_desc.DesktopCoordinates.left||output_desc.DesktopCoordinates.top||output_desc.DesktopCoordinates.right!=960||output_desc.DesktopCoordinates.bottom!=540)
            throw std::runtime_error("Virtual monitor name/identity/bounds mismatch");
        UINT modes=0;check("GetDisplayModeList_count",output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM,0,&modes,nullptr));
        if(modes!=1)throw std::runtime_error("Expected one explicit virtual display mode");
        check("GetDisplayModeList_mode",output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM,0,&modes,&output_mode));
        if(modes!=1||output_mode.Width!=960||output_mode.Height!=540||output_mode.RefreshRate.Numerator!=60||output_mode.RefreshRate.Denominator!=1
           ||output_mode.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||output_mode.ScanlineOrdering!=DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE)
            throw std::runtime_error("Virtual mode width/height/refresh/format mismatch");
        DXGI_MODE_DESC closest={},request={};request.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        check("FindClosestMatchingMode_current",output->FindClosestMatchingMode(&request,&closest,device.p));
        if(closest.Width!=960||closest.Height!=540||closest.RefreshRate.Numerator!=60||closest.RefreshRate.Denominator!=1)
            throw std::runtime_error("Virtual current display mode mismatch");
        Ref<IDXGIOutput> extra;HRESULT next=adapter->EnumOutputs(1,&extra.p);steps.push_back({"EnumOutputs_end",next});
        if(next!=DXGI_ERROR_NOT_FOUND||extra.p)throw std::runtime_error("Virtual display exposed an extra monitor");
        display_policy_pass=true;
        check("CheckFormatSupport_sampled_texture", device->CheckFormatSupport(sampled_format, &sampled_format_support));
        const UINT required_support = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        if ((sampled_format_support & required_support) != required_support) throw std::runtime_error("texture sampling unsupported");
        Ref<ID3D11VertexShader> vs;
        Ref<ID3D11PixelShader> ps;
        check("CreateVertexShader_DXBC_SM5", device->CreateVertexShader(native_vs, sizeof(native_vs), nullptr, &vs.p));
        check("CreatePixelShader_DXBC_SM5", device->CreatePixelShader(native_ps, sizeof(native_ps), nullptr, &ps.p));
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1;
        td.Format = sampled_format; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {source_pixels, 16, sizeof(source_pixels)};
        Ref<ID3D11Texture2D> texture, target, staging;
        Ref<ID3D11ShaderResourceView> srv;
        Ref<ID3D11RenderTargetView> rtv;
        Ref<ID3D11SamplerState> sampler;
        Ref<ID3D11RasterizerState> raster;
        check("CreateTexture2D_sampled_texture", device->CreateTexture2D(&td, &init, &texture.p));
        check("CreateShaderResourceView_sampled_texture", device->CreateShaderResourceView(texture.p, nullptr, &srv.p));
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        check("CreateTexture2D_RGBA8_RT", device->CreateTexture2D(&td, nullptr, &target.p));
        check("CreateRenderTargetView", device->CreateRenderTargetView(target.p, nullptr, &rtv.p));
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check("CreateTexture2D_staging", device->CreateTexture2D(&td, nullptr, &staging.p));
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER; sd.MaxAnisotropy = 1;
        check("CreateSamplerState", device->CreateSamplerState(&sd, &sampler.p));
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = true;
        check("CreateRasterizerState", device->CreateRasterizerState(&rd, &raster.p));
        D3D11_VIEWPORT vp = {0, 0, 4, 4, 0, 1};
        const float clear[4] = {0, 0, 1, 0};
        context->ClearRenderTargetView(rtv.p, clear);
        context->OMSetRenderTargets(1, &rtv.p, nullptr);
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster.p);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.p, nullptr, 0);
        context->PSSetShader(ps.p, nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv.p);
        context->PSSetSamplers(0, 1, &sampler.p);
        context->Draw(3, 0);
        context->CopyResource(staging.p, target.p);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        check("Map_staging_after_Draw_CopyResource", context->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped));
        if (!mapped.pData || mapped.RowPitch < 16) {
            context->Unmap(staging.p, 0);
            throw std::runtime_error("invalid staging map pointer/pitch");
        }
        for (unsigned y = 0; y < 4; ++y) {
            memcpy(pixels + 16*y, static_cast<unsigned char*>(mapped.pData) + y*mapped.RowPitch, 16);
            for (unsigned x = 0; x < 4; ++x) {
                const unsigned char* p = pixels + (y*4+x)*4;
                ++checked;
                if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) ++matched;
            }
        }
        context->Unmap(staging.p, 0);
        check("GetDeviceRemovedReason", device->GetDeviceRemovedReason());
        context->ClearState(); context->Flush();
        if (matched != 16) throw std::runtime_error("DXBC sample/readback pixel mismatch");
        success = true;
    } catch (const std::exception& e) { error = e.what(); }
    printf("{\"success\":%s,\"native_aarch64\":true,\"wine_box64\":false,\"feature_level\":%u,\"adapter\":%s,\"provider_device\":%s,\"provider_driver_id\":%u,\"provider_api_version\":%u,\"sampled_format_support\":%u,\"checked_pixels\":%u,\"matched_pixels\":%u,\"pixels_rgba8\":[",
        success ? "true" : "false", unsigned(chosen), json(adapter_name).c_str(), json(vkprops.deviceName).c_str(),
        vk_driver_id, vkprops.apiVersion, sampled_format_support, checked, matched);
    for (unsigned i = 0; i < 64; ++i) printf("%s%u", i ? "," : "", pixels[i]);
    printf("],\"steps\":[");
    for (size_t i = 0; i < steps.size(); ++i) printf("%s{\"step\":%s,\"hresult\":%u}",
        i ? "," : "", json(steps[i].name).c_str(), unsigned(steps[i].hr));
    printf("],\"unsupported_operations_rejected\":%s,\"no_policy_no_outputs\":%s,\"explicit_virtual_display_pass\":%s,\"virtual_display_name\":%s,\"virtual_monitor\":%llu,\"virtual_width\":%u,\"virtual_height\":%u,\"virtual_refresh_numerator\":%u,\"virtual_refresh_denominator\":%u,\"error\":%s,\"game_rendered\":false,\"fps_measured\":false}\n",
        unsupported_operations_rejected?"true":"false",no_policy_no_outputs?"true":"false",display_policy_pass?"true":"false",json(output_name).c_str(),(unsigned long long)reinterpret_cast<uintptr_t>(output_desc.Monitor),output_mode.Width,output_mode.Height,
        output_mode.RefreshRate.Numerator,output_mode.RefreshRate.Denominator,json(error).c_str());
    return success ? 0 : 1;
}
