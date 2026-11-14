#include "cpu64.h"
#include "bnd_cases.h"
#include <inttypes.h>
#include <stdio.h>
int64_t g_image_delta;
extern void (*const bnd_functions[])(CPU *);
extern void (*const bnd_callees[])(CPU *);
static unsigned active,dispatches;
void frostpunk_unsupported(uint64_t va,const char *text) {fprintf(stderr,"Unsupported BND probe %" PRIx64 ":%s\n",va,text);abort();}
void frostpunk_cpuid(CPU *c){(void)c;abort();}
uint64_t frostpunk_teb_read(uint64_t off,unsigned bytes){(void)off;(void)bytes;abort();}
void frostpunk_teb_write(uint64_t off,uint64_t value,unsigned bytes){(void)off;(void)value;(void)bytes;abort();}
void dispatch(CPU *c,uint64_t target) {
    if(target!=bnd_variants[active].callee || !bnd_callees[active] || dispatches++)abort();
    bnd_callees[active](c);
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
int main(void) {
    uint64_t checksum=UINT64_C(14695981039346656037);
    for(unsigned i=0;i<sizeof(bnd_cases)/sizeof(bnd_cases[0]);++i) {
        const struct bnd_case *test=&bnd_cases[i];active=test->form;
        const struct bnd_variant *variant=&bnd_variants[active];
        _Alignas(16) uint64_t stack[64];uint64_t gpr[16],actual[16],xmm[32];
        for(unsigned r=0;r<16;++r)gpr[r]=UINT64_C(0x9e3779b97f4a7c15)^((uint64_t)r*UINT64_C(0x0102040810204081))^((uint64_t)i<<32);
        for(unsigned k=0;k<32;++k)xmm[k]=UINT64_C(0xabcd12345678eeff)^((uint64_t)k*UINT64_C(0x1122448811224488));
        for(unsigned k=0;k<64;++k)stack[k]=UINT64_C(0x5566778811223344)^k;
        if(variant->indirect)gpr[3]=variant->callee;
        uint64_t initial_rsp=(uintptr_t)&stack[16];gpr[4]=initial_rsp;
        CPU c={0};memcpy(&c.rax,gpr,sizeof(gpr));memcpy(c.xmm,xmm,sizeof(xmm));eflags_unpack(&c,test->flags);dispatches=0;
        bnd_functions[active](&c);memcpy(actual,&c.rax,sizeof(actual));
        for(unsigned r=0;r<16;++r) {
            uint64_t result=actual[r];if(variant->call && r==8)result-=initial_rsp;
            uint64_t expected=r==0?test->rax:r==2?test->rdx:r==8?test->r8:r==4?initial_rsp+8+test->rsp_delta:gpr[r];
            if(result!=expected){fprintf(stderr,"FAIL BND case%u form%u reg%u got=%016" PRIx64 " expected=%016" PRIx64 "\n",i,active,r,result,expected);return 1;}
        }
        for(unsigned k=0;k<64;++k) {
            uint64_t expected=variant->call && k==15?variant->return_va:UINT64_C(0x5566778811223344)^k;
            if(stack[k]!=expected){fprintf(stderr,"FAIL BND stack slot%u case%u\n",k,i);return 2;}
        }
        if((eflags_pack(&c)&0x8d5)!=test->flags || memcmp(c.xmm,xmm,sizeof(xmm)) || dispatches!=variant->dispatches)return 3;
        checksum=(checksum^test->rax^test->rdx^test->r8^test->flags^(uint64_t)test->rsp_delta)*UINT64_C(1099511628211);
    }
    printf("PASS BND native oracle: cases=%zu encodings=%zu flags=64 checksum=%016" PRIx64 "\n",sizeof(bnd_cases)/sizeof(bnd_cases[0]),sizeof(bnd_variants)/sizeof(bnd_variants[0]),checksum);
    return 0;
}
