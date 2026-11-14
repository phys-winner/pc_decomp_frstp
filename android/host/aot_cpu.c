#include "aot_cpu.h"
#include <string.h>

/* Portable core promoted from the frozen native-oracle candidate. Per-lane NaN exception priority was measured against the
   statically compiled native oracle: a NaN suppresses DE in its own lane only.
   All guest exception masks must have been checked by the caller. */
static uint32_t compare32(uint32_t a,uint32_t b,uint32_t daz,unsigned operation,uint32_t *flags) {
    uint32_t aa=a&UINT32_C(0x7fffffff),bb=b&UINT32_C(0x7fffffff);
    if(aa>UINT32_C(0x7f800000)||bb>UINT32_C(0x7f800000)) {
        if(operation || (aa>UINT32_C(0x7f800000)&&!(aa&UINT32_C(0x400000))) ||
                        (bb>UINT32_C(0x7f800000)&&!(bb&UINT32_C(0x400000))))*flags|=1;
        return 0;
    }
    uint32_t dena=(aa-1)<UINT32_C(0x007fffff),denb=(bb-1)<UINT32_C(0x007fffff);
    if(daz){if(dena){a&=UINT32_C(0x80000000);aa=0;}if(denb){b&=UINT32_C(0x80000000);bb=0;}}
    else *flags|=(dena|denb)<<1;
    if(!operation)return (a==b||!(aa|bb))?UINT32_MAX:0;
    uint32_t ka=a^((0-(a>>31))|UINT32_C(0x80000000)),kb=b^((0-(b>>31))|UINT32_C(0x80000000));
    return (ka<kb&&(aa|bb))?UINT32_MAX:0;
}
static uint64_t compare64(uint64_t a,uint64_t b,uint32_t daz,unsigned operation,uint32_t *flags) {
    uint64_t aa=a&UINT64_C(0x7fffffffffffffff),bb=b&UINT64_C(0x7fffffffffffffff);
    if(aa>UINT64_C(0x7ff0000000000000)||bb>UINT64_C(0x7ff0000000000000)) {
        if(operation || (aa>UINT64_C(0x7ff0000000000000)&&!(aa&UINT64_C(0x8000000000000))) ||
                        (bb>UINT64_C(0x7ff0000000000000)&&!(bb&UINT64_C(0x8000000000000))))*flags|=1;
        return operation==2?UINT64_MAX:0;
    }
    uint32_t dena=(aa-1)<UINT64_C(0x000fffffffffffff),denb=(bb-1)<UINT64_C(0x000fffffffffffff);
    if(daz){if(dena){a&=UINT64_C(0x8000000000000000);aa=0;}if(denb){b&=UINT64_C(0x8000000000000000);bb=0;}}
    else *flags|=(dena|denb)<<1;
    if(!operation)return (a==b||!(aa|bb))?UINT64_MAX:0;
    uint64_t ka=a^((0-(a>>63))|UINT64_C(0x8000000000000000)),kb=b^((0-(b>>63))|UINT64_C(0x8000000000000000));
    if(operation==2)return (ka>kb&&(aa|bb))?UINT64_MAX:0;
    return (ka<kb&&(aa|bb))?UINT64_MAX:0;
}
static uint32_t fp_packed_integer(XMM *out,const XMM *destination,const XMM *source,uint32_t mxcsr,unsigned operation) {
    XMM a=*destination,b=*source,r;uint32_t flags=0,daz=mxcsr&UINT32_C(0x40);
    if(operation<2)for(unsigned lane=0;lane<4;++lane)r.u32[lane]=compare32(a.u32[lane],b.u32[lane],daz,operation,&flags);
    else for(unsigned lane=0;lane<2;++lane)r.u64[lane]=compare64(a.u64[lane],b.u64[lane],daz,operation-2,&flags);
    *out=r;return mxcsr|flags;
}

typedef struct {uint64_t va;const char *text,*bytes;unsigned operation,destination,source,lane;} fp_aot_site;
typedef struct {uint64_t va;unsigned size;const char *bytes;} fp_aot_gate;
#include "aot_cpu_sites.inc"
static int verified;

