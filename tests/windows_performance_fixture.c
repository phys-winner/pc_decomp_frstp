#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <stdio.h>
#include <string.h>
#include "performance.h"

int main(void) {
    if(!fp_performance_init())return 1;
    HWND window=CreateWindowW(L"STATIC",L"Frostpunk backbuffer fixture",WS_OVERLAPPED,
        0,0,73,41,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!window)return 2;
    DXGI_SWAP_CHAIN_DESC desc={0};desc.BufferDesc.Width=73;desc.BufferDesc.Height=41;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;
    desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *swap=NULL;ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
    ID3D11Texture2D *buffer=NULL;ID3D11RenderTargetView *view=NULL;
    D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    HRESULT result=D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,&level,1,
        D3D11_SDK_VERSION,&desc,&swap,&device,NULL,&context);
    if(FAILED(result))return 3;
    const GUID iid={0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
    if(FAILED(IDXGISwapChain_GetBuffer(swap,0,&iid,(void **)&buffer)))return 4;
    if(FAILED(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)buffer,NULL,&view)))return 5;
    const float red[4]={1,0,0,1};ID3D11DeviceContext_ClearRenderTargetView(context,view,red);
    char debug[2];
    if(GetEnvironmentVariableA("FROSTPUNK_TEST_D3D11_DEBUG",debug,sizeof(debug))) {
        const GUID queue_iid={0x6543dbb6,0x1b48,0x42f5,{0xab,0x82,0xe9,0x7e,0xc7,0x43,0x26,0xf6}};
        ID3D11InfoQueue *queue=NULL;
        if(FAILED(ID3D11Device_QueryInterface(device,&queue_iid,(void **)&queue)))return 9;
        ID3D11InfoQueue_AddApplicationMessage(queue,D3D11_MESSAGE_SEVERITY_ERROR,"Frostpunk owned debug queue fixture");
        ID3D11InfoQueue_Release(queue);
    }
    CPU cpu;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=0;cpu.r8=DXGI_PRESENT_TEST;
    CPU before=cpu;uint64_t target=(uintptr_t)swap->lpVtbl->Present;
    fp_performance_native_begin(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 6;
    cpu.rax=S_OK;before=cpu;fp_performance_native_return(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 15;
    cpu.rcx=(uintptr_t)swap;cpu.r8=0;before=cpu;
    fp_performance_native_begin(&cpu,target);
    fp_performance_native_begin(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 7;
    cpu.rax=(uint32_t)E_FAIL;before=cpu;
    fp_performance_native_return(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 10;
    cpu.rax=(uint32_t)IDXGISwapChain_Present(swap,0,0);
    if(cpu.rax!=S_OK)return 8;
    before=cpu;fp_performance_native_return(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 11;
    Sleep(1100);
    fp_performance_native_begin(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 12;
    cpu.rax=(uint32_t)IDXGISwapChain_Present(swap,0,0);
    if(cpu.rax!=S_OK)return 13;
    before=cpu;
    fp_performance_native_return(&cpu,target);
    if(memcmp(&cpu,&before,sizeof(cpu)))return 14;
    ID3D11RenderTargetView_Release(view);ID3D11Texture2D_Release(buffer);
    IDXGISwapChain_Release(swap);ID3D11DeviceContext_Release(context);ID3D11Device_Release(device);
    DestroyWindow(window);
    puts("WINDOWS_BACKBUFFER_FIXTURE passed hardware_D3D11=1 before_Present=1 CPU_preserved=1 successful_Presents=2");
    return 0;
}
