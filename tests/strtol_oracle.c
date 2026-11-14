/* Installed Windows UCRT, explicit C locale, LONG32 and actual full RAX. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
extern uint64_t native_strtol_rax(void*,const char*,char**,int);
typedef struct {int32_t base,seed;uint32_t mxcsr,pad;char text[256];} input_row;
typedef struct {uint64_t result;int32_t error,end;uint32_t mxcsr,last_error;} output_row;
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    void *functions[]={(void*)GetProcAddress(crt,"strtol"),(void*)GetProcAddress(crt,"strtoul")};int*(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    char*(__cdecl *locale)(int,const char*)=(char*(__cdecl*)(int,const char*))GetProcAddress(crt,"setlocale");if(!functions[0]||!functions[1]||!crt_errno||!locale||!locale(0,"C"))return 5;
    input_row in;unsigned count=0;
    while(fread(&in,sizeof(in),1,input)==1){
        output_row out={0};char *end=(char*)(uintptr_t)0x1234;*crt_errno()=in.seed;SetLastError(0x5a17);_mm_setcsr(in.mxcsr);
        out.result=native_strtol_rax(functions[in.pad&1u],in.text,(in.pad&2u)?NULL:&end,in.base);out.mxcsr=_mm_getcsr();out.error=*crt_errno();out.last_error=GetLastError();out.end=(in.pad&2u)?-1:(int32_t)(end-in.text);_mm_setcsr(0x1f80);
        if(fwrite(&out,sizeof(out),1,output)!=1)return 6;++count;
    }
    fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u,\"native_LONG_bytes\":%u,\"locale\":\"C\",\"full_RAX\":true}\n",count,(unsigned)sizeof(long));return 0;
}
