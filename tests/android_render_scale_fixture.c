/* Original lifted Combo arrows, delegate/Apply and float property I/O.
 * The engine singleton is a named fixture double. No original bytes execute. */
#include "cpu64.h"
#include <stdio.h>
#include <errno.h>
#include <fenv.h>
#include <inttypes.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

int64_t g_image_delta;
static unsigned char *image;
static unsigned char engine[0x1000],model[0x100],option_data[6*16];
static unsigned char definition[128],helper[128],owner[32],payload[32],profile[4096];
static unsigned char stack_data[0x10000],flags_data[8],property[64],binary[16];
static unsigned char resolutions[5*12];
static unsigned long checks,calls,setter_calls,select_calls;
static int candidate_mode=1;

static void require(int ok,const char *what){++checks;if(!ok){fprintf(stderr,"RENDER_SCALE_FAIL %s check=%lu\n",what,checks);exit(1);}}
static uint64_t ptr(const void*p){return (uint64_t)(uintptr_t)p;}
void frostpunk_unsupported(uint64_t va,const char*t){fprintf(stderr,"UNSUPPORTED %" PRIx64 " %s\n",va,t);exit(73);}
uint64_t frostpunk_teb_read(uint64_t off,unsigned n){(void)off;(void)n;return 0;}
void frostpunk_teb_write(uint64_t off,uint64_t v,unsigned n){(void)off;(void)v;(void)n;abort();}
void frostpunk_cpuid(CPU*c){(void)c;abort();}
void es3_eh_push(es3_ehframe_t*f,uint64_t va,jmp_buf*b){(void)f;(void)va;(void)b;}
void es3_eh_pop(es3_ehframe_t*f){(void)f;}

#include "render_scale_candidate.h"
#include "render_scale_functions.inc"

void dispatch(CPU*c,uint64_t target){
    if(target>=(uint64_t)g_image_delta)target-=(uint64_t)g_image_delta;
    ++calls;
    switch(target){
#include "render_scale_dispatch.inc"
    case UINT64_C(0x141a17690):++setter_calls;if(candidate_mode)candidate_set(c);else baseline_set(c);return;
    case UINT64_C(0x141a17f50):++select_calls;native_select(c);return;
    case UINT64_C(0x141a14ce0):native_delegate(c);return;
    case UINT64_C(0x141a146b0):native_wrapper(c);return;
    case UINT64_C(0x140d87190):native_field_address(c);return;
    case UINT64_C(0x140d871a0):native_field_set(c);return;
    case UINT64_C(0x140e4c470):native_field_set(c);return;
    case UINT64_C(0x140e66520):native_quality(c);return;
    case UINT64_C(0x140e878f0):native_fullscreen(c);return;
    case UINT64_C(0x140e86a10):native_get_fullscreen(c);return;
    case UINT64_C(0x141ae0960):c->rsp+=8;return; /* Security-cookie comparison fixture. */
    case UINT64_C(0x1410f41b0):SET64(c->rax,ptr(profile));c->rsp+=8;return; /* Game singleton; no setting side effects. */
    default:fprintf(stderr,"UNEXPECTED_RENDER_SCALE_CALL %" PRIx64 " rip=%" PRIx64 "\n",target,c->rip);exit(73);
    }
}
void dispatch_jmp(CPU*c,uint64_t t){dispatch(c,t);}