static uint16_t load16(const unsigned char *p){uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t load32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t load64(const unsigned char *p){uint64_t v;memcpy(&v,p,8);return v;}
static int span(size_t at,size_t bytes,size_t size){return at<=size && bytes<=size-at;}
int fp_aot_cpu_init(const void *mapped_pe,size_t size) {
    verified=0;
    if(!mapped_pe || size<64)return 0;
    const unsigned char *image=mapped_pe;
    if(load16(image)!=0x5a4d)return 0;
    size_t nt=load32(image+0x3c);
    if(!span(nt,24,size)||load32(image+nt)!=0x4550)return 0;
    unsigned count=load16(image+nt+6),optional=load16(image+nt+20);
    if(!count||count>96||optional<64||!span(nt+24,optional,size)||
       load16(image+nt+24)!=0x20b||load64(image+nt+48)!=UINT64_C(0x140000000))return 0;
    size_t table=nt+24+optional;
    if(!span(table,(size_t)count*40,size))return 0;
    uint64_t low=0,high=0;
    for(unsigned i=0;i<count;++i) {
        const unsigned char *s=image+table+(size_t)i*40;
        if(!memcmp(s,".text\0\0\0",8)) {
            if(low || !(load32(s+36)&0x20))return 0;
            low=load32(s+12);uint64_t bytes=load32(s+8);
            if(!low||!bytes||low>size||bytes>size-low)return 0;
            high=low+bytes;
        }
    }
    if(!low)return 0;
    for(size_t i=0;i<sizeof(fp_aot_gates)/sizeof(*fp_aot_gates);++i) {
        const fp_aot_gate *g=&fp_aot_gates[i];
        if(g->va<UINT64_C(0x140000000))return 0;
        uint64_t offset=g->va-UINT64_C(0x140000000);
        if(offset<low||offset>=high||g->size>high-offset||
           memcmp(image+(size_t)offset,g->bytes,g->size))return 0;
    }
    verified=1;return 1;
}
static int masked_mxcsr(uint32_t mode) {
    /* Translated Intel/SSE2 contract: DAZ/FTZ supported in software; AMD MM is not.
       shortcut: reject unmasked exceptions until a guest SSE exception bridge exists. */
    return !(mode&~UINT32_C(0xffff)) && (mode&UINT32_C(0x1f80))==UINT32_C(0x1f80);
}
int fp_aot_float_operation(CPU *c,unsigned operation,unsigned destination,unsigned source);
int fp_aot_packed_compare(CPU *c,unsigned destination,const XMM *source,unsigned operation) {
    if(!verified||!c||!source||destination>=16||operation>FP_AOT_CMPPD_NLE||!masked_mxcsr(c->mxcsr))return 0;
    XMM d=c->xmm[destination],s=*source,result;
    uint32_t mode=fp_packed_integer(&result,&d,&s,c->mxcsr,operation);
    c->xmm[destination]=result;c->mxcsr=mode;return 1;
}
int fp_aot_packed_compare_memory(CPU *c,unsigned destination,uint64_t address,unsigned operation) {
    if(!verified||!c||destination>=16||operation>FP_AOT_CMPPD_NLE||!masked_mxcsr(c->mxcsr)||
       (address&15)||address>UINT64_MAX-15)return 0;
    XMM source;rd128(address,&source);
    return fp_aot_packed_compare(c,destination,&source,operation);
}
int fp_aot_cpu_instruction(CPU *c,uint64_t va,const char *instruction) {
    if(!verified||!c||!instruction)return 0;
    size_t lo=0,hi=sizeof(fp_aot_sites)/sizeof(*fp_aot_sites);
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(fp_aot_sites[mid].va<va)lo=mid+1;else hi=mid;}
    if(lo==sizeof(fp_aot_sites)/sizeof(*fp_aot_sites)||fp_aot_sites[lo].va!=va)return 0;
    const fp_aot_site *s=&fp_aot_sites[lo];
    if(strcmp(instruction,s->text))return 0;
    if(s->operation>=6)return fp_aot_float_operation(c,s->operation,s->destination,s->source);
    XMM result;
    if(s->operation==0) {
        XMM a=c->xmm[s->destination],b=c->xmm[s->source];
        for(unsigned i=0;i<4;++i)result.u32[i]=a.u32[i]*b.u32[i];
    } else if(s->operation==1) {
        uint8_t input[4];memcpy(input,&c->xmm[s->source],4);
        for(unsigned i=0;i<4;++i)result.u32[i]=input[i];
    } else if(s->operation==2) {
        XMM input=c->xmm[s->source];
        for(unsigned i=0;i<4;++i){uint32_t v=input.u32[i];result.u32[i]=(v&UINT32_C(0x80000000))?0u-v:v;}
    } else if(s->operation==3) {
        XMM a=c->xmm[s->destination],b=c->xmm[s->source];result.u64[0]=result.u64[1]=0;
        for(unsigned i=0;i<16;++i){int difference=(int)a.u8[i]-(int)b.u8[i];result.u64[i/8]+=(unsigned)(difference<0?-difference:difference);}
    } else if(s->operation==4) {
        int32_t input[2];memcpy(input,&c->xmm[s->source],8);
        result.i64[0]=(int64_t)input[0];result.i64[1]=(int64_t)input[1];
    } else {
        uint64_t value=c->xmm[s->source].u32[s->lane];
        memcpy((unsigned char *)c+s->destination*8,&value,8);return 1;
    }
    c->xmm[s->destination]=result;return 1;
}

