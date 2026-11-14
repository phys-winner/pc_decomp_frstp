/* Typed UCRT ldexp on the original codec's packed-float argument profile. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
#include <emmintrin.h>
typedef struct {uint32_t word,mxcsr;uint64_t upper;} input_row;
typedef struct {uint64_t x,result,rdx;uint32_t mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t word;} row;
_Static_assert(sizeof(row)==40,"ldexp row40");
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    double(__cdecl *fn)(double,int)=(double(__cdecl*)(double,int))GetProcAddress(crt,"ldexp");
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");if(!fn||!crt_errno)return 5;
    input_row in;unsigned count=0;
    while(fread(&in,sizeof(in),1,input)==1){
        row out={0};out.word=in.word;_mm_setcsr(in.mxcsr);
        double x=_mm_cvtsd_f64(_mm_cvtepi32_pd(_mm_cvtsi32_si128((int)(in.word&0x1fffff))));
        memcpy(&out.x,&x,8);if(in.word>>31)out.x^=UINT64_C(0x8000000000000000);memcpy(&x,&out.x,8);
        int exponent=(int)((in.word>>21)&0x3ff)-788;out.rdx=(in.upper&UINT64_C(0xffffffff00000000))|(uint32_t)exponent;
        out.mxcsr=_mm_getcsr();*crt_errno()=1379;SetLastError(0x5a17);double result=fn(x,exponent);
        out.expected_mxcsr=_mm_getcsr();out.expected_errno=*crt_errno();if(GetLastError()!=0x5a17)return 6;
        memcpy(&out.result,&result,8);if(fwrite(&out,sizeof(out),1,output)!=1)return 7;++count;
    }
    _mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u}\n",count);return 0;
}
