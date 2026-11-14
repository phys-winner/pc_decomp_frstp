#include "cpu64.h"
#include <errno.h>
#include <stdio.h>
static int mobile;
int frostpunk_dxvk_mobile_effects(void){return mobile;}
unsigned fp_test_detail_mode(CPU*);
float fp_test_detail_threshold(float,unsigned);
static uint64_t fpcr(void){uint64_t v;__asm__ volatile("mrs %0,fpcr":"=r"(v));return v;}
static uint64_t fpsr(void){uint64_t v;__asm__ volatile("mrs %0,fpsr":"=r"(v));return v;}
static void floating(uint64_t c,uint64_t s){__asm__ volatile("msr fpcr,%0\nmsr fpsr,%1\nisb"::"r"(c),"r"(s):"memory");}
int main(int argc,char**argv){
    if(argc!=2)abort();int override=atoi(argv[1]);unsigned mode=0,cases=0;uint64_t stack[4]={0};
    const uint32_t patterns[]={0,0x80000000,0x3ba3d70a,0x3c23d70a,0x3f800000,0x7f800000,0xff800000,0x7fc13579,0x7f812345,0xff812345,1,0x807fffff};
    uint64_t saved_c=fpcr(),saved_s=fpsr();
    for(mobile=0;mobile<2;++mobile)for(unsigned f=0;f<16;++f)for(unsigned scope=0;scope<6;++scope)for(unsigned i=0;i<sizeof(patterns)/sizeof(patterns[0]);++i){
        mode=override>=0?(unsigned)override:(mobile?40:0);CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;stack[0]=scope==0?UINT64_C(0x140eb3f1a):UINT64_C(0x140eb3f1b);
        if(scope==2)c.rsp=0;else if(scope==3)c.rsp=UINT64_MAX-3;else if(scope==4)c.rsp=(uintptr_t)&c.rip;else if(scope==5)c.rsp=(uintptr_t)&c.rsp;
        CPU before=c;uint64_t initial_stack[4];memcpy(initial_stack,stack,sizeof(stack));
        uint64_t control=(saved_c&~UINT64_C(0x1fc0000))|((uint64_t)(f&3)<<22)|(f&4?UINT64_C(1)<<24:0)|(f&8?UINT64_C(1)<<25:0),status=0x95;
        errno=1931;floating(control,status);unsigned actual=fp_test_detail_mode(&c);float value;memcpy(&value,&patterns[i],4);
        float result=fp_test_detail_threshold(value,actual);uint32_t bits;memcpy(&bits,&result,4);
        unsigned wanted=scope==0?mode:0;float threshold=mode==10?0.01f:mode==20?0.02f:mode==30?0.03f:mode==40?0.04f:0;
        uint32_t expected=patterns[i];if(wanted)memcpy(&expected,&threshold,4);
        if(actual!=wanted||bits!=expected||errno!=1931||fpcr()!=control||fpsr()!=status||memcmp(&c,&before,sizeof(c))||memcmp(stack,initial_stack,sizeof(stack))){fprintf(stderr,"DETAIL_POLICY_FAIL f=%u scope=%u i=%u mode=%u/%u bits=%x/%x\n",f,scope,i,actual,wanted,bits,expected);abort();}++cases;
    }
    if(fp_test_detail_mode(NULL))abort();floating(saved_c,saved_s);printf("{\"success\":true,\"mode\":%u,\"cases\":%u,\"renderer_scope_only\":true,\"full_cpu_stack_fp_errno\":true,\"nan_bits_preserved_in_stock_mode\":true}\n",mode,cases);return 0;
}
