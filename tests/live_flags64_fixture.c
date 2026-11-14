#include "live_flags64.h"
#include <stdio.h>
static uint64_t seed=UINT64_C(0x6c697665666c6167);
static uint64_t random64(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static uint64_t flag(const CPU*c,unsigned i){switch(i){case 0:return c->cf;case 1:return c->zf;case 2:return c->sf;case 3:return c->of;case 4:return c->pf;default:return c->af;}}
unsigned test_generated_blocks(void);
int main(void){
    unsigned cases=0;
    const uint64_t edge[]={0,1,UINT64_MAX,UINT64_C(0x8000000000000000),UINT64_C(0x7fffffffffffffff),0xff,0x7f,0x80,0xffff,0x8000,0xffffffff,0x80000000};
    for(unsigned op=0;op<3;++op)for(unsigned mask=0;mask<64;++mask)for(int sz=1;sz<=8;sz*=2)for(unsigned n=0;n<96;++n){
        CPU initial;for(unsigned i=0;i<sizeof(initial);++i)((unsigned char*)&initial)[i]=(unsigned char)random64();
        initial.cf=random64()&1;initial.zf=random64()&1;initial.sf=random64()&1;initial.of=random64()&1;initial.pf=random64()&1;initial.af=random64()&1;
        CPU a=initial,b=initial;uint64_t x=n<12?edge[n]:random64(),y=n<12?edge[11-n]:random64(),old,new;
        if(op==0){old=flags_add(&a,x,y,sz);new=fp_flags_add(&b,x,y,sz,mask);}
        else if(op==1){old=flags_sub(&a,x,y,sz);new=fp_flags_sub(&b,x,y,sz,mask);}
        else{old=flags_logicz(&a,x,sz);new=fp_flags_logicz(&b,x,sz,mask);}
        if(old!=new)return 1;
        for(unsigned bit=0;bit<6;++bit)if(flag(&b,bit)!=((mask&(1u<<bit))?flag(&a,bit):flag(&initial,bit)))return 2;
        CPU restored=b;restored.cf=initial.cf;restored.zf=initial.zf;restored.sf=initial.sf;restored.of=initial.of;restored.pf=initial.pf;restored.af=initial.af;
        if(memcmp(&restored,&initial,sizeof(initial)))return 3;
        ++cases;
    }
    unsigned blocks=test_generated_blocks();
    printf("{\"success\":true,\"masked_helper_cases\":%u,\"full_CPU_block_cases\":%u}\n",cases,blocks);
}
