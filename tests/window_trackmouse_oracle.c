/* Native USER32 contract only. The own HWND stays hidden; no input injection. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
_Static_assert(sizeof(TRACKMOUSEEVENT)==24 && offsetof(TRACKMOUSEEVENT,dwHoverTime)==16,"Win64 TRACKMOUSEEVENT");
int main(void) {
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"OwnTrackMouseOracle",WS_POPUP,0,0,960,540,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(!window)return 2;
    struct {DWORD flags,size;HWND hwnd;} cases[]={
        {TME_QUERY,24,(HWND)UINTPTR_MAX},{TME_LEAVE,24,window},{TME_QUERY,24,window},
        {TME_CANCEL|TME_LEAVE,24,window},{TME_QUERY,24,window},
        {TME_LEAVE,23,window},{TME_LEAVE,24,(HWND)UINTPTR_MAX}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        TRACKMOUSEEVENT request;memset(&request,0xa5,sizeof(request));request.cbSize=cases[i].size;
        request.dwFlags=cases[i].flags;request.hwndTrack=cases[i].hwnd;
        SetLastError(0x5a17);errno=2718;BOOL result=TrackMouseEvent(&request);DWORD error=GetLastError();
        unsigned leaves=0;MSG msg;while(PeekMessageW(&msg,window,WM_MOUSELEAVE,WM_MOUSELEAVE,PM_REMOVE)){
            if(msg.wParam||msg.lParam)return 3;++leaves;
        }
        printf("{\"case\":%u,\"result\":%d,\"error\":%lu,\"flags\":%lu,\"hwnd_zero\":%s,\"hover\":%lu,\"padding\":%lu,\"leaves\":%u}\n",
            i,!!result,error,request.dwFlags,request.hwndTrack?"false":"true",request.dwHoverTime,
            *(uint32_t*)((unsigned char*)&request+20),leaves);
    }
    DestroyWindow(window);return 0;
}
