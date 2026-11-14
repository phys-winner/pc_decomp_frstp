#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#pragma pack(push,1)
typedef struct {uint32_t kind,key;uint64_t count,width;uint32_t flags,mode;} input_case;
typedef struct {int32_t index;uint32_t record;int32_t error;uint32_t last_error,callbacks,invalid,exception,traces;int16_t trace[16];} output_case;
#pragma pack(pop)
_Static_assert(sizeof(input_case)==32,"input32");
_Static_assert(sizeof(output_case)==64,"output64");
static int *(__cdecl *get_errno)(void);
static input_case current;
static output_case result;
static const unsigned char *base;
static int __cdecl compare(const void *key,const void *element){
    uint32_t a=0,b=0;
    if(result.callbacks<16)result.trace[result.callbacks]=(int16_t)(current.width?((const unsigned char*)element-base)/current.width:0);
    ++result.callbacks;
    if(current.flags&8){*get_errno()=2345;SetLastError(1234);}
    if(current.mode==2)return 0; /* The API permits an opaque NULL key. */
    if(current.kind==0){uint16_t x,y;memcpy(&x,key,2);memcpy(&y,element,2);a=x;b=y;}
    else {memcpy(&a,key,4);memcpy(&b,element,4);}
    if(a==b)return 0;
    if(a<b)return current.mode?INT_MIN:-1;
    return current.mode?INT_MAX:1;
}
static void __cdecl invalid(const wchar_t *expression,const wchar_t *function,const wchar_t *file,unsigned line,uintptr_t reserved){
    (void)expression;(void)function;(void)file;(void)line;(void)reserved;++result.invalid;
    if(current.flags&16){*get_errno()=4321;SetLastError(8765);}
}
int main(int argc,char **argv){
    if(argc!=4)return 2;
    wchar_t path[MAX_PATH],actual[MAX_PATH];UINT n=GetSystemDirectoryW(path,MAX_PATH);
    if(!n||n+14>=MAX_PATH)return 3;wcscat_s(path,MAX_PATH,L"\\ucrtbase.dll");
    HMODULE module=LoadLibraryExW(path,NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module||!GetModuleFileNameW(module,actual,MAX_PATH)||_wcsicmp(path,actual))return 4;
    typedef int (__cdecl *cmp_fn)(const void*,const void*);
    void *(__cdecl *search)(const void*,const void*,size_t,size_t,cmp_fn)=(void*(__cdecl*)(const void*,const void*,size_t,size_t,cmp_fn))GetProcAddress(module,"bsearch");
    get_errno=(int*(__cdecl*)(void))GetProcAddress(module,"_errno");
    typedef void (__cdecl *invalid_fn)(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t);
    invalid_fn (__cdecl *set_invalid)(invalid_fn)=(invalid_fn(__cdecl*)(invalid_fn))GetProcAddress(module,"_set_thread_local_invalid_parameter_handler");
    if(!search||!get_errno||!set_invalid)return 5;
    FILE *table_file=fopen(argv[1],"rb"),*input=fopen(argv[2],"rb"),*output=fopen(argv[3],"wb");
    if(!table_file||!input||!output)return 6;
    unsigned char table[2664],generic[4096];if(fread(table,1,sizeof(table),table_file)!=sizeof(table)||fgetc(table_file)!=EOF)return 7;
    fclose(table_file);invalid_fn old=set_invalid(invalid);unsigned cases=0,callbacks=0,exceptions=0,invalids=0;
    while(fread(&current,sizeof(current),1,input)==1){
        if(current.kind>3||current.flags>31||current.mode>2||current.count>666||current.width>64)return 8;
        memset(&result,0,sizeof(result));result.index=-1;for(unsigned j=0;j<16;++j)result.trace[j]=-1;
        memset(generic,0xa5,sizeof(generic));
        if(current.kind){
            if(current.count*current.width>sizeof(generic))return 9;
            for(size_t j=0;j<(current.width?current.count:1);++j){uint32_t value=current.kind==3?1:(uint32_t)(current.kind==2?j/3:j)*2+1;memcpy(generic+j*current.width,&value,4);}
        }
        base=current.kind?generic:table;
        const void *key=(current.flags&1)?NULL:&current.key,*array=(current.flags&2)?NULL:base;
        cmp_fn cmp=(current.flags&4)?NULL:compare;void *found=NULL;
        *get_errno()=1927;SetLastError(0x5a17);
        __try {found=search(key,array,(size_t)current.count,(size_t)current.width,cmp);}
        __except(EXCEPTION_EXECUTE_HANDLER){result.exception=GetExceptionCode();++exceptions;}
        result.last_error=GetLastError();result.error=*get_errno();result.traces=result.callbacks<16?result.callbacks:16;
        if(found){result.index=current.width?(int32_t)(((const unsigned char*)found-base)/current.width):0;memcpy(&result.record,found,4);}
        if(fwrite(&result,sizeof(result),1,output)!=1)return 10;
        callbacks+=result.callbacks;invalids+=result.invalid;++cases;
    }
    set_invalid(old);if(!feof(input)||fclose(input)||fclose(output))return 11;
    printf("{\"cases\":%u,\"comparator_calls\":%u,\"invalid_callbacks\":%u,\"SEH_exceptions\":%u,\"System32_module_verified\":true,\"input_bytes\":32,\"output_bytes\":64}\n",cases,callbacks,invalids,exceptions);return 0;
}
