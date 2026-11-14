/* Win64 VCRUNTIME initial setjmp capture. Guest registers remain guest data.
 * shortcut: longjmp restoration/unwind is unsupported; host longjmp into a
 * returned HLE frame would be invalid and is never substituted here. */
#include "win64_jump.h"
#include "win64.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t frame,rbx,rsp,rbp,rsi,rdi,r12,r13,r14,r15,rip;
    uint32_t mxcsr;
    uint16_t fpcsr,spare;
    XMM xmm[10];
} windows_jump_buffer;
_Static_assert(sizeof(windows_jump_buffer)==256,"Win64 JUMP_BUFFER size");
_Static_assert(offsetof(windows_jump_buffer,mxcsr)==88,"Win64 MXCSR offset");
_Static_assert(offsetof(windows_jump_buffer,fpcsr)==92,"Win64 x87 control offset");
_Static_assert(offsetof(windows_jump_buffer,spare)==94,"Win64 spare offset");
_Static_assert(offsetof(windows_jump_buffer,xmm)==96,"Win64 XMM6 offset");

/* No guest x87 execution is implemented. The existing fesetround provider is
 * the only supported writer and updates these RC bits on the calling thread.
 * The remaining bits follow the Win64 CRT default, never the ARM FPCR. */
static _Thread_local uint16_t guest_x87_control=UINT16_C(0x027f);
static _Noreturn void jump_stop(const char *why){fprintf(stderr,"STOP Win64 jump %s\n",why);fflush(stderr);_Exit(73);}
void win64_jump_set_round(unsigned rounding){
    if(rounding>3)jump_stop("invalid rounding control");
    guest_x87_control=(uint16_t)((guest_x87_control&~UINT16_C(0x0c00))|(rounding<<10));
}
int win64_jump_call(CPU *c,const char *name){
    if(strcmp(name,"__intrinsic_setjmp"))return 0;
    if(c->rcx&15u)jump_stop("JUMP_BUFFER must be 16-byte aligned");
    if(c->rsp>UINT64_MAX-8)jump_stop("return stack address overflow");
    uint64_t destination=c->rcx,post_rsp=c->rsp+8,return_address;
    unsigned char *out=win64_pointer(destination,sizeof(windows_jump_buffer),1);
    if(destination<post_rsp&&c->rsp<destination+sizeof(windows_jump_buffer))jump_stop("JUMP_BUFFER overlaps return slot");
    memcpy(&return_address,win64_pointer(c->rsp,8,0),8);
    windows_jump_buffer saved={0};
    saved.frame=c->rdx;saved.rbx=c->rbx;saved.rsp=post_rsp;saved.rbp=c->rbp;
    saved.rsi=c->rsi;saved.rdi=c->rdi;saved.r12=c->r12;saved.r13=c->r13;saved.r14=c->r14;saved.r15=c->r15;
    saved.rip=return_address;saved.mxcsr=c->mxcsr;saved.fpcsr=guest_x87_control;
    memcpy(saved.xmm,c->xmm+6,sizeof(saved.xmm));
    /* Native __intrinsic_setjmp leaves Spare untouched, including its canary. */
    memcpy(out,&saved,offsetof(windows_jump_buffer,spare));
    memcpy(out+offsetof(windows_jump_buffer,xmm),saved.xmm,sizeof(saved.xmm));
    c->rax=0;return 1;
}
void win64_jump_self_test(void){
    _Alignas(16) unsigned char buffer[288];uint64_t stack[4]={UINT64_C(0x141c0e3ba),0,0,0};
    CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=(uintptr_t)(buffer+16);c.rdx=UINT64_C(0x123456789abcdef0);c.mxcsr=0x5fa5;
    CPU before=c;uint16_t saved_control=guest_x87_control;
    win64_region_add((uintptr_t)buffer,sizeof(buffer),1);win64_region_add((uintptr_t)stack,sizeof(stack),0);
    for(unsigned r=0;r<4;++r){
        memset(buffer,0x5a,sizeof(buffer));win64_jump_set_round(r);c=before;
        if(!win64_jump_call(&c,"__intrinsic_setjmp"))jump_stop("capture dispatch self-test");
        windows_jump_buffer copy;memcpy(&copy,buffer+16,sizeof(copy));
        if(copy.frame!=before.rdx||copy.rbx!=before.rbx||copy.rsp!=before.rsp+8||copy.rip!=stack[0]||copy.mxcsr!=before.mxcsr||copy.fpcsr!=(0x027fu|(r<<10))||copy.spare!=0x5a5a||memcmp(copy.xmm,before.xmm+6,sizeof(copy.xmm)))jump_stop("context capture self-test");
        for(unsigned i=0;i<16;++i)if(buffer[i]!=0x5a||buffer[272+i]!=0x5a)jump_stop("capture canary self-test");
        c.rax=before.rax;if(memcmp(&c,&before,sizeof(c)))jump_stop("capture CPU preservation self-test");
    }
    guest_x87_control=saved_control;
    c=before;if(win64_jump_call(&c,"longjmp")||memcmp(&c,&before,sizeof(c)))jump_stop("unsupported restore self-test");
    win64_region_remove((uintptr_t)stack);win64_region_remove((uintptr_t)buffer);
    fputs("WIN64_JUMP_SELF_TEST initial_capture=1 Win64_buffer256=1 Spare_preserved=1 NV_GPR_XMM=1 TLS_x87_control=1 no_rsp_pop=1 longjmp_unsupported=1 passed\n",stderr);
}
