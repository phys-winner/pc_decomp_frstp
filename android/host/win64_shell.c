/* Explicit Android guest known-folder redirection, backed by the configured
 * kernel profile anchor. SHGetFolderPath is a query; profile provisioning is
 * separate and queries without CSIDL_FLAG_CREATE never create directories. */
#include "win64_shell.h"
#include "win64.h"
#include "win64_kernel.h"
#include "win64_window.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define E_INVALIDARG UINT32_C(0x80070057)
#define E_NOTIMPL UINT32_C(0x80004001)
#define MAX_PATH_UNITS 260
static const char *const paths[]={"C:\\FrostpunkProfile","C:\\FrostpunkProfile\\AppData","C:\\FrostpunkProfile\\Pictures"};
static uint16_t wide_paths[3][MAX_PATH_UNITS];
static pthread_once_t paths_once=PTHREAD_ONCE_INIT;
static uint32_t hresult_error(uint32_t error){return error?UINT32_C(0x80070000)|(error&0xffff):0;}
static _Noreturn void shell_stop(const char *reason){fprintf(stderr,"STOP Win64 shell %s\n",reason);fflush(stderr);_Exit(73);}
static void initialize_paths(void){
    for(unsigned n=0;n<3;++n){size_t length=strlen(paths[n]);if(length>=MAX_PATH_UNITS)shell_stop("redirected MAX_PATH policy");
        for(size_t i=0;i<=length;++i)wide_paths[n][i]=(unsigned char)paths[n][i];
        win64_region_add((uintptr_t)wide_paths[n],sizeof(wide_paths[n]),0);
    }
}
static uint32_t kernel(const char *name,unsigned path){
    CPU c={0};c.rcx=(uintptr_t)wide_paths[path];
    if(!win64_kernel_call(&c,name))shell_stop("missing kernel filesystem provider");return(uint32_t)c.rax;
}
static uint32_t directory(unsigned path,int create){
    if(create && !kernel("CreateDirectoryW",path) && win64_get_error()!=183)return hresult_error(win64_get_error());
    uint32_t attributes=kernel("GetFileAttributesW",path);
    if(attributes==UINT32_MAX)return hresult_error(win64_get_error());
    return attributes&0x10?0:hresult_error(267);
}
int win64_shell_provision(void){
    if(pthread_once(&paths_once,initialize_paths))shell_stop("path initialization");uint32_t saved=win64_get_error();
    uint32_t result=directory(0,0);if(!result)result=directory(1,1);if(!result)result=directory(2,1);
    if(result){win64_set_error(result&0xffff);return 0;}win64_set_error(saved);return 1;
}
static uint32_t folder_path(CPU *c){
    uint32_t folder=(uint32_t)win64_argument(c,1),type=(uint32_t)win64_argument(c,3);uint64_t token=win64_argument(c,2),out=win64_argument(c,4);
    if(!out || (folder&~UINT32_C(0xffff)))return E_INVALIDARG;
    uint32_t id=folder&0xff;int create=(folder&0x8000)!=0;
    if(id>0x3d)return E_INVALIDARG;
    /* shortcut: only the two proven CURRENT folders, ordinary user token and
     * CREATE flag are supported; other shell namespaces need an actual caller. */
    if(token || type || (folder&~UINT32_C(0x80ff)) || (id!=26 && id!=39)){
        *(uint16_t*)win64_pointer(out,2,1)=0;return E_NOTIMPL;
    }
    uint16_t *buffer=win64_pointer(out,MAX_PATH_UNITS*2,1);
    if(pthread_once(&paths_once,initialize_paths))shell_stop("path initialization");
    unsigned path=id==26?1:2;uint32_t saved=win64_get_error(),result=directory(path,create);win64_set_error(saved);
    if(result){buffer[0]=0;return result;}
    memcpy(buffer,wide_paths[path],(strlen(paths[path])+1)*2);return 0;
}
static uint32_t extract_icon(CPU *c){
    /* shortcut: no PE icon provider. Only the proven SDL first-icon request
     * is accepted; add real extraction if a caller ever requires icon handles. */
    if((uint32_t)c->rdx || (uint32_t)win64_argument(c,4)!=1)shell_stop("unsupported ExtractIconExW index/count");
    size_t n=0;
    for(;n<MAX_PATH_UNITS;++n){uint16_t ch;memcpy(&ch,win64_pointer(c->rcx+n*2,2,0),2);if(!ch)break;}
    if(n==MAX_PATH_UNITS)shell_stop("ExtractIconExW filename exceeds proven MAX_PATH");
    void *large=c->r8?win64_pointer(c->r8,8,1):NULL,*small=c->r9?win64_pointer(c->r9,8,1):NULL;
    uint64_t zero=0;if(large)memcpy(large,&zero,8);if(small)memcpy(small,&zero,8);
    win64_set_error(50);return UINT32_MAX;
}
int win64_shell_call(CPU *c,const char *name){
    if(!strcmp(name,"DragAcceptFiles")){win64_window_drag_accept_files(c);return 1;}
    if(!strcmp(name,"ExtractIconExW")){c->rax=extract_icon(c);return 1;}
    if(strcmp(name,"SHGetFolderPathW"))return 0;c->rax=folder_path(c);return 1;
}
void win64_shell_self_test(void){
    uint32_t saved_error=win64_get_error();uint64_t stack=win64_alloc(64,1),out=win64_alloc(520,1);if(!stack||!out)shell_stop("self-test allocation");
    uint64_t *argument=win64_pointer(stack+40,8,1);*argument=out;
    CPU c={0};c.rsp=stack;c.rdx=UINT64_C(0x1001a);CPU saved=c;
    *(uint16_t*)win64_pointer(out,2,1)=0xa5a5;
    if(!win64_shell_call(&c,"SHGetFolderPathW") || c.rax!=E_INVALIDARG || *(uint16_t*)win64_pointer(out,2,0)!=0xa5a5)shell_stop("invalid-CSIDL/no-write ABI");
    c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c)))shell_stop("CPU/RSP preservation");
    c.rdx=0x801a;*argument=0;if(!win64_shell_call(&c,"SHGetFolderPathW") || c.rax!=E_INVALIDARG)shell_stop("NULL output HRESULT");
    const uint16_t filename[]={'x',0};memcpy(win64_pointer(out+32,sizeof(filename),1),filename,sizeof(filename));
    uint64_t canary=UINT64_MAX;memcpy(win64_pointer(out,8,1),&canary,8);memcpy(win64_pointer(out+8,8,1),&canary,8);
    memset(&c,0xa5,sizeof(c));c.rcx=out+32;c.rdx=0;c.r8=out;c.r9=out+8;c.rsp=stack;*argument=1;saved=c;
    int saved_errno=errno;errno=2718;
    if(!win64_shell_call(&c,"ExtractIconExW") || c.rax!=UINT32_MAX || win64_get_error()!=50 || errno!=2718 ||
       *(uint64_t*)win64_pointer(out,8,0) || *(uint64_t*)win64_pointer(out+8,8,0))shell_stop("icon-unavailable error/output ABI");
    c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c)))shell_stop("icon CPU/RSP preservation");errno=saved_errno;
    saved=c;if(win64_shell_call(&c,"UnknownShellFunction") || memcmp(&c,&saved,sizeof(c)))shell_stop("unknown import protocol");
    win64_free(out);win64_free(stack);win64_set_error(saved_error);
    fprintf(stderr,"WIN64_SHELL_SELF_TEST invalid_CSIL_NULL_HRESULT=1 icon_unavailable50=1 no_filesystem_write=1 no_rsp_pop=1 passed\n");
}
