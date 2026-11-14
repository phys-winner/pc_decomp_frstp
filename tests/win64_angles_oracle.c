/* System UCRT scalar float math only. Original game code is never executed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct{uint32_t y,x,mxcsr,kind;} input_row;
typedef struct{uint32_t y,x,result,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t kind,matherr_calls;} row;
_Static_assert(sizeof(row)==32,"angle row32");
struct _exception;static unsigned calls;
static int __cdecl default_matherr(struct _exception *error){(void)error;++calls;return 0;}
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryExW(L"ucrtbase.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!crt)return 4;
    float(__cdecl *atan_fn)(float,float)=(float(__cdecl*)(float,float))GetProcAddress(crt,"atan2f");
    float(__cdecl *asin_fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"asinf");
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    void(__cdecl *set_matherr)(int(__cdecl*)(struct _exception*))=(void(__cdecl*)(int(__cdecl*)(struct _exception*)))GetProcAddress(crt,"__setusermatherr");
    if(!atan_fn||!asin_fn||!crt_errno||!set_matherr)return 5;set_matherr(default_matherr);
    input_row in;unsigned count=0,callbacks=0;while(fread(&in,sizeof(in),1,input)==1){
        row out={in.y,in.x,0,in.mxcsr,0,0,in.kind,0};float y,x,value;memcpy(&y,&in.y,4);memcpy(&x,&in.x,4);
        _mm_setcsr(in.mxcsr);*crt_errno()=1379;SetLastError(0x5a17);calls=0;
        value=in.kind?asin_fn(y):atan_fn(y,x);out.expected_mxcsr=_mm_getcsr();out.expected_errno=*crt_errno();out.matherr_calls=calls;
        if(GetLastError()!=0x5a17)return 6;memcpy(&out.result,&value,4);if(fwrite(&out,sizeof(out),1,output)!=1)return 7;++count;callbacks+=calls;
    }
    set_matherr(NULL);_mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u,\"matherr_calls\":%u,\"native_LastError_preserved\":true,\"original_game_executed\":false}\n",count,callbacks);return 0;
}
