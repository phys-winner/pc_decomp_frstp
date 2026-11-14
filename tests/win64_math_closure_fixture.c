#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <math.h>

static int guest_errno;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return 0;}
void *win64_pointer(uint64_t pointer,size_t bytes,int write){(void)bytes;(void)write;return(void*)(uintptr_t)pointer;}
static const char *names[]={"exp","atan","coshf","sinhf","tanhf","frexp","modf","lroundf","lrintf","_fdclass","_dclass","_dsign","_fdsign","copysign","lgamma","lgammaf","lgammal","expf","exp2f","logf","log10f","atanf","fmodf","powf","atan2f","log","sin","cos","pow","ldexp"};
typedef struct {uint32_t kind,mxcsr;uint64_t input,aux,result,output;uint32_t after;int32_t error;} row;
_Static_assert(sizeof(row)==48,"math row layout");
static uint64_t ordered(uint64_t value,unsigned wide){
    uint64_t sign=wide?UINT64_C(0x8000000000000000):UINT64_C(0x80000000);
    return value&sign?~value&(wide?UINT64_MAX:UINT32_MAX):value|sign;
}
static void environment(uint64_t out[2]){__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));}
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *file=fopen(argv[1],"rb"),*observed=fopen(argv[2],"wb");if(!file||!observed)return 3;
    unsigned coverage=0,failures=0,count=0,status_diff[30]={0},maximum[30]={0},failed[30]={0};
    win64_math_self_test();
    const char *imports[]={
#include "math_import_names.inc"
    };
    for(unsigned i=0;i<sizeof(imports)/sizeof(*imports);++i){
        CPU c={0};c.mxcsr=0x1f80;uint64_t output=0;float input=1;
        const char *n=imports[i];
        unsigned wide=!strcmp(n,"exp")||!strcmp(n,"atan")||!strcmp(n,"cos")||!strcmp(n,"sin")||!strcmp(n,"log")||!strcmp(n,"pow")
            ||!strcmp(n,"frexp")||!strcmp(n,"modf")||!strcmp(n,"ldexp")||!strcmp(n,"exp2")||!strcmp(n,"round")||!strcmp(n,"ceil")||!strcmp(n,"floor")||!strcmp(n,"sqrt")||!strcmp(n,"log2l")
            ||!strcmp(n,"_dclass")||!strcmp(n,"_dsign")||!strcmp(n,"copysign")||!strcmp(n,"lgamma")||!strcmp(n,"lgammal");
        if(wide)c.xmm[0].u64[0]=UINT64_C(0x3ff0000000000000);else c.xmm[0].u32[0]=0x3f800000;
        c.xmm[1]=c.xmm[0];c.rcx=(uintptr_t)&input;c.rdx=!strcmp(n,"ldexp")?1:(uintptr_t)&output;
        if(!win64_math_call(&c,n)){fprintf(stderr,"MISSING_MATH %s\n",n);return 4;}++coverage;
    }
    row r;
    while(fread(&r,sizeof(r),1,file)==1){
        if(r.kind>=30)return 5;
        CPU c,expected;memset(&c,0xa5,sizeof(c));c.mxcsr=r.mxcsr;c.xmm[0].u64[0]=r.input;
        uint64_t output=UINT64_C(0xa5a5a5a5a5a5a5a5);c.rdx=(uintptr_t)&output;expected=c;
        unsigned wide=r.kind<2||r.kind==5||r.kind==6||r.kind==10||r.kind==11||r.kind==13||r.kind==14||r.kind==16||r.kind>=25,integer=r.kind>=7&&r.kind<=12;
        if(r.kind==29)c.rdx=expected.rdx=(uint32_t)r.aux;
        c.xmm[1].u64[0]=expected.xmm[1].u64[0]=r.aux;
        guest_errno=1379;errno=2718;uint64_t before[2],after[2];environment(before);
        int handled=win64_math_call(&c,names[r.kind]);environment(after);
        uint64_t actual=integer?c.rax:wide?c.xmm[0].u64[0]:c.xmm[0].u32[0];
        uint64_t a=ordered(actual,wide),b=ordered(r.result,wide),delta=a>b?a-b:b-a;
        if(integer){delta=actual==r.result?0:UINT64_MAX;expected.rax=c.rax;}
        else if(wide)expected.xmm[0].u64[0]=actual;else expected.xmm[0].u32[0]=(uint32_t)actual;
        expected.mxcsr=c.mxcsr;
        unsigned status=c.mxcsr!=r.after;if(status)++status_diff[r.kind];
        unsigned transcend=r.kind<5||r.kind>=14;
        unsigned numeric=delta<=(transcend?4u:0u);
        if(r.kind>=14&&r.kind<=16){
            double actual_number,reference_number;
            if(wide){memcpy(&actual_number,&actual,8);memcpy(&reference_number,&r.result,8);}
            else{uint32_t low=(uint32_t)actual,ref=(uint32_t)r.result;float a,b;memcpy(&a,&low,4);memcpy(&b,&ref,4);actual_number=a;reference_number=b;}
            if(isfinite(actual_number)&&isfinite(reference_number))numeric=fabs(actual_number-reference_number)<=(wide?4e-14:4e-7)*fmax(1,fabs(reference_number));
        }
        unsigned okay=handled&&numeric&&!memcmp(&c,&expected,sizeof(c))&&errno==2718&&guest_errno==r.error&&!memcmp(before,after,sizeof(before));
        if(r.kind==5)okay&=(uint32_t)output==r.output;
        if(r.kind==6)okay&=output==r.output;
        if(!transcend)okay&=!status;
        if(delta<UINT32_MAX&&delta>maximum[r.kind])maximum[r.kind]=(unsigned)delta;
        if(!okay)++failures;
        if(!okay&&failed[r.kind]++<3)fprintf(stderr,"MATH_FAIL %s input=%llx CSR=%x actual=%llx expected=%llx ULP=%llu status=%x/%x output=%llx/%llx errno=%d/%d CPU=%d nativeFP=%d\n",
            names[r.kind],(unsigned long long)r.input,r.mxcsr,(unsigned long long)actual,(unsigned long long)r.result,(unsigned long long)delta,c.mxcsr,r.after,(unsigned long long)output,(unsigned long long)r.output,guest_errno,r.error,memcmp(&c,&expected,sizeof(c)),memcmp(before,after,sizeof(before)));
        row actual_row=r;actual_row.result=actual;actual_row.after=c.mxcsr;actual_row.error=guest_errno;
        if(r.kind==5)actual_row.output=(uint32_t)output;else if(r.kind==6)actual_row.output=output;
        if(fwrite(&actual_row,sizeof(actual_row),1,observed)!=1)return 6;++count;
    }
    fclose(file);fclose(observed);printf("{\"success\":%s,\"cases\":%u,\"imported_math_functions\":%u,\"failures\":%u,\"functions\":[",failures?"false":"true",count,coverage,failures);
    for(unsigned i=0;i<30;++i)printf("%s{\"name\":\"%s\",\"max_ULP\":%u,\"status_differences\":%u,\"failures\":%u}",i?",":"",names[i],maximum[i],status_diff[i],failed[i]);
    puts("]}");return failures?1:0;
}
