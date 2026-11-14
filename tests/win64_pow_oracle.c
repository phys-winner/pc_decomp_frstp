/* Native UCRT, with the original codec's scalar argument-construction steps. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int32_t entries,dimension;uint32_t mxcsr,route;} input_row;
typedef struct {uint64_t x,y,result;uint32_t mxcsr,route;int32_t entries,dimension;uint32_t expected_mxcsr;int32_t expected_errno;} row;
_Static_assert(sizeof(row)==48,"pow row48");
int main(int argc,char **argv) {
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    double (__cdecl *function)(double,double)=(double(__cdecl*)(double,double))GetProcAddress(crt,"pow");
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");if(!function||!crt_errno)return 5;
    input_row in;unsigned count=0;
    while(fread(&in,sizeof(in),1,input)==1) {
        row out={0};_mm_setcsr(in.mxcsr);
        __m128 dimension=_mm_cvtepi32_ps(_mm_cvtsi32_si128(in.dimension));
        __m128 base=_mm_cvtepi32_ps(_mm_cvtsi32_si128(in.entries));
        __m128 reciprocal=_mm_div_ss(_mm_set_ss(1.0f),dimension);
        double x=_mm_cvtsd_f64(_mm_cvtps_pd(base)),y=_mm_cvtsd_f64(_mm_cvtps_pd(reciprocal));
        memcpy(&out.x,&x,8);memcpy(&out.y,&y,8);out.mxcsr=_mm_getcsr();
        *crt_errno()=1379;SetLastError(0x5a17);double result=function(x,y);
        out.expected_mxcsr=_mm_getcsr();out.expected_errno=*crt_errno();memcpy(&out.result,&result,8);
        if(GetLastError()!=0x5a17)return 6;
        out.route=in.route;out.entries=in.entries;out.dimension=in.dimension;
        if(fwrite(&out,sizeof(out),1,output)!=1)return 7;++count;
    }
    _mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u}\n",count);return 0;
}
