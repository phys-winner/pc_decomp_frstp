/* Native Windows UCRT oracle. Only the child process installs matherr hooks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
typedef struct {uint64_t bits;uint32_t is_double,mxcsr;int32_t initial_errno;uint32_t callback_mode;} input_case;
typedef struct {uint64_t bits;uint32_t mxcsr;int32_t error;uint32_t callback_count;int32_t exception_type;uint64_t argument1,argument2,retval;char name[16];} output_case;
static output_case *current;
static int callback_mode;
static int __cdecl error_callback(struct _exception *e){
    ++current->callback_count;current->exception_type=e->type;
    memcpy(&current->argument1,&e->arg1,8);memcpy(&current->argument2,&e->arg2,8);memcpy(&current->retval,&e->retval,8);
    if(e->name){size_t n=strlen(e->name);if(n>15)n=15;memcpy(current->name,e->name,n);}
    if(callback_mode==2){e->retval=123.5;return 1;}return 0;
}
int main(int argc,char**argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryW(L"ucrtbase.dll");if(!crt)return 4;
    float (__cdecl *rootf)(float)=(float(__cdecl*)(float))GetProcAddress(crt,"sqrtf");
    double (__cdecl *root)(double)=(double(__cdecl*)(double))GetProcAddress(crt,"sqrt");
    void (__cdecl *set_handler)(int(__cdecl*)(struct _exception*))=(void(__cdecl*)(int(__cdecl*)(struct _exception*)))GetProcAddress(crt,"__setusermatherr");
    int* (__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    if(!rootf||!root||!set_handler||!crt_errno)return 5;
    input_case in;unsigned count=0,callbacks=0;
    while(fread(&in,sizeof(in),1,input)==1){
        output_case out={0};current=&out;callback_mode=(int)in.callback_mode;set_handler(in.callback_mode?error_callback:NULL);
        *crt_errno()=in.initial_errno;_mm_setcsr(in.mxcsr);
        if(in.is_double){double x,y;memcpy(&x,&in.bits,8);y=root(x);memcpy(&out.bits,&y,8);}
        else{uint32_t bits=(uint32_t)in.bits;float x,y;memcpy(&x,&bits,4);y=rootf(x);memcpy(&out.bits,&y,4);}
        out.mxcsr=_mm_getcsr();out.error=*crt_errno();callbacks+=out.callback_count;
        if(fwrite(&out,sizeof(out),1,output)!=1)return 6;++count;
    }
    set_handler(NULL);_mm_setcsr(0x1f80);fclose(input);fclose(output);
    printf("{\"cases\":%u,\"callbacks\":%u,\"input_size\":%zu,\"output_size\":%zu}\n",count,callbacks,sizeof(input_case),sizeof(output_case));return 0;
}
