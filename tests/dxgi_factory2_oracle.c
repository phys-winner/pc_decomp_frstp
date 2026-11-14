/* Own native Windows DXGI objects only; no original code or visible window. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <stdint.h>
#include <dxgi1_2.h>

_Static_assert(sizeof(DXGI_SWAP_CHAIN_DESC1)==48,"DESC1 size");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,Width)==0,"Width offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,Height)==4,"Height offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,Format)==8,"Format offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,Stereo)==12,"Stereo offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,SampleDesc)==16,"SampleDesc offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,BufferUsage)==24,"BufferUsage offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,BufferCount)==28,"BufferCount offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,Scaling)==32,"Scaling offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,SwapEffect)==36,"SwapEffect offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,AlphaMode)==40,"AlphaMode offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_DESC1,Flags)==44,"Flags offset");
_Static_assert(sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC)==20,"FULLSCREEN size");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC,RefreshRate)==0,"RefreshRate offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC,ScanlineOrdering)==8,"ScanlineOrdering offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC,Scaling)==12,"Fullscreen Scaling offset");
_Static_assert(offsetof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC,Windowed)==16,"Windowed offset");
_Static_assert(offsetof(IDXGIFactory2Vtbl,EnumAdapters1)==12*8,"Factory2 EnumAdapters1 slot");
_Static_assert(offsetof(IDXGIFactory2Vtbl,IsCurrent)==13*8,"Factory2 IsCurrent slot");
_Static_assert(offsetof(IDXGIFactory2Vtbl,CreateSwapChainForHwnd)==15*8,"Factory2 CreateForHwnd slot");
_Static_assert(offsetof(IDXGIFactory2Vtbl,CreateSwapChainForCoreWindow)==16*8,"Factory2 CoreWindow slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl,GetDevice)==7*8,"SwapChain1 GetDevice slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl,Present)==8*8,"SwapChain1 Present slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl,GetBuffer)==9*8,"SwapChain1 GetBuffer slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl,GetDesc)==12*8,"SwapChain1 GetDesc slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl,GetDesc1)==18*8,"SwapChain1 GetDesc1 slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl,GetHwnd)==20*8,"SwapChain1 GetHwnd slot");

#ifdef FROSTPUNK_LAYOUT_ONLY
int frostpunk_dxgi_factory2_layout_verified;
#else
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

static const GUID unknown_iid={0xfbe33ae1,0x6f09,0x4598,{0x91,0x31,0x44,0x01,0x08,0xb7,0xf4,0x01}};
static const GUID expected_factory2_iid={0x50c83a1c,0xe072,0x4c48,{0x87,0xb0,0x36,0x30,0xfa,0x36,0xa6,0xd0}};
static const GUID expected_chain1_iid={0x790a45f7,0x0d42,0x4876,{0x98,0x3a,0x0a,0x55,0xcf,0xe6,0xf4,0xaa}};
#define RELEASE(p) do{if(p){(p)->lpVtbl->Release(p);(p)=NULL;}}while(0)
#define REQUIRE(test) do{if(!(test)){failure=__LINE__;goto cleanup;}}while(0)

int main(void){
    IDXGIFactory1 *factory1=NULL;IDXGIFactory2 *factory2=NULL,*parent_factory=NULL;
    IUnknown *factory_identity1=NULL,*factory_identity2=NULL,*chain_identity1=NULL,*chain_identity2=NULL;
    ID3D11Device *device=NULL,*chain_device=NULL;ID3D11DeviceContext *context=NULL;
    IDXGIDevice2 *dxgi_device=NULL;IDXGIAdapter *adapter=NULL;
    IDXGISwapChain1 *chain=NULL,*chain_back=NULL;IDXGISwapChain *chain_base=NULL;ID3D11Texture2D *texture=NULL;
    HWND hwnd=NULL,returned_hwnd=NULL;D3D_FEATURE_LEVEL chosen=0,level=D3D_FEATURE_LEVEL_11_0;
    HRESULT factory_hr=E_FAIL,factory_qi_hr=E_FAIL,factory_unknown_hr=E_FAIL;
    HRESULT device_hr=E_FAIL,dxgi_hr=E_FAIL,adapter_hr=E_FAIL,parent_hr=E_FAIL;
    HRESULT create_hr=E_FAIL,null_hwnd_hr=E_FAIL,association_hr=E_FAIL,desc1_hr=E_FAIL,desc_hr=E_FAIL;
    HRESULT hwnd_hr=E_FAIL,get_device_hr=E_FAIL,buffer_hr=E_FAIL,bad_buffer_hr=E_FAIL,chain_unknown_hr=E_FAIL,chain_back_hr=E_FAIL;
    ULONG factory_added=0,factory_released=0,chain_added=0,chain_released=0;
    int failure=0,window_hidden=0,factory_identity=0,chain_identity=0,unknown_null=0,bad_buffer_null=0;
    DXGI_SWAP_CHAIN_DESC1 desc={0},actual_desc={0};DXGI_SWAP_CHAIN_DESC legacy_desc={0};D3D11_TEXTURE2D_DESC texture_desc={0};
    void *unknown=(void *)(uintptr_t)UINT64_C(0x1122334455667788);
    REQUIRE(memcmp(&IID_IDXGIFactory2,&expected_factory2_iid,sizeof(GUID))==0&&memcmp(&IID_IDXGISwapChain1,&expected_chain1_iid,sizeof(GUID))==0);
    factory_hr=CreateDXGIFactory1(&IID_IDXGIFactory1,(void **)&factory1);REQUIRE(factory_hr==S_OK&&factory1);
    factory_qi_hr=factory1->lpVtbl->QueryInterface(factory1,&IID_IDXGIFactory2,(void **)&factory2);REQUIRE(factory_qi_hr==S_OK&&factory2);
    REQUIRE(factory1->lpVtbl->QueryInterface(factory1,&IID_IUnknown,(void **)&factory_identity1)==S_OK);
    REQUIRE(factory2->lpVtbl->QueryInterface(factory2,&IID_IUnknown,(void **)&factory_identity2)==S_OK);
    factory_identity=factory_identity1==factory_identity2;REQUIRE(factory_identity);
    factory_added=factory2->lpVtbl->AddRef(factory2);factory_released=factory2->lpVtbl->Release(factory2);
    factory_unknown_hr=factory2->lpVtbl->QueryInterface(factory2,&unknown_iid,&unknown);unknown_null=unknown==NULL;
    REQUIRE(factory_unknown_hr==E_NOINTERFACE&&unknown_null);
    device_hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_WARP,NULL,D3D11_CREATE_DEVICE_BGRA_SUPPORT,&level,1,D3D11_SDK_VERSION,&device,&chosen,&context);
    REQUIRE(device_hr==S_OK&&device&&context&&chosen==D3D_FEATURE_LEVEL_11_0);
    dxgi_hr=device->lpVtbl->QueryInterface(device,&IID_IDXGIDevice2,(void **)&dxgi_device);REQUIRE(dxgi_hr==S_OK&&dxgi_device);
    adapter_hr=dxgi_device->lpVtbl->GetAdapter(dxgi_device,&adapter);REQUIRE(adapter_hr==S_OK&&adapter);
    parent_hr=adapter->lpVtbl->GetParent(adapter,&IID_IDXGIFactory2,(void **)&parent_factory);REQUIRE(parent_hr==S_OK&&parent_factory);
    hwnd=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"Frostpunk private DXGI ABI oracle",WS_OVERLAPPEDWINDOW,0,0,960,540,NULL,NULL,GetModuleHandleW(NULL),NULL);
    REQUIRE(hwnd!=NULL);window_hidden=!IsWindowVisible(hwnd);REQUIRE(window_hidden);
    desc.Width=960;desc.Height=540;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT|DXGI_USAGE_SHADER_INPUT;desc.BufferCount=2;
    desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    null_hwnd_hr=parent_factory->lpVtbl->CreateSwapChainForHwnd(parent_factory,(IUnknown *)device,NULL,&desc,NULL,NULL,&chain);
    REQUIRE(FAILED(null_hwnd_hr)&&chain==NULL);
    create_hr=parent_factory->lpVtbl->CreateSwapChainForHwnd(parent_factory,(IUnknown *)device,hwnd,&desc,NULL,NULL,&chain);
    REQUIRE(create_hr==S_OK&&chain);
    association_hr=parent_factory->lpVtbl->MakeWindowAssociation(parent_factory,hwnd,DXGI_MWA_NO_WINDOW_CHANGES);REQUIRE(association_hr==S_OK);
    REQUIRE(chain->lpVtbl->QueryInterface(chain,&IID_IDXGISwapChain,(void **)&chain_base)==S_OK&&chain_base);
    chain_back_hr=chain_base->lpVtbl->QueryInterface(chain_base,&IID_IDXGISwapChain1,(void **)&chain_back);REQUIRE(chain_back_hr==S_OK&&chain_back==chain);
    REQUIRE(chain->lpVtbl->QueryInterface(chain,&IID_IUnknown,(void **)&chain_identity1)==S_OK);
    REQUIRE(chain_base->lpVtbl->QueryInterface(chain_base,&IID_IUnknown,(void **)&chain_identity2)==S_OK);
    chain_identity=chain_identity1==chain_identity2;REQUIRE(chain_identity);
    chain_added=chain->lpVtbl->AddRef(chain);chain_released=chain->lpVtbl->Release(chain);
    unknown=(void *)(uintptr_t)UINT64_C(0x1122334455667788);
    chain_unknown_hr=chain->lpVtbl->QueryInterface(chain,&unknown_iid,&unknown);REQUIRE(chain_unknown_hr==E_NOINTERFACE&&unknown==NULL);
    desc1_hr=chain->lpVtbl->GetDesc1(chain,&actual_desc);REQUIRE(desc1_hr==S_OK&&memcmp(&desc,&actual_desc,sizeof(desc))==0);
    desc_hr=chain->lpVtbl->GetDesc(chain,&legacy_desc);REQUIRE(desc_hr==S_OK&&legacy_desc.OutputWindow==hwnd&&legacy_desc.BufferDesc.Width==960&&legacy_desc.BufferDesc.Height==540);
    hwnd_hr=chain->lpVtbl->GetHwnd(chain,&returned_hwnd);REQUIRE(hwnd_hr==S_OK&&returned_hwnd==hwnd);
    get_device_hr=chain->lpVtbl->GetDevice(chain,&IID_ID3D11Device,(void **)&chain_device);REQUIRE(get_device_hr==S_OK&&chain_device==device);
    buffer_hr=chain->lpVtbl->GetBuffer(chain,0,&IID_ID3D11Texture2D,(void **)&texture);REQUIRE(buffer_hr==S_OK&&texture);
    texture->lpVtbl->GetDesc(texture,&texture_desc);REQUIRE(texture_desc.Width==960&&texture_desc.Height==540&&texture_desc.Format==desc.Format);
    unknown=(void *)(uintptr_t)UINT64_C(0x1122334455667788);
    bad_buffer_hr=chain->lpVtbl->GetBuffer(chain,2,&IID_ID3D11Texture2D,&unknown);bad_buffer_null=unknown==NULL;
    REQUIRE(FAILED(bad_buffer_hr)&&bad_buffer_null);
cleanup:
    RELEASE(texture);RELEASE(chain_device);RELEASE(chain_identity2);RELEASE(chain_identity1);RELEASE(chain_back);RELEASE(chain_base);RELEASE(chain);
    if(hwnd){if(!DestroyWindow(hwnd)&&!failure)failure=__LINE__;hwnd=NULL;}
    RELEASE(parent_factory);RELEASE(adapter);RELEASE(dxgi_device);RELEASE(context);RELEASE(device);
    RELEASE(factory_identity2);RELEASE(factory_identity1);RELEASE(factory2);RELEASE(factory1);
    printf("{\"success\":%s,\"failure_line\":%d,\"driver\":\"WARP explicit isolated Windows oracle\",\"feature_level\":%u,\"own_window_hidden\":%s,\"Present_executed\":false,",failure?"false":"true",failure,(unsigned)chosen,window_hidden?"true":"false");
    printf("\"layouts\":{\"DESC1\":48,\"DESC1_offsets\":[0,4,8,12,16,24,28,32,36,40,44],\"FULLSCREEN\":20,\"FULLSCREEN_offsets\":[0,8,12,16],\"Factory2_CreateForHwnd_slot\":15,\"SwapChain1_Present_slot\":8,\"SwapChain1_GetBuffer_slot\":9},");
    printf("\"hr\":{\"CreateFactory1\":%u,\"QI_Factory2\":%u,\"Factory_unknown_QI\":%u,\"CreateDevice\":%u,\"QI_Device2\":%u,\"GetAdapter\":%u,\"GetParent_Factory2\":%u,\"CreateForHwnd_NULL\":%u,\"CreateForHwnd\":%u,\"MakeWindowAssociation\":%u,\"GetDesc1\":%u,\"GetDesc\":%u,\"GetHwnd\":%u,\"GetDevice\":%u,\"GetBuffer0\":%u,\"GetBuffer2\":%u,\"Chain_unknown_QI\":%u},",(unsigned)factory_hr,(unsigned)factory_qi_hr,(unsigned)factory_unknown_hr,(unsigned)device_hr,(unsigned)dxgi_hr,(unsigned)adapter_hr,(unsigned)parent_hr,(unsigned)null_hwnd_hr,(unsigned)create_hr,(unsigned)association_hr,(unsigned)desc1_hr,(unsigned)desc_hr,(unsigned)hwnd_hr,(unsigned)get_device_hr,(unsigned)buffer_hr,(unsigned)bad_buffer_hr,(unsigned)chain_unknown_hr);
    printf("\"identity\":{\"Factory2_IID\":\"50c83a1c-e072-4c48-87b0-3630fa36a6d0\",\"SwapChain1_IID\":\"790a45f7-0d42-4876-983a-0a55cfe6f4aa\",\"Factory1_Factory2_IUnknown_same\":%s,\"SwapChain_SwapChain1_IUnknown_same\":%s,\"base_QI_SwapChain1_hr\":%u,\"unknown_QI_output_NULL\":%s,\"bad_buffer_output_NULL\":%s},\"refcounts_observed\":{\"factory_AddRef\":%lu,\"factory_Release\":%lu,\"chain_AddRef\":%lu,\"chain_Release\":%lu},",factory_identity?"true":"false",chain_identity?"true":"false",(unsigned)chain_back_hr,unknown_null?"true":"false",bad_buffer_null?"true":"false",factory_added,factory_released,chain_added,chain_released);
    printf("\"desc1\":{\"Width\":%u,\"Height\":%u,\"Format\":%u,\"Stereo\":%u,\"SampleDesc\":[%u,%u],\"BufferUsage\":%u,\"BufferCount\":%u,\"Scaling\":%u,\"SwapEffect\":%u,\"AlphaMode\":%u,\"Flags\":%u}}\n",actual_desc.Width,actual_desc.Height,(unsigned)actual_desc.Format,(unsigned)actual_desc.Stereo,actual_desc.SampleDesc.Count,actual_desc.SampleDesc.Quality,actual_desc.BufferUsage,actual_desc.BufferCount,(unsigned)actual_desc.Scaling,(unsigned)actual_desc.SwapEffect,(unsigned)actual_desc.AlphaMode,actual_desc.Flags);
    return failure?1:0;
}
#endif
