#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#ifdef DIALOG_MATH_ORACLE
#include <xmmintrin.h>
#else
#include "win64_math.h"
static int guest_errno;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return 0;}
void *win64_pointer(uint64_t p,size_t n,int w){(void)n;(void)w;return(void*)(uintptr_t)p;}
#endif
typedef struct{uint64_t x,y,result;uint32_t kind,csr,expected_csr;int32_t error;} Case;
_Static_assert(sizeof(Case)==40,"dialog math protocol");
static uint64_t ordered(uint64_t x,unsigned single){uint64_t sign=single?0x80000000u:UINT64_C(0x8000000000000000);return x&sign?~x: x|sign;}
int main(int argc,char**argv){
    if(argc!=3)return 2;
    FILE *input=fopen(argv[1],"rb"),*output=fopen(argv[2],"wb");if(!input||!output)return 3;
    unsigned count=0,failures=0;uint64_t worst=0;Case t;
    while(fread(&t,sizeof(t),1,input)==1){
#ifdef DIALOG_MATH_ORACLE
        _mm_setcsr(t.csr);errno=1379;
        if(t.kind){uint32_t x=(uint32_t)t.x,r;float f,z;memcpy(&f,&x,4);float(*volatile fn)(float)=log10f;z=fn(f);memcpy(&r,&z,4);t.result=r;}
        else{double x,y,z;memcpy(&x,&t.x,8);memcpy(&y,&t.y,8);double(*volatile fn)(double,double)=pow;z=fn(x,y);memcpy(&t.result,&z,8);}
        t.expected_csr=_mm_getcsr();t.error=errno;fwrite(&t,sizeof(t),1,output);
#else
        CPU c,before;memset(&c,0xa5,sizeof(c));c.mxcsr=t.csr;
        if(t.kind)c.xmm[0].u32[0]=(uint32_t)t.x;else{c.xmm[0].u64[0]=t.x;c.xmm[1].u64[0]=t.y;}
        before=c;guest_errno=1379;errno=791;
        uint64_t old_control,old_status,control,status;
        __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(old_control),"=r"(old_status));
        if(!win64_math_call(&c,t.kind?"log10f":"pow"))return 4;
        __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
        uint64_t result=t.kind?c.xmm[0].u32[0]:c.xmm[0].u64[0],a=ordered(result,t.kind),b=ordered(t.result,t.kind),ulp=a>b?a-b:b-a;
        unsigned csr=c.mxcsr;c.mxcsr=before.mxcsr;c.xmm[0]=before.xmm[0];
        if(ulp>worst)worst=ulp;
        if(ulp>4||csr!=t.expected_csr||guest_errno!=t.error||errno!=791||memcmp(&c,&before,sizeof(c))||old_control!=control||old_status!=status){
            fprintf(stderr,"DIALOG_MATH_FAIL case=%u kind=%u x=%llx y=%llx ULP=%llu csr=%x expected=%x errno=%d expected=%d\n",count,t.kind,(unsigned long long)t.x,(unsigned long long)t.y,(unsigned long long)ulp,csr,t.expected_csr,guest_errno,t.error);++failures;
        }
#endif
        ++count;if(failures>=8)break;
    }
    fclose(input);fclose(output);
    printf("{\"success\":%s,\"cases\":%u,\"failures\":%u,\"max_ulp\":%llu}\n",failures?"false":"true",count,failures,(unsigned long long)worst);
    return failures?1:0;
}
