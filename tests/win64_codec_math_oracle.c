/* Native UCRT only; original game instructions are inspected, never executed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint64_t x;uint32_t kind,mxcsr;} input_row;
typedef struct {uint64_t x,result;uint32_t kind,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t float_result,reserved;} row;
_Static_assert(sizeof(row)==40,"codec row40");
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    const char *names[]={"log","cos","sin"};double(__cdecl *function[3])(double);
    for(unsigned i=0;i<3;++i){function[i]=(double(__cdecl*)(double))GetProcAddress(crt,names[i]);if(!function[i])return 5;}
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");if(!crt_errno)return 5;
    input_row in;unsigned count=0;
    while(fread(&in,sizeof(in),1,input)==1){
        unsigned kind=in.kind&255u,construction=in.kind>>8;if(kind>=3||construction>4)return 6;
        row out={0};out.kind=kind;double x,y;memcpy(&x,&in.x,8);_mm_setcsr(in.mxcsr);
        if(construction){
            int n=(int)(uint32_t)in.x,index=(int)(uint32_t)(in.x>>32);
            __m128 size=_mm_cvtepi32_ps(_mm_cvtsi32_si128(n));
            if(construction==1)x=_mm_cvtsd_f64(_mm_cvtps_pd(size));
            else{
                uint32_t pi_bits=0x40490fdb;float pi;memcpy(&pi,&pi_bits,4);
                int multiplier=construction==2?4*index:construction==3?2*index+1:4*index+2;
                if(construction==3)size=_mm_cvtepi32_ps(_mm_cvtsi32_si128(2*n));
                __m128 step=_mm_div_ss(_mm_set_ss(pi),size);
                __m128 angle=_mm_mul_ss(_mm_cvtepi32_ps(_mm_cvtsi32_si128(multiplier)),step);
                x=_mm_cvtsd_f64(_mm_cvtps_pd(angle));
            }
        }
        memcpy(&out.x,&x,8);out.mxcsr=_mm_getcsr();*crt_errno()=1379;SetLastError(0x5a17);
        y=function[kind](x);out.expected_mxcsr=_mm_getcsr();out.expected_errno=*crt_errno();
        if(GetLastError()!=0x5a17)return 7;memcpy(&out.result,&y,8);
        float narrowed=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(y)));memcpy(&out.float_result,&narrowed,4);
        if(!kind){
            double log_two=function[0](2.0);
            __m128d ratio=_mm_div_sd(_mm_set_sd(y),_mm_set_sd(log_two));
            double sum=_mm_cvtsd_f64(_mm_add_sd(ratio,_mm_set_sd(0.5)));
            double(__cdecl *floor_fn)(double)=(double(__cdecl*)(double))GetProcAddress(crt,"floor");
            out.reserved=(uint32_t)_mm_cvttsd_si32(_mm_set_sd(floor_fn(sum)));
        }
        if(fwrite(&out,sizeof(out),1,output)!=1)return 8;++count;
    }
    _mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u}\n",count);return 0;
}