static uint64_t fp_control(void){
#if defined(__aarch64__)
    uint64_t v;__asm__ volatile("mrs %0, fpcr":"=r"(v));return v;
#else
    return _mm_getcsr()&~63u;
#endif
}
static uint64_t fp_status(void){
#if defined(__aarch64__)
    uint64_t v;__asm__ volatile("mrs %0, fpsr":"=r"(v));return v;
#else
    return _mm_getcsr()&63u;
#endif
}
static CPU initial(void){
    CPU c;memset(&c,0xa5,sizeof(c));
    c.rsp=ptr(stack_data+sizeof(stack_data)-0x108);c.mxcsr=0x9fc0;
    c.cf=1;c.pf=0;c.af=1;c.zf=0;c.sf=1;c.of=0;
    memset(stack_data,0x3d,sizeof(stack_data));wr64(c.rsp,UINT64_C(0x140123450));
    return c;
}
static void nonvolatile(const CPU*a,const CPU*b){
    require(a->rbx==b->rbx&&a->rbp==b->rbp&&a->rdi==b->rdi&&a->rsi==b->rsi&&
        a->r12==b->r12&&a->r13==b->r13&&a->r14==b->r14&&a->r15==b->r15,"native Win64 nonvolatile GPR");
    require(!memcmp(a->xmm+6,b->xmm+6,10*sizeof(XMM)),"native Win64 nonvolatile XMM");
    require(a->mxcsr==b->mxcsr,"guest MXCSR");
    require(a->rsp+8==b->rsp,"one outer pop");
}
static void setup(unsigned index){
    memset(engine,0,sizeof(engine));memset(model,0,sizeof(model));memset(flags_data,0,sizeof(flags_data));
    memset(profile,0,sizeof(profile));memset(helper,0,sizeof(helper));
    wr64(GVA(0x142985ab0),ptr(engine));wr32(GVA(0x14296fae0),0);wr64(GVA(0x14289c978),0xabc123);
    wr32(ptr(engine+0x1a4),rs_factors[index]);wr32(ptr(engine+0xcc),0);
    wr64(ptr(engine+0xf8),ptr(resolutions));
    for(unsigned i=0;i<5;++i){wr32(ptr(resolutions+12*i),640+i*16);wr32(ptr(resolutions+12*i+4),360+i*9);wr32(ptr(resolutions+12*i+8),60);}
    wr32(ptr(model+0x20),20);wr64(ptr(model+0x28),ptr(option_data));wr32(ptr(model+0x30),6);
    model[0x49]=1;wr32(ptr(model+0x34),6);wr32(ptr(model+0x40),index);wr32(ptr(model+0x44),index);
    wr64(ptr(model+0x50),GVA(0x141a146b0));wr64(ptr(model+0x58),GVA(0x14289c0e8));
    wr64(ptr(model+0x60),ptr(payload));wr64(ptr(payload),ptr(helper));wr64(ptr(payload+8),ptr(owner));
    wr64(ptr(owner),ptr(definition));
    for(unsigned i=0;i<6;++i){wr32(ptr(option_data+i*16),50+10*i);wr64(ptr(option_data+i*16+8),0);}
}
static void getter(unsigned index){
    setup(index);CPU c=initial(),before=c;c.rcx=ptr(helper);c.rdx=ptr(model);
    wr32(ptr(model+0x40),99);wr32(ptr(model+0x44),98);
    errno=2345;uint64_t control=fp_control(),status=fp_status();candidate_get(&c);
    nonvolatile(&before,&c);
    require(rd32(ptr(model+0x40))==index&&rd32(ptr(model+0x44))==index,"getter displays persisted effective scale");
    require(errno==2345&&control==fp_control()&&status==fp_status(),"getter native FP/errno");
}
static void apply(unsigned old,unsigned next){
    setup(old);wr32(ptr(model+0x40),next);
    CPU c=initial(),before=c;c.rcx=ptr(model);c.rdx=ptr(flags_data);c.r8=ptr(flags_data+1);c.r9=ptr(flags_data+2);
    unsigned long calls_before=setter_calls;errno=2345;uint64_t control=fp_control(),status=fp_status();native_apply(&c);
    nonvolatile(&before,&c);
    require(rd32(ptr(engine+0x1a4))==rs_factors[next],"Apply actual RendererConfig F4");
    require(rd32(ptr(model+0x44))==next,"Apply commits original applied index");
    require(flags_data[0]==(old!=next)&&!flags_data[1]&&!flags_data[2],"screen dirty only, no profile/restart flags");
    require(setter_calls-calls_before==(old!=next),"delegate exactly once only for changed value");
    require(errno==2345&&control==fp_control()&&status==fp_status(),"Apply native FP/errno");
}
static void arrows(unsigned old,int direction,unsigned steps){
    setup(old);unsigned long previous=setter_calls;
    for(unsigned i=0;i<steps;++i){CPU c=initial(),before=c;c.rcx=ptr(model);
        if(direction>0)native_right(&c);else native_left(&c);nonvolatile(&before,&c);}
    unsigned next=(old+6+(direction>0?1:-1)*(int)(steps%6))%6;
    require(rd32(ptr(model+0x40))==next,"original arrow changes pending index/wrap");
    require(rd32(ptr(model+0x44))==old&&rd32(ptr(engine+0x1a4))==rs_factors[old],"pending arrow leaves applied scale untouched");
    require(previous==setter_calls,"arrow does not call Apply delegate");
    CPU c=initial();c.rcx=ptr(helper);c.rdx=ptr(model);candidate_get(&c);
    require(rd32(ptr(model+0x40))==old&&rd32(ptr(model+0x44))==old,"rebuild after Cancel restores effective scale");
}
static void serialization(unsigned index){
    setup(index);memset(property,0,sizeof(property));memset(binary,0xcc,sizeof(binary));
    wr64(ptr(property),GVA(0x141d4e880));wr32(ptr(property+0x28),0xf4);
    CPU c=initial();c.rcx=ptr(property);c.rdx=ptr(binary+4);c.r8=ptr(engine+0xb0);wr32(c.rsp+40,0);
    native_write_float(&c);require(c.rax==4,"original float binary writer length");
    require(rd32(ptr(binary+4))==rs_factors[index]&&rd32(ptr(binary))==0xcccccccc&&rd32(ptr(binary+8))==0xcccccccc,"binary writer exact four bytes/canaries");
    wr32(ptr(engine+0x1a4),0xdeadbeef);c=initial();c.rcx=ptr(property);c.rdx=ptr(binary+4);c.r8=ptr(engine+0xb0);
    native_read_float(&c);require(c.rax==4&&rd32(ptr(engine+0x1a4))==rs_factors[index],"original float binary reader restores F4");
    c=initial();c.rcx=ptr(helper);c.rdx=ptr(model);candidate_get(&c);
    require(rd32(ptr(model+0x40))==index,"native decoded value feeds UI getter");
}
static void packets(unsigned quality,unsigned index){
    setup(index);wr32(ptr(engine+0xcc),quality);unsigned char packet[0xc00];memset(packet,0x8f,sizeof(packet));
    CPU a=initial(),b=a;a.rbx=ptr(packet);a.rdi=ptr(engine);a.rax=0x58;wr32(ptr(packet+0x38),quality);
    uint64_t control=fp_control(),status=fp_status();candidate_initial_packet(&a);
    require(rd32(ptr(packet+0x40))==rs_factors[index],"initial packet actual factor independent of quality");
    require(fp_control()==control&&fp_status()==status,"initial packet native FP");
    a=initial();a.rcx=ptr(packet);a.r8=ptr(engine);a.rdx=quality;candidate_refresh_packet(&a);
    require(rd32(ptr(packet+0x9f8))==rs_factors[index],"refresh packet actual factor independent of quality");
    if(!quality||index==5){
        unsigned char copy[0xc00];memset(copy,0x8f,sizeof(copy));wr32(ptr(copy+0x38),quality);
        a=initial();a.rbx=ptr(packet);a.rdi=ptr(engine);a.rax=0x58;b=a;b.rbx=ptr(copy);
        baseline_initial_packet(&a);candidate_initial_packet(&b);b.rbx=a.rbx;
        require(!memcmp(&a,&b,sizeof(a))&&!memcmp(packet,copy,0xac),"unchanged initial packet full CPU/bytes");
    }
}
static void old_renderer(unsigned id,unsigned value){
    setup(3);wr32(ptr(model+0x20),id);
    for(unsigned i=0;i<5;++i)wr32(ptr(option_data+i*16),i);wr32(ptr(model+0x30),5);
    wr32(ptr(engine+0xcc),value);wr32(ptr(engine+0x134),value);wr32(ptr(engine+0x128),value);
    wr32(ptr(engine+0x12c),value);wr32(ptr(engine+0x138),value);
    CPU a=initial();a.rcx=ptr(helper);a.rdx=ptr(model);CPU b=a;
    unsigned char before_stack[sizeof(stack_data)],after_stack[sizeof(stack_data)],after_model[sizeof(model)];
    memcpy(before_stack,stack_data,sizeof(stack_data));baseline_get(&a);
    memcpy(after_stack,stack_data,sizeof(stack_data));memcpy(after_model,model,sizeof(model));
    memcpy(stack_data,before_stack,sizeof(stack_data));wr32(ptr(model+0x40),3);wr32(ptr(model+0x44),3);
    candidate_get(&b);require(!memcmp(&a,&b,sizeof(a))&&!memcmp(after_stack,stack_data,sizeof(stack_data))&&!memcmp(after_model,model,sizeof(model)),"old renderer getter exact CPU/memory");
    setup(3);a=initial();a.rcx=ptr(helper);a.rdx=ptr(definition);a.r8=id;a.r9=1;
    wr32(a.rsp+40,value);wr64(a.rsp+48,ptr(flags_data));wr64(a.rsp+56,ptr(flags_data+1));wr64(a.rsp+64,ptr(flags_data+2));b=a;
    memcpy(before_stack,stack_data,sizeof(stack_data));unsigned char initial_engine[sizeof(engine)],after_engine[sizeof(engine)],after_flags[8];
    memcpy(initial_engine,engine,sizeof(engine));baseline_set(&a);memcpy(after_stack,stack_data,sizeof(stack_data));memcpy(after_engine,engine,sizeof(engine));memcpy(after_flags,flags_data,8);
    memcpy(stack_data,before_stack,sizeof(stack_data));memcpy(engine,initial_engine,sizeof(engine));memset(flags_data,0,8);candidate_set(&b);
    require(!memcmp(&a,&b,sizeof(a))&&!memcmp(after_stack,stack_data,sizeof(stack_data))&&!memcmp(after_engine,engine,sizeof(engine))&&!memcmp(after_flags,flags_data,8),"old renderer setter exact CPU/memory");
}
int main(int argc,char**argv){
#if defined(_WIN32)
    image=VirtualAlloc((void*)(uintptr_t)UINT64_C(0x140000000),0x4540000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#else
    image=mmap((void*)(uintptr_t)UINT64_C(0x140000000),0x4540000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    require(ptr(image)==UINT64_C(0x140000000),"fixed NX image allocation");g_image_delta=0;
    if(argc<2){fprintf(stderr,"usage: fixture image-blocks.bin [bad-factor|bad-percent|bad-gate INDEX]\n");return 2;}
    FILE*f=fopen(argv[1],"rb");require(f!=NULL,"read fixture image blocks");uint32_t count;
    require(fread(&count,4,1,f)==1&&count<100,"image block count");
    for(unsigned i=0;i<count;++i){uint32_t rva,n;require(fread(&rva,4,1,f)==1&&fread(&n,4,1,f)==1&&rva<=0x4540000&&n<=0x4540000-rva,"image block bounds");require(fread(image+rva,1,n,f)==n,"image block bytes");}
    require(fgetc(f)==EOF&&!ferror(f)&&fclose(f)==0,"image block exact tail");
    if(argc>2){
        if(!strcmp(argv[2],"bad-factor")){setup(3);wr32(ptr(engine+0x1a4),0x7fc00000);CPU c=initial();c.rdx=ptr(model);candidate_get(&c);return 3;}
        if(!strcmp(argv[2],"bad-percent")){setup(3);CPU c=initial();c.r8=20;wr32(c.rsp+40,atoi(argv[3]));wr64(c.rsp+48,ptr(flags_data));candidate_set(&c);return 3;}
        if(!strcmp(argv[2],"bad-gate")){unsigned i=(unsigned)atoi(argv[3]);if(i>=8)return 2;image[rs_gate_rvas[i]]^=1;rs_verify();return 3;}
        return 2;
    }
    rs_verify();
    for(unsigned i=0;i<6;++i){getter(i);serialization(i);for(unsigned j=0;j<6;++j)apply(i,j);
        for(int direction=-1;direction<=1;direction+=2)for(unsigned steps=1;steps<=13;++steps)arrows(i,direction,steps);
        for(unsigned quality=0;quality<5;++quality)packets(quality,i);}
    for(unsigned id=6;id<18;++id)for(unsigned value=0;value<5;++value)old_renderer(id,value);
    printf("{\"success\":true,\"checks\":%lu,\"calls\":%lu,\"setter_calls\":%lu,\"select_calls\":%lu,\"original_combo_arrows\":true,\"original_apply_delegate\":true,\"original_float_binary_roundtrip\":true,\"device_actions\":false}\n",checks,calls,setter_calls,select_calls);
#if defined(_WIN32)
    VirtualFree(image,0,MEM_RELEASE);
#else
    munmap(image,0x4540000);
#endif
    return 0;
}
