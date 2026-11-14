/* Native system UCRT oracle. No original game code is executed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t input, mxcsr; } input_row;
typedef struct {
    uint32_t input, mxcsr, result, integral, expected_mxcsr;
    int32_t expected_errno;
    uint32_t matherr_calls, last_error;
} row;
_Static_assert(sizeof(row)==32,"modff oracle row32");
struct _exception;
static unsigned matherr_calls;
static int __cdecl matherr(struct _exception *exception) { (void)exception; ++matherr_calls; return 0; }

int main(int argc,char **argv) {
    if(argc!=3)return 2;
    FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");
    if(!in||!out)return 3;
    HMODULE crt=LoadLibraryExW(L"ucrtbase.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!crt)return 4;
    float(__cdecl *fn)(float,float*)=(float(__cdecl*)(float,float*))GetProcAddress(crt,"modff");
    int*(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    void(__cdecl *set_matherr)(int(__cdecl*)(struct _exception*))=(void(__cdecl*)(int(__cdecl*)(struct _exception*)))GetProcAddress(crt,"__setusermatherr");
    if(!fn||!crt_errno||!set_matherr)return 5;
    set_matherr(matherr);input_row input;unsigned count=0,callbacks=0;
    while(fread(&input,sizeof(input),1,in)==1) {
        row value={input.input,input.mxcsr,0,0,0,0,0,0};
        float x,result,integral;memcpy(&x,&input.input,4);
        _mm_setcsr(input.mxcsr);*crt_errno()=1379;SetLastError(0x5a17);matherr_calls=0;
        result=fn(x,&integral);value.expected_mxcsr=_mm_getcsr();
        value.expected_errno=*crt_errno();value.matherr_calls=matherr_calls;value.last_error=GetLastError();
        memcpy(&value.result,&result,4);memcpy(&value.integral,&integral,4);
        if(fwrite(&value,sizeof(value),1,out)!=1)return 6;
        ++count;callbacks+=matherr_calls;
    }
    _mm_setcsr(0x1f80);set_matherr(NULL);fclose(in);fclose(out);
    printf("{\"success\":true,\"cases\":%u,\"matherr_calls\":%u,\"original_game_executed\":false}\n",count,callbacks);
    return 0;
}
