/* Development-only native Windows oracle. Its own windows stay hidden. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(WNDCLASSW)==72 && offsetof(WNDCLASSW,lpszClassName)==64,"WNDCLASSW");
_Static_assert(sizeof(WNDCLASSEXW)==80 && offsetof(WNDCLASSEXW,hIconSm)==72,"WNDCLASSEXW");
_Static_assert(sizeof(MSG)==48 && offsetof(MSG,wParam)==16 && offsetof(MSG,time)==32 && offsetof(MSG,pt)==36,"MSG");
_Static_assert(sizeof(CREATESTRUCTW)==80 && offsetof(CREATESTRUCTW,lpszName)==56 && offsetof(CREATESTRUCTW,dwExStyle)==72,"CREATESTRUCTW");
static unsigned checks,count,events[64];static int reject_nc,reject_create,creating_message;
static void require(int value,const char *why){if(!value){fprintf(stderr,"WINDOW_ORACLE_FAIL %s error=%lu\n",why,GetLastError());exit(1);}++checks;}
static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_NCCREATE || msg==WM_CREATE || msg==WM_DESTROY || msg==WM_NCDESTROY){require(count<64,"callback log capacity");events[count++]=msg;}
    if(msg==WM_NCCREATE || msg==WM_CREATE){CREATESTRUCTW *data=(void*)lp;require(data->lpCreateParams==(void*)(uintptr_t)0xaabbccdd,"CREATESTRUCT parameter");if(creating_message)require(data->hwndParent && data->hwndParent!=HWND_MESSAGE && !data->style,"message-only internal opaque parent/raw style");else require(data->cx==800 && data->cy==600,"CREATESTRUCT values");}
    if(msg==WM_NCCREATE){SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)0xaabbccdd);if(reject_nc)return FALSE;}
    if(msg==WM_CREATE && reject_create)return -1;
    if(msg==WM_DESTROY)require(GetWindowLongPtrW(window,GWLP_USERDATA)==(LONG_PTR)0xaabbccdd,"userdata alive in destroy");
    return DefWindowProcW(window,msg,wp,lp);
}
static HWND create(ATOM atom,HINSTANCE instance){return CreateWindowExW(0,MAKEINTATOM(atom),L"",WS_POPUP,0,0,800,600,NULL,NULL,instance,(void*)(uintptr_t)0xaabbccdd);}
static void drain(void){MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE))require(msg.message!=WM_QUIT,"no unexpected quit while draining");}
static int relevant(const MSG *a,const MSG *b){return a->hwnd==b->hwnd && a->message==b->message && a->wParam==b->wParam && a->lParam==b->lParam && a->time==b->time && a->pt.x==b->pt.x && a->pt.y==b->pt.y;}
static void scan(const char *name,UINT vk,UINT scan_vk,UINT force_extended,HKL layout){
    UINT value=MapVirtualKeyExW(scan_vk,MAPVK_VK_TO_VSC_EX,layout);require(value!=0,"MapVirtualKeyEx US scan oracle");
    if(force_extended)value|=0xe000;printf("KEYSCAN %s %u %u\n",name,vk,value);
}
static void keyboard_scans(void){
    /* Loading without KLF_ACTIVATE leaves the user's active keyboard alone. */
    HKL previous=GetKeyboardLayout(0),layout=LoadKeyboardLayoutW(L"00000409",KLF_NOTELLSHELL);require(layout!=NULL && GetKeyboardLayout(0)==previous,"load oracle US layout without activation");
    char name[32];for(unsigned i=0;i<26;++i){sprintf_s(name,sizeof(name),"AKEYCODE_%c",'A'+i);scan(name,'A'+i,'A'+i,0,layout);}
    for(unsigned i=0;i<10;++i){sprintf_s(name,sizeof(name),"AKEYCODE_%c",'0'+i);scan(name,'0'+i,'0'+i,0,layout);}
    for(unsigned i=0;i<12;++i){sprintf_s(name,sizeof(name),"AKEYCODE_F%u",i+1);scan(name,VK_F1+i,VK_F1+i,0,layout);}
    static const struct {const char *name;UINT vk,scan_vk,extended;} keys[]={
        {"BACK",VK_ESCAPE,VK_ESCAPE,0},{"ESCAPE",VK_ESCAPE,VK_ESCAPE,0},{"ENTER",VK_RETURN,VK_RETURN,0},{"NUMPAD_ENTER",VK_RETURN,VK_RETURN,1},
        {"TAB",VK_TAB,VK_TAB,0},{"SPACE",VK_SPACE,VK_SPACE,0},{"DEL",VK_BACK,VK_BACK,0},{"FORWARD_DEL",VK_DELETE,VK_DELETE,1},
        {"DPAD_LEFT",VK_LEFT,VK_LEFT,1},{"DPAD_UP",VK_UP,VK_UP,1},{"DPAD_RIGHT",VK_RIGHT,VK_RIGHT,1},{"DPAD_DOWN",VK_DOWN,VK_DOWN,1},
        {"SHIFT_LEFT",VK_SHIFT,VK_LSHIFT,0},{"SHIFT_RIGHT",VK_SHIFT,VK_RSHIFT,0},{"CTRL_LEFT",VK_CONTROL,VK_LCONTROL,0},{"CTRL_RIGHT",VK_CONTROL,VK_RCONTROL,0},
        {"ALT_LEFT",VK_MENU,VK_LMENU,0},{"ALT_RIGHT",VK_MENU,VK_RMENU,0},{"CAPS_LOCK",VK_CAPITAL,VK_CAPITAL,0},{"NUM_LOCK",VK_NUMLOCK,VK_NUMLOCK,0},{"SCROLL_LOCK",VK_SCROLL,VK_SCROLL,0},
        {"MOVE_HOME",VK_HOME,VK_HOME,1},{"MOVE_END",VK_END,VK_END,1},{"PAGE_UP",VK_PRIOR,VK_PRIOR,1},{"PAGE_DOWN",VK_NEXT,VK_NEXT,1},{"INSERT",VK_INSERT,VK_INSERT,1},
        {"MINUS",VK_OEM_MINUS,VK_OEM_MINUS,0},{"EQUALS",VK_OEM_PLUS,VK_OEM_PLUS,0},{"LEFT_BRACKET",VK_OEM_4,VK_OEM_4,0},{"RIGHT_BRACKET",VK_OEM_6,VK_OEM_6,0},
        {"BACKSLASH",VK_OEM_5,VK_OEM_5,0},{"SEMICOLON",VK_OEM_1,VK_OEM_1,0},{"APOSTROPHE",VK_OEM_7,VK_OEM_7,0},{"COMMA",VK_OEM_COMMA,VK_OEM_COMMA,0},
        {"PERIOD",VK_OEM_PERIOD,VK_OEM_PERIOD,0},{"SLASH",VK_OEM_2,VK_OEM_2,0},{"GRAVE",VK_OEM_3,VK_OEM_3,0}
    };
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i){sprintf_s(name,sizeof(name),"AKEYCODE_%s",keys[i].name);scan(name,keys[i].vk,keys[i].scan_vk,keys[i].extended,layout);}
    /* The layout may already be globally loaded; retain it rather than unload
     * a layout another user thread could be using. No activation is performed. */
}
int main(void){
    HINSTANCE instance=GetModuleHandleW(NULL);const WCHAR *name=L"FrostpunkStaticHiddenWindowOracle";
    WNDCLASSEXW cls={0};cls.cbSize=sizeof(cls);cls.lpfnWndProc=procedure;cls.cbWndExtra=16;cls.hInstance=instance;cls.lpszClassName=name;
    ATOM atom=RegisterClassExW(&cls);require(atom!=0,"RegisterClassExW");
    WNDCLASSEXW out={0};out.cbSize=sizeof(out);require(GetClassInfoExW(instance,MAKEINTATOM(atom),&out) && out.lpfnWndProc==procedure && out.cbWndExtra==16,"GetClassInfoExW ABI");
    creating_message=1;HWND message_window=CreateWindowExW(0,MAKEINTATOM(atom),NULL,0,0,0,0,0,HWND_MESSAGE,NULL,instance,(void*)(uintptr_t)0xaabbccdd);creating_message=0;
    require(message_window && count==2 && events[0]==WM_NCCREATE && events[1]==WM_CREATE,"message-only creation callbacks");
    require(IsWindow(message_window) && !GetParent(message_window) && !IsWindowVisible(message_window) && !IsIconic(message_window) && GetWindowLongPtrW(message_window,GWL_STYLE)==0x04c00000,"message-only native identity/parent/style/visibility");
    require(PostMessageW(message_window,WM_APP+81,0x123,0x456),"message-only native post");MSG message={0};require(!PeekMessageW(&message,(HWND)(intptr_t)-1,0,0,PM_REMOVE) && PeekMessageW(&message,message_window,WM_APP+81,WM_APP+81,PM_REMOVE) && message.hwnd==message_window && message.wParam==0x123 && message.lParam==0x456,"message-only is a window message with HWND filter");
    require(DestroyWindow(message_window),"message-only destroy");count=0;
    reject_nc=1;require(!create(atom,instance),"NCCREATE false rejects window");require(count==2 && events[0]==WM_NCCREATE && events[1]==WM_NCDESTROY,"NCCREATE failure order");reject_nc=0;
    count=0;reject_create=1;require(!create(atom,instance),"CREATE minus1 rejects window");require(count==4 && events[0]==WM_NCCREATE && events[1]==WM_CREATE && events[2]==WM_DESTROY && events[3]==WM_NCDESTROY,"CREATE failure order");reject_create=0;
    count=0;HWND window=create(atom,instance);require(window && count==2 && events[0]==WM_NCCREATE && events[1]==WM_CREATE,"creation callback order");
    require(!IsWindowVisible(window),"oracle remains hidden");require(!UnregisterClassW(MAKEINTATOM(atom),instance) && GetLastError()==ERROR_CLASS_HAS_WINDOWS,"cannot unregister live class");
    drain();MSG first={0},second={0};
    require(PostMessageW(window,WM_APP+1,11,22) && PostThreadMessageW(GetCurrentThreadId(),WM_APP+2,33,44),"post window and thread messages");
    require(PeekMessageW(&first,(HWND)(intptr_t)-1,0,0,PM_NOREMOVE) && first.message==WM_APP+2 && !first.hwnd,"minus1 thread filter");
    require(PeekMessageW(&second,(HWND)(intptr_t)-1,0,0,PM_REMOVE) && relevant(&first,&second),"NOREMOVE retains same message");
    require(PeekMessageW(&first,window,WM_APP+1,WM_APP+1,PM_REMOVE) && first.message==WM_APP+1 && first.wParam==11 && first.lParam==22,"HWND and range filter");
    PostQuitMessage(37);require(PostMessageW(window,WM_APP+3,55,66),"post after low priority quit");
    require(PeekMessageW(&first,NULL,0,0,PM_REMOVE) && first.message==WM_APP+3,"posted message precedes pending PostQuit");
    require(GetMessageW(&first,window,WM_APP+9,WM_APP+9)==0 && first.message==WM_QUIT && first.wParam==37 && !first.hwnd,"QUIT ignores HWND and range");
    SetLastError(0);require(GetMessageW(&first,(HWND)(uintptr_t)0x12345678,0,0)==-1 && GetLastError()==ERROR_INVALID_WINDOW_HANDLE,"GetMessage invalid HWND minus1");
    /* A directly posted WM_QUIT is a real posted entry, unlike PostQuitMessage. */
    require(PostMessageW(window,WM_QUIT,91,92) && PostMessageW(window,WM_APP+4,0,0),"direct posted quit");
    require(PeekMessageW(&first,NULL,0,0,PM_REMOVE) && first.message==WM_QUIT && first.wParam==91 && first.lParam==92 && first.hwnd==window,"direct WM_QUIT FIFO and payload");
    require(PeekMessageW(&first,NULL,0,0,PM_REMOVE) && first.message==WM_APP+4,"posted after direct quit");
    require(PostMessageW(window,WM_QUIT,93,94),"direct quit for range test");
    int direct_filter=PeekMessageW(&first,window,WM_APP+9,WM_APP+9,PM_REMOVE);
    if(direct_filter)require(first.message==WM_QUIT && first.wParam==93,"direct quit filtered retrieval");
    else require(PeekMessageW(&first,NULL,0,0,PM_REMOVE) && first.message==WM_QUIT && first.wParam==93,"direct quit remains after nonmatching filter");
    for(UINT msg=WM_KEYDOWN;msg<=WM_SYSKEYUP;++msg){if(msg==WM_CHAR || msg==WM_DEADCHAR)continue;MSG key={0};key.hwnd=window;key.message=msg;key.wParam=VK_SHIFT;key.lParam=1;MSG before=key;require(TranslateMessage(&key)!=0 && !memcmp(&key,&before,sizeof(key)),"Translate four key types and unchanged MSG");}
    MSG other={0};other.hwnd=window;other.message=WM_APP+10;require(!TranslateMessage(&other),"Translate nonkey false");drain();
    count=0;require(DestroyWindow(window) && count==2 && events[0]==WM_DESTROY && events[1]==WM_NCDESTROY,"destroy callback order");require(!IsWindow(window),"destroyed HWND");require(UnregisterClassW(MAKEINTATOM(atom),instance),"unregister destroyed class");
    keyboard_scans();printf("WINDOW_ORACLE_PASS checks=%u ABI72_80_48_80=1 hidden_window_only=1 message_only=1 creation_failure_order=1 filters_quit_priority=1 directly_posted_quit_ignores_filter=%d original_game_exec=0\n",checks,direct_filter);
    return 0;
}
