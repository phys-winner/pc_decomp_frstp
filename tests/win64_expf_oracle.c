/* Native UCRT only: scalar float argument/result in XMM0. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint32_t x,unused,mxcsr,tag;} input_row;
typedef struct {uint32_t x,unused,result,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t tag,reserved;} row;
_Static_assert(sizeof(row)==32,"expf row32");
struct _exception;
static unsigned hook_calls;
static int __cdecl zero_matherr(struct _exception *error){(void)error;++hook_calls;return 0;}
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    float(__cdecl *fn)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"expf");
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    void(__cdecl *set_matherr)(int(__cdecl*)(struct _exception*))=(void(__cdecl*)(int(__cdecl*)(struct _exception*)))GetProcAddress(crt,"__setusermatherr");
    if(!fn||!crt_errno||!set_matherr)return 5;
    input_row in;unsigned count=0,callback_cases=0;
    while(fread(&in,sizeof(in),1,input)==1){
        row out={in.x,in.unused,0,in.mxcsr,0,0,in.tag,0};float x,result;memcpy(&x,&in.x,4);
        set_matherr(in.tag&4?zero_matherr:NULL);hook_calls=0;_mm_setcsr(in.mxcsr);
        if(in.tag&0x80000000u){
            static const float coefficient[]={9.0f,16.0f,36.0f,25.0f,4.0f,1.0f};unsigned i=in.tag&7u;if(i>=6)return 8;
            float sigma;memcpy(&sigma,&in.unused,4);__m128 s=_mm_set_ss(sigma);
            __m128 denominator=_mm_mul_ss(_mm_mul_ss(s,_mm_set_ss(-2.0f)),s);
            __m128 q=_mm_div_ss(_mm_set_ss(1.0f),denominator);
            x=_mm_cvtss_f32(_mm_mul_ss(q,_mm_set_ss(coefficient[i])));memcpy(&out.x,&x,4);out.mxcsr=_mm_getcsr();
        }
        *crt_errno()=1379;SetLastError(0x5a17);result=fn(x);
        out.expected_mxcsr=_mm_getcsr();out.expected_errno=*crt_errno();if(GetLastError()!=0x5a17)return 6;
        out.reserved=hook_calls;if(hook_calls)++callback_cases;
        memcpy(&out.result,&result,4);if(fwrite(&out,sizeof(out),1,output)!=1)return 7;++count;
    }
    set_matherr(NULL);_mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u,\"native_return0_matherr_cases\":%u}\n",count,callback_cases);return 0;
}
