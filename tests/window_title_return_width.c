#define UNICODE
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
static LRESULT answer;
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_SETTEXT)return answer;return DefWindowProcW(h,m,w,l);}
int main(void){
    WNDCLASSW c={0};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(NULL);c.lpszClassName=L"OwnTitleReturnWidth";
    if(!RegisterClassW(&c))return 2;
    HWND h=CreateWindowExW(0,c.lpszClassName,L"Initial",0,0,0,1,1,NULL,NULL,c.hInstance,NULL);if(!h)return 3;
    uint64_t values[]={0,1,2,UINT64_C(0x100000000),UINT64_C(0xffffffff),UINT64_C(0x12345678ffffffff),UINT64_MAX};
    puts("[");
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i){
        answer=(LRESULT)values[i];SetLastError(0x5a17);errno=1927;BOOL result=SetWindowTextW(h,L"Own probe");DWORD error=GetLastError();int e=errno;
        printf("%s{\"callback_LRESULT_hex\":\"%016llx\",\"BOOL\":%ld,\"LastError\":%lu,\"errno\":%d}",i?",\n":"",(unsigned long long)values[i],(long)result,(unsigned long)error,e);
    }
    puts("\n]");DestroyWindow(h);return UnregisterClassW(c.lpszClassName,c.hInstance)?0:4;
}
