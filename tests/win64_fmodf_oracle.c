/* Native UCRT only, typed binary32 remainder ABI. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint32_t x,y,mxcsr,tag;} input_row;
typedef struct {uint32_t x,y,result,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t tag,reserved;} row;
_Static_assert(sizeof(row)==32,"fmodf row32");
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    float(__cdecl *fn)(float,float)=(float(__cdecl*)(float,float))GetProcAddress(crt,"fmodf");
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");if(!fn||!crt_errno)return 5;
    input_row in;unsigned count=0;
    while(fread(&in,sizeof(in),1,input)==1){
        row out={in.x,in.y,0,in.mxcsr,0,0,in.tag,0};float x,y,result;memcpy(&x,&in.x,4);memcpy(&y,&in.y,4);
        _mm_setcsr(in.mxcsr);*crt_errno()=1379;SetLastError(0x5a17);result=fn(x,y);
        out.expected_mxcsr=_mm_getcsr();out.expected_errno=*crt_errno();if(GetLastError()!=0x5a17)return 6;
        memcpy(&out.result,&result,4);if(fwrite(&out,sizeof(out),1,output)!=1)return 7;++count;
    }
    _mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u}\n",count);return 0;
}
