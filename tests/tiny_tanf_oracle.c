/* Real Windows UCRT float-tangent oracle; no original game code executes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
typedef struct {uint32_t bits,mxcsr;} input_row;
typedef struct {uint32_t bits,mxcsr;int32_t error;uint32_t last_error,calls;} output_row;
static unsigned callbacks;
static int __cdecl matherr(struct _exception *e){(void)e;++callbacks;return 0;}
int main(int argc,char**argv){
    if(argc!=3)return 2;FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out)return 3;
    HMODULE crt=LoadLibraryExW(L"ucrtbase.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!crt)return 4;
    float(__cdecl *fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"tanf");
    int*(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    void(__cdecl *set_handler)(int(__cdecl*)(struct _exception*))=(void(__cdecl*)(int(__cdecl*)(struct _exception*)))GetProcAddress(crt,"__setusermatherr");
    if(!fn||!crt_errno||!set_handler)return 5;set_handler(matherr);unsigned saved=_mm_getcsr(),count=0;input_row row;
    while(fread(&row,sizeof(row),1,in)==1){
        float x,y;memcpy(&x,&row.bits,4);callbacks=0;*crt_errno()=1379;SetLastError(0x5a17);_mm_setcsr(row.mxcsr);
        y=fn(x);output_row result;memcpy(&result.bits,&y,4);result.mxcsr=_mm_getcsr();result.error=*crt_errno();result.last_error=GetLastError();result.calls=callbacks;
        if(fwrite(&result,sizeof(result),1,out)!=1)return 6;++count;
    }
    _mm_setcsr(saved);set_handler(NULL);fclose(in);fclose(out);printf("{\"success\":true,\"cases\":%u,\"LastError_checked\":true,\"UCRT_errno_checked\":true}\n",count);return 0;
}
