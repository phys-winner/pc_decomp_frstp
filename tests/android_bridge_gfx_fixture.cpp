/* Tests the exact candidate observation helpers, no GPU or game execution. */
#include <d3d11_4.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include <cstdlib>

static std::recursive_mutex mutex;
#include "gfx_diagnostics.inc"

/* Inherited methods deduce the same base member-pointer type in invoke_impl. */
static_assert(std::is_same_v<decltype(&ID3D11Device::CreateTexture2D),decltype(&ID3D11Device1::CreateTexture2D)>);
static_assert(std::is_same_v<decltype(&ID3D11Device::CreateTexture2D),decltype(&ID3D11Device2::CreateTexture2D)>);
static_assert(std::is_same_v<decltype(&ID3D11Device::CreateTexture2D),decltype(&ID3D11Device3::CreateTexture2D)>);
static_assert(std::is_same_v<decltype(&ID3D11Device::CreateTexture2D),decltype(&ID3D11Device4::CreateTexture2D)>);
static_assert(std::is_same_v<decltype(&ID3D11Device::CreateTexture2D),decltype(&ID3D11Device5::CreateTexture2D)>);

static unsigned checks;
static void require(bool ok) { ++checks;if(!ok)std::abort(); }
class Texture final:public ID3D11Texture2D {
public:
    D3D11_TEXTURE2D_DESC desc={960,540,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};
    std::atomic<ULONG> refs{1},queries{0},descriptions{0};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        ++queries;errno=ERANGE;feraiseexcept(FE_INVALID|FE_INEXACT);
        if(!result)return E_POINTER;*result=nullptr;
        if(iid!=__uuidof(ID3D11Texture2D)&&iid!=__uuidof(IUnknown))return E_NOINTERFACE;
        *result=static_cast<ID3D11Texture2D*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { errno=ERANGE;return --refs; }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device**) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override { std::abort(); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,const IUnknown*) override { std::abort(); }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION*) override { std::abort(); }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override { std::abort(); }
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { std::abort(); }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* out) override { ++descriptions;*out=desc;errno=ERANGE;feraiseexcept(FE_INVALID|FE_INEXACT); }
};
class NonTexture final:public IUnknown {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** result) override { *result=nullptr;errno=ERANGE;feraiseexcept(FE_INVALID);return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { std::abort(); }
    ULONG STDMETHODCALLTYPE Release() override { std::abort(); }
};
static void preserved() {
    errno=EDOM;fesetround(FE_DOWNWARD);feclearexcept(FE_ALL_EXCEPT);feraiseexcept(FE_DIVBYZERO);
}
static void check_preserved() { require(errno==EDOM);require(fetestexcept(FE_ALL_EXCEPT)==FE_DIVBYZERO);require(fegetround()==FE_DOWNWARD); }
int main() {
    Texture texture;NonTexture other;
    preserved();gfx_observe_backbuffer(&other,0);check_preserved();require(!gfx_backbuffer_count);
    preserved();gfx_observe_backbuffer(&texture,0);check_preserved();require(texture.refs==1);require(gfx_backbuffer_count==1);
    for(unsigned i=0;i<1000;++i)gfx_observe_backbuffer(&texture,0);
    require(gfx_backbuffer_count==1);require(texture.refs==1);
    for(unsigned i=1;i<20;++i){texture.desc.Width=960+i;gfx_observe_backbuffer(&texture,0);}
    require(gfx_backbuffer_count==16);require(texture.refs==1);
    texture.desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;gfx_observe_created_texture(&texture);require(!gfx_texture_count);
    texture.desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;texture.desc.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;
    preserved();gfx_observe_created_texture(&texture);check_preserved();require(gfx_texture_count==1);
    for(unsigned i=0;i<1000;++i)gfx_observe_created_texture(&texture);require(gfx_texture_count==1);
    std::vector<std::thread> workers;
    for(unsigned worker=0;worker<4;++worker)workers.emplace_back([worker]{
        Texture current;for(unsigned i=0;i<80;++i){current.desc.Width=1000+worker*100+i;gfx_observe_created_texture(&current);}
    });
    for(auto& thread:workers)thread.join();require(gfx_texture_count==64);
    require(gfx_textures[0].Width==979);require(gfx_textures[0].BindFlags==D3D11_BIND_DEPTH_STENCIL);
    printf("{\"success\":true,\"checks\":%u,\"backbuffer_descriptors\":16,\"rt_ds_descriptors\":64,\"concurrent_workers\":4,\"native_error_fp_preserved\":true,\"gpu_executed\":false}\n",checks);
    return 0;
}
