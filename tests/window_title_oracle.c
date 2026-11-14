#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

static int mode,set_calls,get_calls,len_calls;
static int creation_before_length,creation_after_length;
static WCHAR creation_before[32],creation_after[32];
static LPARAM last_set_pointer;

static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_NCCREATE){
        creation_before_length=GetWindowTextLengthW(hwnd);
        GetWindowTextW(hwnd,creation_before,32);
        LRESULT result=DefWindowProcW(hwnd,msg,wp,lp);
        creation_after_length=GetWindowTextLengthW(hwnd);
        GetWindowTextW(hwnd,creation_after,32);
        return result;
    }
    if(msg==WM_SETTEXT){
        ++set_calls;last_set_pointer=lp;
        if(mode==1)return 0;
        if(mode==2)return 2;
        if(mode==3)return (LRESULT)UINT64_C(0x100000000);
        if(mode==4)return -1;
        if(mode==5){SetLastError(1234);errno=2345;return 0;}
    }
    if(msg==WM_GETTEXT)++get_calls;
    if(msg==WM_GETTEXTLENGTH)++len_calls;
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static void hex(const WCHAR *text,unsigned count){
    putchar('"');for(unsigned i=0;i<count;++i)printf("%04x",(unsigned)(uint16_t)text[i]);putchar('"');
}
static void set_row(HWND hwnd,const char *label,int requested_mode,const WCHAR *title){
    mode=requested_mode;int before=set_calls;
    SetLastError(0x5a17);errno=1927;
    BOOL result=SetWindowTextW(hwnd,title);DWORD error=GetLastError();int saved_errno=errno;
    WCHAR stored[32]={0};GetWindowTextW(hwnd,stored,32);
    printf("{\"label\":\"%s\",\"mode\":%d,\"BOOL\":%ld,\"LastError\":%lu,\"errno\":%d,\"WM_SETTEXT_calls\":%d,\"callback_lparam_NULL\":%s,\"stored32\":",label,requested_mode,(long)result,(unsigned long)error,saved_errno,set_calls-before,last_set_pointer==0?"true":"false");
    hex(stored,32);puts("},");
}
static void get_row(HWND hwnd,const char *label,int capacity){
    mode=0;WCHAR out[32];for(unsigned i=0;i<32;++i)out[i]=0xa5a5;
    int before=get_calls;SetLastError(0x5a17);errno=1927;
    int result=GetWindowTextW(hwnd,out,capacity);DWORD error=GetLastError();int saved_errno=errno;
    printf("{\"label\":\"%s\",\"capacity\":%d,\"result\":%d,\"LastError\":%lu,\"errno\":%d,\"WM_GETTEXT_calls\":%d,\"buffer32\":",label,capacity,result,(unsigned long)error,saved_errno,get_calls-before);hex(out,32);puts("},");
}
static void length_row(HWND hwnd,const char *label){
    int before=len_calls;SetLastError(0x5a17);errno=1927;
    int result=GetWindowTextLengthW(hwnd);DWORD error=GetLastError();int saved_errno=errno;
    printf("{\"label\":\"%s\",\"length\":%d,\"LastError\":%lu,\"errno\":%d,\"WM_GETTEXTLENGTH_calls\":%d},\n",label,result,(unsigned long)error,saved_errno,len_calls-before);
}
int main(void){
    HINSTANCE module=GetModuleHandleW(NULL);WNDCLASSW cls={0};cls.lpfnWndProc=procedure;cls.hInstance=module;cls.lpszClassName=L"FrostpunkOwnTitleOracle";
    if(!RegisterClassW(&cls))return 2;
    HWND hwnd=CreateWindowExW(0,cls.lpszClassName,L"Initial\x0416",0,0,0,1,1,NULL,NULL,module,NULL);
    if(!hwnd)return 3;
    puts("{\"hidden_window_only\":true,\"host_settings_changed\":false,\"creation\":{");
    printf("\"before_DefProc_length\":%d,\"before32\":",creation_before_length);hex(creation_before,32);
    printf(",\"after_DefProc_length\":%d,\"after32\":",creation_after_length);hex(creation_after,32);puts("},\"rows\":[");
    length_row(hwnd,"initial-length");
    set_row(hwnd,"normal",0,L"Hello\x0416\xd83d\xde42");
    int capacities[]={-1,0,1,2,7,8,9,32};
    for(unsigned i=0;i<sizeof(capacities)/sizeof(capacities[0]);++i)get_row(hwnd,"normal-read",capacities[i]);
    length_row(hwnd,"unicode-length");
    set_row(hwnd,"reject",1,L"Rejected");
    set_row(hwnd,"return2",2,L"ReturnTwo");
    set_row(hwnd,"returnHigh32",3,L"ReturnHigh");
    set_row(hwnd,"returnMinus1",4,L"ReturnMinus");
    set_row(hwnd,"callback-errors",5,L"CallbackErrors");
    set_row(hwnd,"NULL-title",0,NULL);length_row(hwnd,"NULL-length");
    static const WCHAR embedded[]={'A',0,'B',0};set_row(hwnd,"embedded-NUL",0,embedded);
    static const WCHAR high[]={0xd800,0};set_row(hwnd,"lone-high",0,high);get_row(hwnd,"lone-high-read",4);
    static const WCHAR low[]={0xdc00,0};set_row(hwnd,"lone-low",0,low);get_row(hwnd,"lone-low-read",4);
    set_row(NULL,"invalid-NULL-HWND",0,L"Title");
    set_row((HWND)(uintptr_t)1,"invalid-HWND1-NULL-title",0,NULL);
    get_row(NULL,"invalid-NULL-HWND-read",8);length_row(NULL,"invalid-NULL-HWND-length");
    DestroyWindow(hwnd);set_row(hwnd,"destroyed-HWND",0,L"Title");
    puts("{\"end\":true}]}");
    if(!UnregisterClassW(cls.lpszClassName,module))return 4;
    return 0;
}
