/* Native UCRT pow only; data comes from new C, never original game code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint64_t x,y;uint32_t csr,route;} input_row;
typedef struct {uint64_t x,y,result;uint32_t csr,expected_csr,route,error;int32_t expected_errno;uint32_t exception;} row;
_Static_assert(sizeof(row)==48,"zero pow row48");
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE module=LoadLibraryW(L"ucrtbase.dll");if(!module)return 4;
    double(__cdecl *fn)(double,double)=(double(__cdecl*)(double,double))GetProcAddress(module,"pow");
    int*(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(module,"_errno");if(!fn||!crt_errno)return 5;
    input_row in;unsigned count=0,exceptions=0;
    while(fread(&in,sizeof(in),1,input)==1){row r={0};r.x=in.x;r.y=in.y;r.csr=in.csr;r.route=in.route;double x,y,result;
        memcpy(&x,&in.x,8);memcpy(&y,&in.y,8);*crt_errno()=1379;SetLastError(0x5a17);_mm_setcsr(in.csr);
        __try{result=fn(x,y);memcpy(&r.result,&result,8);}
        __except(EXCEPTION_EXECUTE_HANDLER){r.exception=GetExceptionCode();++exceptions;}
        r.expected_csr=_mm_getcsr();r.expected_errno=*crt_errno();r.error=GetLastError();_mm_setcsr(0x1f80);
        if(fwrite(&r,sizeof(r),1,output)!=1)return 6;++count;
    }
    fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u,\"exceptions\":%u}\n",count,exceptions);return 0;
}
