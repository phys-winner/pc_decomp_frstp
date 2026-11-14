/* Measure successful native DXGI Present calls, including the real call time. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <stdio.h>
#include <stdlib.h>
#include "performance.h"

static uint64_t present_target;
static LARGE_INTEGER frequency,first,last,window_start;
static unsigned long long total,window_frames;
static double maximum_ms;
static SRWLOCK lock=SRWLOCK_INIT;
static LARGE_INTEGER next_capture;
static unsigned capture_count;
static wchar_t capture_directory[32768];
static const GUID texture_iid={0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
static const GUID info_queue_iid={0x6543dbb6,0x1b48,0x42f5,{0xab,0x82,0xe9,0x7e,0xc7,0x43,0x26,0xf6}};

static void inspect_debug_device(ID3D11Device *device) {
    ID3D11InfoQueue *queue=NULL;
    if(FAILED(ID3D11Device_QueryInterface(device,&info_queue_iid,(void **)&queue)))return;
    UINT64 count=ID3D11InfoQueue_GetNumStoredMessagesAllowedByRetrievalFilter(queue);
    unsigned errors=0,warnings=0,logged=0;
    for(UINT64 i=0;i<count && i<2048;++i) {
        SIZE_T bytes=0;
        if(FAILED(ID3D11InfoQueue_GetMessage(queue,i,NULL,&bytes)) || bytes<sizeof(D3D11_MESSAGE) || bytes>(1u<<20))continue;
        SIZE_T allocated=bytes;D3D11_MESSAGE *message=malloc(allocated);if(!message)break;
        if(SUCCEEDED(ID3D11InfoQueue_GetMessage(queue,i,message,&bytes)) && bytes<=allocated && message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
            if(message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR)++errors;else ++warnings;
            unsigned limit=message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR?64:16;
            unsigned number=message->Severity<=D3D11_MESSAGE_SEVERITY_ERROR?errors:warnings;
            uintptr_t text=(uintptr_t)message->pDescription,end=(uintptr_t)message+allocated;
            if(number<=limit && text>=(uintptr_t)message && text<=end && message->DescriptionByteLength<=end-text) {
                unsigned length=message->DescriptionByteLength>4096?4096:(unsigned)message->DescriptionByteLength;
                fprintf(stderr,"NATIVE_D3D11_DEBUG severity=%u id=%u %.*s\n",
                    (unsigned)message->Severity,(unsigned)message->ID,(int)length,message->pDescription);
                ++logged;
            }
        }
        free(message);
    }
    fprintf(stderr,"NATIVE_D3D11_INFO_QUEUE available=1 messages=%llu errors=%u warnings=%u logged=%u inspected_limit=2048\n",
        count,errors,warnings,logged);
    ID3D11InfoQueue_ClearStoredMessages(queue);ID3D11InfoQueue_Release(queue);
}

/* Read the application's backbuffer before Present can discard its contents. */
static __declspec(noinline) void capture(IDXGISwapChain *swap,unsigned number) {
    IDXGISwapChain *chain=swap;ID3D11Texture2D *source=NULL,*staging=NULL;
    ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
    HANDLE file=INVALID_HANDLE_VALUE;unsigned char *row=NULL;int mapped=0,ok=0,nonblack=0;
    wchar_t path[32768]={0};DXGI_SWAP_CHAIN_DESC swap_desc;
    HRESULT result=IDXGISwapChain_GetDesc(chain,&swap_desc);
    DWORD owner=0;
    if(FAILED(result) || !GetWindowThreadProcessId(swap_desc.OutputWindow,&owner) || owner!=GetCurrentProcessId())goto done;
    result=IDXGISwapChain_GetBuffer(chain,0,&texture_iid,(void **)&source);
    if(FAILED(result))goto done;
    D3D11_TEXTURE2D_DESC desc;ID3D11Texture2D_GetDesc(source,&desc);
    int rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    int bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if((!rgba && !bgra) || !desc.Width || !desc.Height || desc.Width>16384 || desc.Height>16384 || desc.SampleDesc.Count!=1)goto done;
    ID3D11Texture2D_GetDevice(source,&device);ID3D11Device_GetImmediateContext(device,&context);
    inspect_debug_device(device);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    result=ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging);
    if(FAILED(result))goto done;
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)source);
    D3D11_MAPPED_SUBRESOURCE pixels;
    result=ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&pixels);
    if(FAILED(result))goto done;
    mapped=1;
    if(pixels.RowPitch<desc.Width*4u || !pixels.pData)goto done;
    row=malloc((size_t)desc.Width*4);if(!row)goto done;
    swprintf_s(path,32768,L"%ls\\present-%lu-%u.bmp",capture_directory,GetCurrentProcessId(),number);
    file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)goto done;
    BITMAPFILEHEADER header={0};BITMAPINFOHEADER info={0};DWORD written;
    header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);
    header.bfSize=header.bfOffBits+desc.Width*desc.Height*4u;
    info.biSize=sizeof(info);info.biWidth=(LONG)desc.Width;info.biHeight=-(LONG)desc.Height;
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    if(!WriteFile(file,&header,sizeof(header),&written,NULL) || written!=sizeof(header) ||
       !WriteFile(file,&info,sizeof(info),&written,NULL) || written!=sizeof(info))goto done;
    for(unsigned y=0;y<desc.Height;++y) {
        const unsigned char *input=(const unsigned char *)pixels.pData+(size_t)y*pixels.RowPitch;
        for(unsigned x=0;x<desc.Width;++x) {
            row[4*x]=input[4*x+(rgba?2:0)];row[4*x+1]=input[4*x+1];
            row[4*x+2]=input[4*x+(rgba?0:2)];row[4*x+3]=255;
            nonblack|=row[4*x]|row[4*x+1]|row[4*x+2];
        }
        if(!WriteFile(file,row,desc.Width*4,&written,NULL) || written!=desc.Width*4)goto done;
    }
    ok=1;
    char utf8[32768];int count=WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,sizeof(utf8),NULL,NULL);
    fprintf(stderr,"NATIVE_BACKBUFFER_CAPTURE path=%s width=%u height=%u all_black=%d owner_pid=%lu before_Present=1 gameplay_proven=0\n",
        count?utf8:"<path conversion failed>",desc.Width,desc.Height,!nonblack,owner);
