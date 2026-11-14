#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "bridge.h"

_Static_assert(offsetof(CPU,rax)==0 && offsetof(CPU,rcx)==8 &&
    offsetof(CPU,rdx)==16 && offsetof(CPU,rsp)==32 && offsetof(CPU,r8)==64 &&
    offsetof(CPU,r9)==72 && offsetof(CPU,rip)==128 && offsetof(CPU,xmm)==184,
    "bridge.asm CPU layout");
_Static_assert(FP_NATIVE_MAX_STACK_BYTES==256,"bridge.asm stack limit");
extern void fp_test_return_registers(void);
extern void fp_test_stack_words(void);
extern int fp_test_nonvolatile(CPU *,uint64_t,unsigned);
static const uint64_t return_pc=UINT64_C(0x141234567);
static unsigned calls;
static void aligned(void) {
    /* A separate C function can have its own prologue, so inspect the actual
     * native target's return-address location in each target below. */
    ++calls;
}
#define ENTRY() do { assert(((uintptr_t)_AddressOfReturnAddress()&15)==8); aligned(); } while(0)
static __declspec(noinline) uint64_t zero(void) {ENTRY();return UINT64_C(0xfeca12345678);}
static __declspec(noinline) uint64_t four(uint64_t a,uint64_t b,uint64_t c,uint64_t d) {ENTRY();return a+3*b+5*c+7*d;}
static __declspec(noinline) uint64_t sixteen(uint64_t a,uint64_t b,uint64_t c,uint64_t d,
    uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j,uint64_t k,uint64_t l,
    uint64_t m,uint64_t n,uint64_t o,uint64_t p) {
    ENTRY();return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j+11*k+12*l+13*m+14*n+15*o+16*p;
}
static __declspec(noinline) double mixed(uint64_t a,double b,uint64_t c,float d,
    double e,uint64_t f,double g,uint64_t h) {ENTRY();return a+b*2+c*3+d*4+e*5+f*6+g*7+h*8;}
