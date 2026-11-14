#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "type_info_input.inc"
typedef const char*(__cdecl *name_fn)(void*,void*);
typedef void(__cdecl *destroy_fn)(void*);
typedef int*(__cdecl *errno_fn)(void);
typedef void*(__cdecl *malloc_fn)(size_t);
static malloc_fn real_malloc;static int *native_errno;static unsigned malloc_calls,fail_malloc_call;
static void* __cdecl probe_malloc(size_t bytes){
    ++malloc_calls;if(malloc_calls==fail_malloc_call){*native_errno=12;return NULL;}return real_malloc(bytes);
}
static uintptr_t *malloc_slot(HMODULE module){
    unsigned char *base=(unsigned char*)module;IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *imports=(IMAGE_IMPORT_DESCRIPTOR*)(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for(;imports->Name;++imports){
        IMAGE_THUNK_DATA64 *names=(IMAGE_THUNK_DATA64*)(base+imports->OriginalFirstThunk);
        uintptr_t *slots=(uintptr_t*)(base+imports->FirstThunk);
        for(unsigned i=0;names[i].u1.AddressOfData;++i)if(!IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)){
            IMAGE_IMPORT_BY_NAME *symbol=(IMAGE_IMPORT_BY_NAME*)(base+names[i].u1.AddressOfData);
            if(!strcmp((char*)symbol->Name,"malloc"))return &slots[i];
        }
    }
    return NULL;
}
typedef struct {uint64_t cache;char decorated[512];} descriptor;
int main(int argc,char **argv){
    if(argc!=2)return 2;
    HMODULE crt=LoadLibraryExW(L"vcruntime140.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!crt)return 3;
    name_fn name=(name_fn)GetProcAddress(crt,"__std_type_info_name");
    destroy_fn destroy=(destroy_fn)GetProcAddress(crt,"__std_type_info_destroy_list");if(!name||!destroy)return 4;
    HMODULE ucrt=GetModuleHandleW(L"ucrtbase.dll");errno_fn dll_errno=ucrt?(errno_fn)GetProcAddress(ucrt,"_errno"):NULL;
    if(!dll_errno)return 13;native_errno=dll_errno();
    FILE *out=fopen(argv[1],"wb");if(!out)return 5;
    unsigned count=sizeof(input_cases)/sizeof(input_cases[0]);if(fwrite(&count,4,1,out)!=1)return 6;
    __declspec(align(16)) uint64_t root[2]={0};descriptor data[11]={0};uint64_t previous=0;unsigned csr=_mm_getcsr();
    for(unsigned i=0;i<count;++i){
        strcpy_s(data[i].decorated,sizeof(data[i].decorated),input_cases[i].decorated);
        _mm_setcsr(0x9fd5);errno=2718;*native_errno=1379;SetLastError(0x5a17);
        const char *text=name(&data[i],root);int e=errno;DWORD error=GetLastError();unsigned fp=_mm_getcsr();
        if(!text||e!=2718||*native_errno!=1379||error!=0x5a17||fp!=0x9fd5||data[i].cache!=(uintptr_t)text)return 7;
        uint64_t node=(uintptr_t)text-16;
        if(root[0]!=(uint64_t)(i+1)*UINT64_C(0x20001)||root[1]!=node||*(uint64_t*)(uintptr_t)node!=previous||*(uint64_t*)(uintptr_t)(node+8))return 8;
        uint64_t before[2]={root[0],root[1]};
        if(name(&data[i],NULL)!=text||memcmp(root,before,sizeof(root)))return 9;
        uint32_t head[]={input_cases[i].rva,(uint32_t)strlen(data[i].decorated)+1,(uint32_t)strlen(text)+1};
        if(fwrite(head,sizeof(head),1,out)!=1||fwrite(&root[0],8,1,out)!=1||fwrite(data[i].decorated,head[1],1,out)!=1||fwrite(text,head[2],1,out)!=1)return 10;
        previous=node;
    }
    destroy(root);_mm_setcsr(csr);if(root[0]!=(uint64_t)count*UINT64_C(0x20000)||root[1])return 11;
    if(fclose(out))return 12;
    /* Fault injection patches only this oracle process's loaded native DLL IAT.
     * Exercise both the demangler's first malloc and final SLIST-node malloc. */
    uintptr_t *slot=malloc_slot(crt);DWORD protection;if(!slot||!VirtualProtect(slot,8,PAGE_READWRITE,&protection))return 14;
    real_malloc=(malloc_fn)*slot;*slot=(uintptr_t)probe_malloc;
    descriptor probe={0};strcpy_s(probe.decorated,sizeof(probe.decorated),input_cases[0].decorated);
    __declspec(align(16)) uint64_t probe_root[2]={0};malloc_calls=0;fail_malloc_call=0;
    if(!name(&probe,probe_root)||!malloc_calls)return 15;unsigned total_malloc_calls=malloc_calls;destroy(probe_root);
    unsigned failures[]={1,total_malloc_calls};
    for(unsigned i=0;i<2;++i){memset(&probe,0,sizeof(probe));strcpy_s(probe.decorated,sizeof(probe.decorated),input_cases[0].decorated);memset(probe_root,0,sizeof(probe_root));
        malloc_calls=0;fail_malloc_call=failures[i];*native_errno=1379;SetLastError(0x5a17);
        if(name(&probe,probe_root)||probe.cache||probe_root[0]||probe_root[1]||*native_errno!=12||GetLastError()!=0x5a17)return 16;
    }
    *slot=(uintptr_t)real_malloc;DWORD ignored;if(!VirtualProtect(slot,8,protection,&ignored))return 17;
    printf("{\"success\":true,\"cases\":%u,\"cached_ignores_root\":true,\"SList16_node_chain_depth_sequence\":true,\"errno_LastError_FP_preserved\":true,\"ucrt_errno_preserved\":true,\"OOM_errno12_no_cache_or_list\":true,\"native_malloc_calls\":%u}\n",count,total_malloc_calls);return 0;
}
