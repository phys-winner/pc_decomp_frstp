/* System VCRUNTIME only; synthetic polymorphic data, no original code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef struct{uint32_t table,source,target,kind,delta,reference,mode,seed;} input_row;
typedef struct{input_row input;uint64_t expected;uint32_t error;int32_t native_errno;uint32_t csr_after,reserved;} row;
_Static_assert(sizeof(input_row)==32,"RTTI input32");
_Static_assert(sizeof(row)==56,"RTTI row56");
static void *load(const char*path,size_t *bytes){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);void*p=malloc((size_t)n);if(n<=0||!p||fread(p,(size_t)n,1,f)!=1)return NULL;fclose(f);*bytes=(size_t)n;return p;}
int main(int argc,char**argv){
    if(argc!=4)return 2;size_t image_bytes;unsigned char*image=load(argv[1],&image_bytes);if(!image||image_bytes>UINT32_MAX)return 3;
    HMODULE crt=LoadLibraryExW(L"vcruntime140.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!crt)return 4;
    void*(__cdecl *fn)(void*,LONG,void*,void*,BOOL)=(void*(__cdecl*)(void*,LONG,void*,void*,BOOL))GetProcAddress(crt,"__RTDynamicCast");if(!fn)return 5;
    FILE*in=fopen(argv[2],"rb"),*out=fopen(argv[3],"wb");if(!in||!out)return 6;
    input_row input;unsigned count=0,nonnull=0,nulls=0;unsigned original=_mm_getcsr();
    while(fread(&input,sizeof(input),1,in)==1){
        if(input.table<8||input.table>image_bytes-8)return 7;
        uint64_t locator;memcpy(&locator,image+input.table-8,8);if(locator<image_bytes){locator+=(uintptr_t)image;memcpy(image+input.table-8,&locator,8);}
        uint64_t object[8]={0};object[0]=(uintptr_t)(image+input.table);
        unsigned csr=0x1f80|((input.mode&3u)<<13)|((input.mode&4u)?64:0)|((input.mode&8u)?32768:0)|0x15;
        _mm_setcsr(csr);errno=2718;SetLastError(0x5a17);
        void*value=fn(input.kind?NULL:object,(LONG)input.delta,(void*)((uintptr_t)image+input.source),(void*)((uintptr_t)image+input.target),(BOOL)input.reference);
        row result={input,0,GetLastError(),errno,_mm_getcsr(),0};
        if(value){if(value!=object)return 8;result.expected=UINT64_MAX;++nonnull;}else ++nulls;
        if(result.error!=0x5a17||result.native_errno!=2718||result.csr_after!=csr)return 9;
        if(fwrite(&result,sizeof(result),1,out)!=1)return 10;++count;
    }
    _mm_setcsr(original);fclose(in);if(fclose(out))return 11;free(image);
    printf("{\"success\":true,\"cases\":%u,\"nonnull\":%u,\"null\":%u,\"native_environment_preserved\":true}\n",count,nonnull,nulls);return 0;
}