static __declspec(noinline) uint64_t thirtysix(uint64_t a,uint64_t b,uint64_t c,uint64_t d,
    uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t j,uint64_t k,uint64_t l,
    uint64_t m,uint64_t n,uint64_t o,uint64_t p,uint64_t q,uint64_t r,uint64_t s,uint64_t t,
    uint64_t u,uint64_t v,uint64_t w,uint64_t x,uint64_t y,uint64_t z,uint64_t aa,uint64_t ab,
    uint64_t ac,uint64_t ad,uint64_t ae,uint64_t af,uint64_t ag,uint64_t ah,uint64_t ai,uint64_t aj) {
    ENTRY();return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q+r+s+t+u+v+w+x+y+z+aa+ab+ac+ad+ae+af+ag+ah+ai+aj;
}
static CPU frame(uint64_t *stack) {
    CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;stack[0]=return_pc;
    c.rcx=1;c.rdx=2;c.r8=3;c.r9=4;
    for(unsigned i=0;i<32;++i)stack[5+i]=5+i;
    return c;
}
static void unchanged(CPU before,CPU after) {
    assert(after.rsp==before.rsp+8 && after.rip==return_pc);
    before.rax=after.rax;before.rdx=after.rdx;before.rsp=after.rsp;before.rip=after.rip;
    before.xmm[0]=after.xmm[0];before.xmm[1]=after.xmm[1];
    assert(!memcmp(&before,&after,sizeof(before)));
}
static __declspec(noinline) uint64_t guarded_words(uint64_t count) {
    assert(((uintptr_t)_AddressOfReturnAddress()&15)==8);
    assert(GetLastError()==0xe5ef1234);
    const volatile uint64_t *arguments=(const volatile uint64_t *)((char *)_AddressOfReturnAddress()+40);
    uint64_t sum=0;for(uint64_t i=0;i<count;++i)sum+=arguments[i]*(i+1);
    SetLastError(0xe5ef7654);return sum;
}
static void guard_page_cases(void) {
    unsigned char *memory=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(memory);
    DWORD previous;assert(VirtualProtect(memory+4096,4096,PAGE_NOACCESS,&previous));
    unsigned saved_mxcsr=_mm_getcsr();
    const unsigned controls[]={0,0x1f80,0x3fa1,0x5fc2,0x9fff};
    for(unsigned bytes=0;bytes<=256;bytes+=8)for(unsigned state=0;state<5;++state) {
        uint64_t *stack=(uint64_t *)(memory+4096-40-bytes);memset(stack,0x6b,40+bytes);stack[0]=return_pc;
        uint64_t expected=0;
        for(unsigned i=0;i<bytes/8;++i){stack[5+i]=5+i;expected+=(5+i)*(i+1);}
        unsigned char snapshot[296];memcpy(snapshot,stack,40+bytes);
        CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=bytes/8;CPU before=c;
        _mm_setcsr(controls[state]);SetLastError(0xe5ef1234);
        fp_native_call(&c,(uintptr_t)guarded_words,bytes);
        assert(c.rax==expected && _mm_getcsr()==controls[state] && GetLastError()==0xe5ef7654);
        assert(!memcmp(snapshot,stack,40+bytes));unchanged(before,c);
    }
    _mm_setcsr(saved_mxcsr);assert(VirtualFree(memory,0,MEM_RELEASE));
}
typedef struct {void *low,*high;unsigned called;} alt_context;
static void alt(void *opaque) {
    alt_context *c=opaque;NT_TIB *tib=(NT_TIB *)NtCurrentTeb();unsigned local;
    assert((uintptr_t)&local>=(uintptr_t)c->low && (uintptr_t)&local<(uintptr_t)c->high);
    assert(tib->StackBase==c->high && tib->StackLimit==c->low);++c->called;
}
int main(int argc,char **argv) {
    uint64_t stack[40];CPU c=frame(stack),before;uint64_t target=(uintptr_t)zero;
    if(argc>1) {
        if(!strcmp(argv[1],"--invalid-stack-bytes"))fp_native_call(&c,target,257);
        else if(!strcmp(argv[1],"--unaligned-stack-bytes"))fp_native_call(&c,target,1);
        else if(!strcmp(argv[1],"--invalid-alt-bounds"))fp_on_alt_stack(stack,stack,alt,NULL);
        else return 2;
        return 99;
    }
    before=c;fp_native_call(&c,target,0);assert(c.rax==UINT64_C(0xfeca12345678));unchanged(before,c);
    c=frame(stack);before=c;fp_native_call(&c,(uintptr_t)four,0);assert(c.rax==50);unchanged(before,c);
    c=frame(stack);before=c;fp_native_call(&c,(uintptr_t)sixteen,96);assert(c.rax==1496);unchanged(before,c);
    c=frame(stack);before=c;fp_native_call(&c,(uintptr_t)thirtysix,256);assert(c.rax==666);unchanged(before,c);
    c=frame(stack);c.xmm[1].f64[0]=2.5;c.xmm[3].f32[0]=4.5f;
    double fifth=5.5,seventh=7.5;memcpy(&stack[5],&fifth,8);stack[6]=6;memcpy(&stack[7],&seventh,8);stack[8]=8;
    before=c;fp_native_call(&c,(uintptr_t)mixed,32);assert(c.xmm[0].f64[0]==213);unchanged(before,c);
    c=frame(stack);before=c;assert(fp_test_nonvolatile(&c,(uintptr_t)fp_test_return_registers,0));
    assert(c.rax==UINT64_C(0x123456789abcdef0) && c.rdx==UINT64_C(0x0fedcba9876543210));
    for(unsigned i=0;i<16;++i)assert(c.xmm[0].u8[i]==i*17 && c.xmm[1].u8[i]==i*17);
    unchanged(before,c);
    for(unsigned count=0;count<=32;++count) {
        uint64_t expected=0,snapshot[40];c=frame(stack);c.rcx=count;before=c;
        memcpy(snapshot,stack,sizeof(stack));
        for(unsigned i=0;i<count;++i)expected+=(5+i)*(i+1);
        fp_native_call(&c,(uintptr_t)fp_test_stack_words,count*8);
        assert(c.rax==expected && !memcmp(stack,snapshot,sizeof(stack)));unchanged(before,c);
    }
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();void *old_top=tib->StackBase,*old_low=tib->StackLimit;
    size_t bytes=1024*1024;void *memory=VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(memory);
    alt_context context={memory,(char *)memory+bytes,0};
    fp_on_alt_stack(context.high,context.low,alt,&context);
    assert(context.called==1 && tib->StackBase==old_top && tib->StackLimit==old_low);
    assert(VirtualFree(memory,0,MEM_RELEASE));assert(calls==5);
    guard_page_cases();
    puts("{\"success\":true,\"native_calls\":204,\"every_stack_length_0_to_32\":true,\"max_stack_arguments\":32,\"mixed_float\":true,\"stack_alignment\":true,\"all_nonvolatile_GPR_XMM\":true,\"CPU_preservation_single_pop\":true,\"alt_stack_TIB_bounds\":true,\"guard_page_lengths\":33,\"guard_MXCSR_states\":5,\"guard_source_no_overread\":true,\"LastError_input_output\":true,\"guest_stack_unchanged\":true}");
    return 0;
}
