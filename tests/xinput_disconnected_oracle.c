/* System XInput 9.1.0 only. Refuse connected controllers before any SetState. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef DWORD (WINAPI *get_state_fn)(DWORD,XINPUT_STATE *);
typedef DWORD (WINAPI *set_state_fn)(DWORD,XINPUT_VIBRATION *);
_Static_assert(sizeof(XINPUT_VIBRATION)==4 && sizeof(XINPUT_STATE)==16,"Win64 XInput layouts");
int main(int argc,char **argv) {
    if(argc!=4)return 64;
    unsigned api=(unsigned)strtoul(argv[1],NULL,0),kind=(unsigned)strtoul(argv[3],NULL,0);
    DWORD index=(DWORD)strtoul(argv[2],NULL,0);
    if(api>1||kind>6)return 64;
    HMODULE module=LoadLibraryExW(L"xinput9_1_0.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module)return 2;
    get_state_fn get=(get_state_fn)(void*)GetProcAddress(module,"XInputGetState");
    set_state_fn set=(set_state_fn)(void*)GetProcAddress(module,"XInputSetState");
    if(!get||!set)return 3;
    /* Repeated in each isolated process, including immediately before SetState. */
    for(DWORD i=0;i<4;++i){XINPUT_STATE state;if(get(i,&state)!=ERROR_DEVICE_NOT_CONNECTED)return 4;}
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    unsigned char *guard=VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    unsigned char *readonly=VirtualAlloc(NULL,page,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!guard||!readonly)return 5;
    DWORD old;if(!VirtualProtect(guard+page,page,PAGE_NOACCESS,&old)||
                 !VirtualProtect(readonly,page,PAGE_READONLY,&old))return 6;
    unsigned char output[32],before[32];memset(output,0xa5,sizeof(output));
    memset(output+8,0,16);memcpy(before,output,sizeof(output));
    void *pointers[]={output+8,NULL,(void*)1,(void*)UINTPTR_MAX,guard+page,readonly,guard+page-2};
    if(api==1&&index<4){XINPUT_STATE state;if(get(index,&state)!=ERROR_DEVICE_NOT_CONNECTED)return 4;}
    SetLastError(0x51cafe);errno=2718;
    DWORD result=api?set(index,pointers[kind]):get(index,pointers[kind]);
    DWORD error=GetLastError();int native_errno=errno;
    int unchanged=!memcmp(output,before,sizeof(output));
    printf("{\"api\":%u,\"index\":%lu,\"pointer_kind\":%u,\"result\":%lu,\"LastError\":%lu,\"errno\":%d,\"buffer_untouched\":%s,\"controllers_absent_verified\":true}\n",
           api,index,kind,result,error,native_errno,unchanged?"true":"false");
    VirtualFree(readonly,0,MEM_RELEASE);VirtualFree(guard,0,MEM_RELEASE);FreeLibrary(module);return 0;
}
