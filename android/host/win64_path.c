/* SHLWAPI UTF16 path strings; these operations never access the filesystem. */
#include "win64_path.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_UNITS 260
static _Noreturn void path_stop(const char *reason){fprintf(stderr,"STOP Win64 path %s\n",reason);fflush(stderr);_Exit(73);}
static int drive_letter(uint16_t u){return(u>='A'&&u<='Z')||(u>='a'&&u<='z');}
static uint16_t unit(uint64_t address,size_t index){
    if(address>UINT64_MAX-index*2)path_stop("UTF16 address overflow");
    uint16_t u;memcpy(&u,win64_pointer(address+index*2,2,0),2);return u;
}
static uint32_t relative(uint64_t address){
    if(!address)return 1;uint16_t first=unit(address,0);
    if(first=='\\')return 0;
    return !drive_letter(first)||unit(address,1)!=':';
}
static uint32_t canonicalize(uint64_t destination,uint64_t source){
    if(!destination){win64_set_error(87);return 0;}
    void *buffer=win64_pointer(destination,PATH_UNITS*2,1);
    if(!source){memset(buffer,0,2);win64_set_error(87);return 0;}
    /* Native SHLWAPI clears the destination before reading the input. */
    if(destination==source){memcpy(buffer,(uint16_t[]){'\\',0},4);return 1;}
    uint16_t input[PATH_UNITS],output[PATH_UNITS];size_t length=0;
    for(;length<PATH_UNITS;++length){input[length]=unit(source,length);if(!input[length])break;}
    /* shortcut: the proven game calls use MAX_PATH DOS strings and distinct
     * buffers; extend long-path/UNC/device namespaces only for an actual caller. */
    if(length==PATH_UNITS)path_stop("input exceeds MAX_PATH contract");
    if(length>=2&&input[0]=='\\'&&input[1]=='\\')path_stop("UNC/device namespace is unsupported");
    uint64_t input_bytes=(length+1)*2;
    if(source<destination+PATH_UNITS*2&&destination<source+input_bytes)path_stop("partially overlapping path buffers");
    size_t used=0,floor=0,i=0;
    if(length>=3&&drive_letter(input[0])&&input[1]==':'&&input[2]=='\\'){
        memcpy(output,input,6);used=floor=i=3;
    }else if(length&&input[0]=='\\'){output[used++]='\\';floor=i=1;}
    while(i<length){
        size_t end=i;while(end<length&&input[end]!='\\')++end;
        size_t count=end-i;
        if(count>256){memset(buffer,0,2);win64_set_error(206);return 0;}
        int two_root=used==2&&output[0]=='\\'&&output[1]=='\\';
        if(count==1&&input[i]=='.'){
            if(end==length){if(used>floor&&output[used-1]=='\\'&&!two_root)--used;i=end;}
            else i=end+1;
        }else if(count==2&&input[i]=='.'&&input[i+1]=='.'){
            if(two_root)i=end+1;
            else if(used){
                if(used>floor&&output[used-1]=='\\')--used;
                while(used>floor&&output[used-1]!='\\')--used;
                if(end==length){if(used>floor&&output[used-1]=='\\')--used;i=end;}
                else i=used?end+1:end;
            }else i=end+1;
        }else{
            if(count>PATH_UNITS-1-used){memset(buffer,0,2);win64_set_error(206);return 0;}
            memcpy(output+used,input+i,count*2);used+=count;i=end;
        }
        if(i<length&&input[i]=='\\'){
            if(used==PATH_UNITS-1){memset(buffer,0,2);win64_set_error(206);return 0;}
            output[used++]='\\';++i;
        }
    }
    while(used&&output[used-1]=='.')--used;
    if(used==2&&drive_letter(output[0])&&output[1]==':')output[used++]='\\';
    if(!used)output[used++]='\\';output[used]=0;
    memcpy(buffer,output,(used+1)*2);return 1;
}
int win64_path_call(CPU *c,const char *name){
    if(!strcmp(name,"PathIsRelativeW")){c->rax=relative(win64_argument(c,0));return 1;}
    if(!strcmp(name,"PathCanonicalizeW")){c->rax=canonicalize(win64_argument(c,0),win64_argument(c,1));return 1;}
    return 0;
}
void win64_path_self_test(void){
    uint32_t saved_error=win64_get_error();uint64_t source=win64_alloc(32,1),destination=win64_alloc(PATH_UNITS*2,1);
    if(!source||!destination)path_stop("self-test allocation");
    const uint16_t path[]={'C',':','\\','a','\\','.','.','\\','b',0},expected[]={'C',':','\\','b',0};
    memcpy(win64_pointer(source,sizeof(path),1),path,sizeof(path));CPU c;memset(&c,0xa5,sizeof(c));c.rcx=destination;c.rdx=source;CPU saved=c;
    win64_set_error(1927);if(!win64_path_call(&c,"PathCanonicalizeW")||c.rax!=1||memcmp(win64_pointer(destination,sizeof(expected),0),expected,sizeof(expected))||win64_get_error()!=1927)path_stop("canonicalization self-test");
    c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c)))path_stop("CPU/RSP preservation");
    c.rcx=source;if(!win64_path_call(&c,"PathIsRelativeW")||c.rax)path_stop("drive-relative classification");
    saved=c;if(win64_path_call(&c,"UnknownPathFunction")||memcmp(&c,&saved,sizeof(c)))path_stop("unknown import protocol");
    win64_free(destination);win64_free(source);win64_set_error(saved_error);
    fprintf(stderr,"WIN64_PATH_SELF_TEST DOS_UTF16=1 canonicalization=1 relative=1 no_filesystem=1 no_rsp_pop=1 passed\n");
}