done:
    if(file!=INVALID_HANDLE_VALUE){CloseHandle(file);if(!ok)DeleteFileW(path);}
    free(row);
    if(mapped)ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
    if(staging)ID3D11Texture2D_Release(staging);
    if(context)ID3D11DeviceContext_Release(context);
    if(device)ID3D11Device_Release(device);
    if(source)ID3D11Texture2D_Release(source);
    if(!ok)fprintf(stderr,"NATIVE_BACKBUFFER_CAPTURE unavailable hresult=0x%08lx capture=%u\n",(unsigned long)result,number);
}

int fp_performance_init(void) {
    WNDCLASSW cls={0};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"FrostpunkPresentIdentity";
    if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 0;
    HWND window=CreateWindowW(cls.lpszClassName,L"",WS_OVERLAPPED,0,0,16,16,NULL,NULL,cls.hInstance,NULL);
    if(!window)return 0;
    DXGI_SWAP_CHAIN_DESC desc={0};desc.BufferDesc.Width=16;desc.BufferDesc.Height=16;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *swap=NULL;ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
    D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    HRESULT result=D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,&level,1,D3D11_SDK_VERSION,
        &desc,&swap,&device,NULL,&context);
    if(SUCCEEDED(result)) {
        present_target=(uintptr_t)swap->lpVtbl->Present;
        QueryPerformanceFrequency(&frequency);
        wchar_t capture_enabled[2];capture_directory[0]=0;
        if(GetEnvironmentVariableW(L"FROSTPUNK_CAPTURE_BACKBUFFER",capture_enabled,2)==1 && capture_enabled[0]==L'1') {
            DWORD count=GetEnvironmentVariableW(L"FROSTPUNK_CAPTURE_DIRECTORY",capture_directory,32768);
            if(!count || count>32768-128)capture_directory[0]=0;
        }
        IDXGISwapChain_Release(swap);ID3D11DeviceContext_Release(context);ID3D11Device_Release(device);
    }
    DestroyWindow(window);
    fprintf(stderr,"NATIVE_PRESENT_MEASUREMENT available=%d target=0x%llx hardware_D3D11=%d\n",present_target!=0,present_target,SUCCEEDED(result));
    return present_target!=0;
}

void fp_performance_native_begin(const CPU *cpu,uint64_t target) {
    if(!present_target || target!=present_target || !capture_directory[0] || ((uint32_t)cpu->r8&DXGI_PRESENT_TEST))return;
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&lock);
    if(capture_count<32 && now.QuadPart>=next_capture.QuadPart) {
        next_capture.QuadPart=now.QuadPart+frequency.QuadPart*15;
        capture((IDXGISwapChain *)(uintptr_t)cpu->rcx,++capture_count);
    }
    ReleaseSRWLockExclusive(&lock);
}

void fp_performance_native_return(const CPU *cpu,uint64_t target) {
    if(target!=present_target || (uint32_t)cpu->rax!=S_OK || ((uint32_t)cpu->r8&DXGI_PRESENT_TEST))return;
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    AcquireSRWLockExclusive(&lock);
    if(!total) {
        first=last=window_start=now;
        fprintf(stderr,"NATIVE_FIRST_SUCCESSFUL_PRESENT swapchain=0x%llx result=S_OK gameplay_proven=0\n",cpu->rcx);
    }
    else {
        double ms=(double)(now.QuadPart-last.QuadPart)*1000.0/frequency.QuadPart;
        if(ms>maximum_ms)maximum_ms=ms;
        ++window_frames;
    }
    ++total;last=now;
    double seconds=(double)(now.QuadPart-window_start.QuadPart)/frequency.QuadPart;
    if(seconds>=1.0) {
        fprintf(stderr,"PRESENT_SAMPLE total=%llu elapsed_s=%.3f fps=%.3f max_frame_ms=%.3f successful_Present_only=1\n",
            total,(double)(now.QuadPart-first.QuadPart)/frequency.QuadPart,window_frames/seconds,maximum_ms);
        window_start=now;window_frames=0;maximum_ms=0;
    }
    ReleaseSRWLockExclusive(&lock);
}
