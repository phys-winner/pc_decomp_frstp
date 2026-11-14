#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {uint32_t kind,mxcsr;uint64_t input,aux;} math_input;
typedef struct {math_input in;uint64_t result,aux;uint32_t mxcsr;int32_t error;} math_row;
_Static_assert(sizeof(math_row)==48,"math row layout");
static const char *names[]={"exp","atan","coshf","sinhf","tanhf","frexp","modf","lroundf","lrintf","_fdclass","_dclass","_dsign","_fdsign","copysign","lgamma","lgammaf","lgammal","expf","exp2f","logf","log10f","atanf","fmodf","powf","atan2f","log","sin","cos","pow","ldexp"};
int main(int argc,char **argv){
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    HMODULE crt=LoadLibraryExW(L"ucrtbase.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!crt)return 4;
    int *(__cdecl *crt_errno)(void)=(int*(__cdecl*)(void))GetProcAddress(crt,"_errno");
    FARPROC functions[30];for(unsigned i=0;i<30;++i){functions[i]=GetProcAddress(crt,names[i]);if(!functions[i])return 5;}
    math_input in;unsigned count=0;
    while(fread(&in,sizeof(in),1,input)==1){
        if(in.kind>=30)return 6;
        math_row row={0};row.in=in;row.aux=UINT64_C(0xa5a5a5a5a5a5a5a5);
        double x,d;float f,y;memcpy(&x,&in.input,8);memcpy(&f,&in.input,4);
        _mm_setcsr(in.mxcsr);*crt_errno()=1379;SetLastError(0x5a17);
        if(in.kind>=17){
            if(in.kind<=21){y=((float(__cdecl*)(float))functions[in.kind])(f);memcpy(&row.result,&y,4);}
            else if(in.kind<=24){float other;memcpy(&other,&in.aux,4);y=((float(__cdecl*)(float,float))functions[in.kind])(f,other);memcpy(&row.result,&y,4);}
            else{double other;memcpy(&other,&in.aux,8);
                if(in.kind<=27)d=((double(__cdecl*)(double))functions[in.kind])(x);
                else if(in.kind==28)d=((double(__cdecl*)(double,double))functions[in.kind])(x,other);
                else d=((double(__cdecl*)(double,int))functions[in.kind])(x,(int32_t)in.aux);
                memcpy(&row.result,&d,8);}
        }
        else if(in.kind<2){d=((double(__cdecl*)(double))functions[in.kind])(x);memcpy(&row.result,&d,8);}
        else if(in.kind<5){y=((float(__cdecl*)(float))functions[in.kind])(f);memcpy(&row.result,&y,4);}
        else if(in.kind==5){int exponent=0;d=((double(__cdecl*)(double,int*))functions[5])(x,&exponent);memcpy(&row.result,&d,8);row.aux=(uint32_t)exponent;}
        else if(in.kind==6){double integral;d=((double(__cdecl*)(double,double*))functions[6])(x,&integral);memcpy(&row.result,&d,8);memcpy(&row.aux,&integral,8);}
        else if(in.kind<9)row.result=(uint32_t)((long(__cdecl*)(float))functions[in.kind])(f);
        else if(in.kind==9||in.kind==12)row.result=(uint32_t)(int32_t)((short(__cdecl*)(float))functions[in.kind])(f);
        else if(in.kind==10||in.kind==11)row.result=(uint32_t)(int32_t)((short(__cdecl*)(double))functions[in.kind])(x);
        else if(in.kind==13){double sign;memcpy(&sign,&in.aux,8);d=((double(__cdecl*)(double,double))functions[13])(x,sign);memcpy(&row.result,&d,8);}
        else if(in.kind==15){y=((float(__cdecl*)(float))functions[15])(f);memcpy(&row.result,&y,4);}
        else{d=((double(__cdecl*)(double))functions[in.kind])(x);memcpy(&row.result,&d,8);}
        row.mxcsr=_mm_getcsr();row.error=*crt_errno();if(GetLastError()!=0x5a17)return 7;
        if(fwrite(&row,sizeof(row),1,output)!=1)return 8;++count;
    }
    _mm_setcsr(0x1f80);fclose(input);fclose(output);printf("{\"success\":true,\"cases\":%u}\n",count);return 0;
}
