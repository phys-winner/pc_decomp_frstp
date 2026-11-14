#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <assert.h>
#include <stdio.h>
#include <inttypes.h>
#include "cpu64.h"
void e35_baseline(CPU *c);
void e35_legacy(CPU *c);
void e35_candidate(CPU *c);
int e35_admit(const CPU *c);
int e35_test_working_admit(const CPU *c,uint64_t start);
int e35_test_stack_admit(const CPU *c,uint64_t a,uint64_t b,uint64_t sp);

#define ARENA_BYTES 0x20000u
#define CPU_OFFSET 0x1c000u
/* Test-only equivalents of the caller's TLS identity and nesting boundary. */
static _Thread_local CPU *observed_active_cpu;
static _Thread_local unsigned observed_depth;
static int native_mode;
static unsigned char *arena;
static CPU *cpu;
static unsigned char *initial,*expected;
static unsigned fault_count;
static DWORD fault_code;
static ULONG_PTR fault_access,fault_address;
static ULONG_PTR fault_parameter0,fault_parameter1;
static DWORD fault_parameter_count;
static unsigned fault_depth,fault_identity,fault_host_mxcsr;
static CPU fault_cpu;
static void *fault_page;
static int fault_sse;

static void reset_cpu(unsigned layout,unsigned pattern,unsigned control) {
    uint32_t random=0x75319bdfu+layout*31+pattern;
    for(unsigned i=0;i<ARENA_BYTES;++i){random^=random<<13;random^=random>>17;random^=random<<5;arena[i]=(unsigned char)random;}
    memset(cpu,0x5a,sizeof(*cpu));
    cpu->rdx=(uintptr_t)(arena+0x4000);cpu->rcx=(uintptr_t)(arena+0x5000);cpu->rsp=(uintptr_t)(arena+0x8000);
    if(native_mode)cpu->rsp+=8;
    uintptr_t s=cpu->rsp,p=(uintptr_t)cpu;
    switch(layout) {
    case 1:cpu->rcx=cpu->rdx;break;
    case 2:cpu->rcx=cpu->rdx+8;break;
    case 3:cpu->rcx=cpu->rdx-8;break;
    case 4:cpu->rdx=s-0x108;break;
    case 5:cpu->rdx=s-0x18;break;
    case 6:cpu->rcx=s-0x108;break;
    case 7:cpu->rcx=s-0x28;break;
    case 8:cpu->rcx=s-0xa8;break;
    case 9:cpu->rcx=s-0x18;break;
    case 10:cpu->rcx=cpu->rdx=s-0x108;break;
    case 11:cpu->rdx=s-0x100;cpu->rcx=cpu->rdx+8;break;
    case 12:cpu->rdx=s-0x28;cpu->rcx=s-0x40;break;
    case 13:cpu->rdx++;cpu->rcx+=3;cpu->rsp+=5;break;
    case 14:cpu->rcx=s-0x80;cpu->rdx=s-0x40;break;
    case 15:cpu->rcx=p+0xb8;break;
    case 16:cpu->rdx=p+0xb8;break;
    case 17:cpu->rsp=p+sizeof(*cpu);break;
    case 18:cpu->rcx=p+sizeof(*cpu)-8;break;
    case 19:cpu->rdx=p+sizeof(*cpu)-1;break;
    case 20:cpu->rcx=cpu->rdx=p+0x150;break;
    case 21:cpu->rcx=p+sizeof(*cpu);cpu->rdx=p-64;break;
    case 22:cpu->rcx=p-64;cpu->rdx=p+sizeof(*cpu);break;
    /* The generated RET only increments RSP: these return slots overlap CPU,
     * while all actual scratch writes remain disjoint from it. */
    case 23:cpu->rsp=p;break;
    case 24:cpu->rsp=p-7;break;
    case 25:cpu->rsp=p+7;break;
    case 26:cpu->rsp=p+8;break;
    }
    static const uint32_t edges[]={0,0x80000000,1,0x807fffff,0x00800000,0x3f800000,0xbf800000,0x7f7fffff,
        0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0xffc23456,0x7f800001,0xff800001,0x3f800001};
    for(unsigned i=0;i<16;++i) {
        uint32_t value;
        if(pattern==0){float f=i%5==0?1.0f:0.0f;memcpy(&value,&f,4);}
        else if(pattern==1){float f=(float)((int)(i*7%19)-9)*0.125f;memcpy(&value,&f,4);}
        else if(pattern==2)value=edges[i];
        else {random^=random<<13;random^=random>>17;random^=random<<5;value=random;}
        wr32(cpu->rdx+i*4,value);
    }
    /* Deliberately distinct from host MXCSR; this body never reads the guest field. */
    cpu->mxcsr=0xf00d0000u|(control^0x6000u);
    observed_active_cpu=cpu;observed_depth=4;
}

static unsigned first_difference(const unsigned char *a,const unsigned char *b,unsigned size) {
    unsigned i=0;while(i<size && a[i]==b[i])++i;return i;
}
static int normal_cases(int legacy) {
    void (*reference)(CPU *)=legacy?e35_legacy:e35_baseline;
    unsigned cases=0,fast=0,fallback=0,mismatch=0,environment_mismatch=0,patterns[4]={0},environment_patterns[4]={0};
    for(unsigned layout=0;layout<27;++layout)for(unsigned state=0;state<32;++state)for(unsigned pattern=0;pattern<4;++pattern) {
        unsigned control=0x1f80|((state&3)<<13)|((state&4)?0x40:0)|((state&8)?0x8000:0)|((state&16)?0x3f:0);
        reset_cpu(layout,pattern,control);memcpy(initial,arena,ARENA_BYTES);
        _mm_setcsr(control);int admitted=e35_admit(cpu);assert(admitted==(layout<15 || layout>20));
        SetLastError(0xe5ef1234);(admitted?reference:e35_legacy)(cpu);unsigned mxcsr=_mm_getcsr();DWORD error=GetLastError();
        _mm_setcsr(0x1f80);memcpy(expected,arena,ARENA_BYTES);memcpy(arena,initial,ARENA_BYTES);
        _mm_setcsr(control);SetLastError(0xe5ef1234);e35_candidate(cpu);
        unsigned actual_mxcsr=_mm_getcsr();DWORD actual_error=GetLastError();_mm_setcsr(0x1f80);
        assert(observed_active_cpu==cpu && observed_depth==4);
        unsigned difference=first_difference(expected,arena,ARENA_BYTES);
        if(difference!=ARENA_BYTES) {
            if(mismatch<16)printf("{\"mismatch\":true,\"layout\":%u,\"state\":%u,\"pattern\":%u,\"offset\":%u,\"expected_u32\":%u,\"actual_u32\":%u,\"cpu_region\":%s}\n",
                layout,state,pattern,difference,rd32((uintptr_t)expected+(difference&~3u)),rd32((uintptr_t)arena+(difference&~3u)),difference>=CPU_OFFSET && difference<CPU_OFFSET+sizeof(CPU)?"true":"false");
            ++mismatch;++patterns[pattern];
        }
        if(mxcsr!=actual_mxcsr || error!=actual_error || actual_error!=0xe5ef1234) {
            if(environment_mismatch<16)printf("{\"environment_mismatch\":true,\"layout\":%u,\"state\":%u,\"pattern\":%u,\"expected_mxcsr\":%u,\"actual_mxcsr\":%u,\"expected_LastError\":%lu,\"actual_LastError\":%lu}\n",layout,state,pattern,mxcsr,actual_mxcsr,error,actual_error);
            ++environment_mismatch;++environment_patterns[pattern];
        }
        ++cases;if(admitted)++fast;else ++fallback;
    }
    printf("{\"normal_cases\":%u,\"fast\":%u,\"fallback\":%u,\"memory_mismatches\":%u,\"environment_mismatches\":%u,\"memory_mismatches_by_pattern\":[%u,%u,%u,%u],\"environment_mismatches_by_pattern\":[%u,%u,%u,%u]}\n",cases,fast,fallback,mismatch,environment_mismatch,patterns[0],patterns[1],patterns[2],patterns[3],environment_patterns[0],environment_patterns[1],environment_patterns[2],environment_patterns[3]);
    return 0;
}
static int admission_cases(void) {
    unsigned count=0;reset_cpu(0,0,0x1f80);CPU saved=*cpu;_mm_setcsr(0x1f80);
    assert(!e35_admit(NULL));++count;
    assert(!e35_admit((CPU *)(uintptr_t)UINT64_MAX));++count;
    static const uint64_t bad_pointer[]={UINT64_MAX,UINT64_MAX-63,UINT64_MAX-31};
    for(unsigned i=0;i<3;++i) {
        *cpu=saved;cpu->rdx=bad_pointer[i];assert(!e35_admit(cpu));++count;
        *cpu=saved;cpu->rcx=bad_pointer[i];assert(!e35_admit(cpu));++count;
    }
    static const uint64_t bad_stack[]={0,0x107,UINT64_MAX,UINT64_MAX-7};
    for(unsigned i=0;i<4;++i){*cpu=saved;cpu->rsp=bad_stack[i];assert(!e35_admit(cpu));++count;}
    for(unsigned bit=7;bit<=12;++bit) {
        *cpu=saved;_mm_setcsr(0x1f80&~(1u<<bit));assert(!e35_admit(cpu));++count;
    }
    _mm_setcsr(0x1f80);*cpu=saved;assert(e35_admit(cpu));++count;
    uint64_t starts[]={cpu->rdx,cpu->rcx,cpu->rsp-0x108};
    uint64_t ends[]={cpu->rdx+64,cpu->rcx+64,cpu->rsp+8};
    for(unsigned i=0;i<3;++i) {
        assert(e35_test_working_admit(cpu,starts[i]-sizeof(CPU)));++count;
        assert(e35_test_working_admit(cpu,ends[i]));++count;
        assert(!e35_test_working_admit(cpu,starts[i]-sizeof(CPU)+1));++count;
        assert(!e35_test_working_admit(cpu,ends[i]-1));++count;
    }
    assert(!e35_test_working_admit(cpu,cpu->rsp));++count;
    assert(!e35_test_working_admit(cpu,UINT64_MAX));++count;
    assert(e35_test_working_admit(cpu,(uintptr_t)(arena+0x12000)));++count;
    uint64_t low=(uintptr_t)(arena+0x12000),high=low+4096;
    assert(!e35_test_stack_admit(cpu,high,low,low));++count;
    assert(!e35_test_stack_admit(cpu,low,low,low));++count;
    assert(!e35_test_stack_admit(cpu,low,high,low-1));++count;
    assert(!e35_test_stack_admit(cpu,low,high,high));++count;
    assert(e35_test_stack_admit(cpu,low,high,low));++count;
    assert(e35_test_stack_admit(cpu,low,high,high-1));++count;
    for(unsigned layout=23;layout<27;++layout) {
        reset_cpu(layout,0,0x1f80);assert(e35_admit(cpu));++count;
    }
    *cpu=saved;cpu->rsp=0x108;assert(e35_admit(cpu));++count;
    *cpu=saved;cpu->rsp=UINT64_MAX-8;assert(e35_admit(cpu));++count;
    *cpu=saved;cpu->rdx=UINT64_MAX-64;assert(e35_admit(cpu));++count;
    *cpu=saved;cpu->rcx=UINT64_MAX-64;assert(e35_admit(cpu));++count;
    printf("{\"admission_cases\":%u,\"invalid_addresses_not_dereferenced\":true}\n",count);return 0;
}

/* Full-state, single-thread isolated oracle. No original image initialization. */
#include "native_bytes.h"
#include <stddef.h>
typedef struct {uint64_t gpr[16],flags,pad;XMM xmm[16];} native_state;
__declspec(align(16)) native_state dt_in,dt_out;
uint64_t dt_code;
void run_native(void);
_Static_assert(offsetof(native_state,flags)==128 && offsetof(native_state,xmm)==144 && sizeof(native_state)==400,"native bridge layout");
_Static_assert(offsetof(CPU,rip)==128 && offsetof(CPU,xmm)==184 && sizeof(CPU)==448,"CPU layout");

static void native_invoke(CPU *c) {
    CPU result=*c;
    memcpy(dt_in.gpr,c,128);memcpy(dt_in.xmm,c->xmm,256);dt_in.flags=0x202;
    uint64_t return_slot=c->rsp,saved_return=rd64(return_slot);
    /* Every original MOVAPS stack address is aligned for an ABI entry RSP. */
    assert((return_slot&15)==8);
    assert(c->rdx+64<=return_slot || c->rdx>=return_slot+8);
    assert(c->rcx+64<=return_slot || c->rcx>=return_slot+8);
    run_native();
    wr64(return_slot,saved_return);
    memcpy(&result,dt_out.gpr,128);memcpy(result.xmm,dt_out.xmm,256);
    result.rip=UINT64_C(0x140e35a50);
    result.cf=(dt_out.flags>>0)&1;result.pf=(dt_out.flags>>2)&1;result.af=(dt_out.flags>>4)&1;
    result.zf=(dt_out.flags>>6)&1;result.sf=(dt_out.flags>>7)&1;result.of=(dt_out.flags>>11)&1;
    *c=result;
}

static int native_cases(void) {
    void *code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(code);
    memcpy(code,original_bytes,sizeof(original_bytes));DWORD old;
    assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
    assert(FlushInstructionCache(GetCurrentProcess(),code,sizeof(original_bytes)));
    dt_code=(uintptr_t)code;native_mode=1;
    const unsigned layouts[]={0,1,2,3,4,6,8,10,11,14,21,22};
    unsigned cases=0,memory_mismatch=0,environment_mismatch=0,by_variant[3]={0},env_variant[3]={0};
    void (*variants[])(CPU *)={e35_baseline,e35_candidate,e35_legacy};
    for(unsigned li=0;li<sizeof(layouts)/sizeof(layouts[0]);++li)for(unsigned state=0;state<32;++state)for(unsigned pattern=0;pattern<4;++pattern) {
        unsigned layout=layouts[li];
        unsigned control=0x1f80|((state&3)<<13)|((state&4)?0x40:0)|((state&8)?0x8000:0)|((state&16)?0x3f:0);
        reset_cpu(layout,pattern,control);memcpy(initial,arena,ARENA_BYTES);
        _mm_setcsr(control);SetLastError(0xe5ef1234);native_invoke(cpu);
        unsigned native_mxcsr=_mm_getcsr();DWORD native_error=GetLastError();_mm_setcsr(0x1f80);
        memcpy(expected,arena,ARENA_BYTES);
        for(unsigned v=0;v<3;++v) {
            memcpy(arena,initial,ARENA_BYTES);_mm_setcsr(control);SetLastError(0xe5ef1234);variants[v](cpu);
            unsigned actual_mxcsr=_mm_getcsr();DWORD actual_error=GetLastError();_mm_setcsr(0x1f80);
            unsigned difference=first_difference(expected,arena,ARENA_BYTES);
            if(difference!=ARENA_BYTES) {
                if(by_variant[v]<8)printf("{\"native_mismatch\":true,\"variant\":%u,\"layout\":%u,\"state\":%u,\"pattern\":%u,\"offset\":%u,\"native_u32\":%u,\"actual_u32\":%u}\n",v,layout,state,pattern,difference,rd32((uintptr_t)expected+(difference&~3u)),rd32((uintptr_t)arena+(difference&~3u)));
                ++by_variant[v];if(v<2)++memory_mismatch;
            }
            if(native_mxcsr!=actual_mxcsr || native_error!=actual_error || actual_error!=0xe5ef1234) {
                if(env_variant[v]<8)printf("{\"native_environment_mismatch\":true,\"variant\":%u,\"layout\":%u,\"state\":%u,\"pattern\":%u,\"native_mxcsr\":%u,\"actual_mxcsr\":%u}\n",v,layout,state,pattern,native_mxcsr,actual_mxcsr);
                ++env_variant[v];if(v<2)++environment_mismatch;
            }
            assert(observed_active_cpu==cpu && observed_depth==4);
        }
        ++cases;
    }
    native_mode=0;assert(VirtualFree(code,0,MEM_RELEASE));
    printf("{\"native_cases\":%u,\"variants\":3,\"memory_mismatches\":%u,\"environment_mismatches\":%u,\"ordered_memory\":%u,\"local_memory\":%u,\"legacy_memory\":%u,\"ordered_environment\":%u,\"local_environment\":%u,\"legacy_environment\":%u}\n",cases,memory_mismatch,environment_mismatch,by_variant[0],by_variant[1],by_variant[2],env_variant[0],env_variant[1],env_variant[2]);
    return 0;
}

static int stack_fallback_cases(void) {
    __declspec(align(16)) unsigned char storage[4096],saved[4096],expected_storage[4096];
    unsigned cases=0,memory_mismatch=0,environment_mismatch=0;
    for(unsigned role=0;role<3;++role)for(unsigned state=0;state<32;++state)for(unsigned pattern=0;pattern<4;++pattern) {
        unsigned control=0x1f80|((state&3)<<13)|((state&4)?0x40:0)|((state&8)?0x8000:0)|((state&16)?0x3f:0);
        reset_cpu(0,pattern,control);memset(storage,0xa6,sizeof(storage));
        if(role==0){memcpy(storage+0x100,(void *)(uintptr_t)cpu->rdx,64);cpu->rdx=(uintptr_t)(storage+0x100);}
        if(role==1)cpu->rcx=(uintptr_t)(storage+0x200);
        if(role==2)cpu->rsp=(uintptr_t)(storage+0x800);
        uint64_t sp;__asm__ volatile("movq %%rsp,%0" : "=r"(sp));
        assert(!e35_test_stack_admit(cpu,__readgsqword(0x10),__readgsqword(8),sp));
        memcpy(initial,arena,ARENA_BYTES);memcpy(saved,storage,sizeof(storage));
        _mm_setcsr(control);assert(e35_admit(cpu));SetLastError(0xe5ef1234);e35_legacy(cpu);
        unsigned expected_mxcsr=_mm_getcsr();DWORD expected_error=GetLastError();_mm_setcsr(0x1f80);
        memcpy(expected,arena,ARENA_BYTES);memcpy(expected_storage,storage,sizeof(storage));
        memcpy(arena,initial,ARENA_BYTES);memcpy(storage,saved,sizeof(storage));
        _mm_setcsr(control);SetLastError(0xe5ef1234);e35_candidate(cpu);
        unsigned actual_mxcsr=_mm_getcsr();DWORD actual_error=GetLastError();_mm_setcsr(0x1f80);
        if(memcmp(expected,arena,ARENA_BYTES) || memcmp(expected_storage,storage,sizeof(storage)))++memory_mismatch;
        if(expected_mxcsr!=actual_mxcsr || expected_error!=actual_error || actual_error!=0xe5ef1234)++environment_mismatch;
        assert(observed_active_cpu==cpu && observed_depth==4);++cases;
    }
    printf("{\"stack_fallback_cases\":%u,\"memory_mismatches\":%u,\"environment_mismatches\":%u}\n",cases,memory_mismatch,environment_mismatch);
    return 0;
}


static LONG CALLBACK capture_fault(EXCEPTION_POINTERS *p) {
    DWORD saved_error=GetLastError();EXCEPTION_RECORD *e=p->ExceptionRecord;
    if(fault_count || (e->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION && !fault_sse))return EXCEPTION_CONTINUE_SEARCH;
    ++fault_count;fault_code=e->ExceptionCode;fault_parameter_count=e->NumberParameters;
    fault_parameter0=e->NumberParameters?e->ExceptionInformation[0]:0;
    fault_parameter1=e->NumberParameters>=2?e->ExceptionInformation[1]:0;
    fault_access=e->ExceptionCode==EXCEPTION_ACCESS_VIOLATION?fault_parameter0:0;
    fault_address=e->ExceptionCode==EXCEPTION_ACCESS_VIOLATION?fault_parameter1:0;
    fault_cpu=*observed_active_cpu;fault_depth=observed_depth;fault_identity=observed_active_cpu==cpu;
    fault_host_mxcsr=p->ContextRecord->MxCsr;
    if(e->ExceptionCode==EXCEPTION_ACCESS_VIOLATION) {
        DWORD old;if(!VirtualProtect(fault_page,4096,PAGE_READWRITE,&old))return EXCEPTION_CONTINUE_SEARCH;
    } else p->ContextRecord->MxCsr|=0x1f80;
    SetLastError(saved_error);return EXCEPTION_CONTINUE_EXECUTION;
}
static void write_bytes(const char *name,const void *data,size_t size) {
    FILE *f=fopen(name,"wb");assert(f && fwrite(data,1,size,f)==size);assert(!fclose(f));
}
static int fault_case(int candidate,const wchar_t *kind) {
    reset_cpu(0,0,0x1f80);unsigned control=0x1f80;
    if(!wcscmp(kind,L"input"))cpu->rdx=(uintptr_t)(arena+0x10000);
    else if(!wcscmp(kind,L"output"))cpu->rcx=(uintptr_t)(arena+0x10000);
    else if(!wcscmp(kind,L"stack"))cpu->rsp=(uintptr_t)(arena+0x10800);
    else if(!wcscmp(kind,L"stack-lower"))cpu->rsp=(uintptr_t)(arena+0x11080);
    else if(!wcscmp(kind,L"invalid")) {
        fault_sse=1;control=0x1f00;
        for(unsigned i=0;i<16;++i)wr32(cpu->rdx+4*i,0x7f800001);
    } else if(!wcscmp(kind,L"overflow")) {
        fault_sse=1;control=0x1b80;for(unsigned i=0;i<16;++i)wr32(cpu->rdx+4*i,0x7f7fffff);
    } else assert(!"unknown fault case");
    if(!fault_sse) {fault_page=arena+0x10000;DWORD old;assert(VirtualProtect(fault_page,4096,PAGE_NOACCESS,&old));}
    PVOID handler=AddVectoredExceptionHandler(1,capture_fault);assert(handler);
    _mm_setcsr(control);int admitted=e35_admit(cpu);assert(admitted==!fault_sse);
    SetLastError(0xe5ef1234);if(candidate)e35_candidate(cpu);else (admitted?e35_baseline:e35_legacy)(cpu);
    unsigned after_mxcsr=_mm_getcsr();DWORD after_error=GetLastError();_mm_setcsr(0x1f80);
    assert(fault_count==1 && fault_identity && fault_depth==4 && observed_active_cpu==cpu && observed_depth==4);
    assert(RemoveVectoredExceptionHandler(handler));
    write_bytes("fault-cpu.bin",&fault_cpu,sizeof(fault_cpu));write_bytes("fault-final-arena.bin",arena,ARENA_BYTES);
    printf("{\"faults\":%u,\"code\":%lu,\"access\":%llu,\"address\":%llu,\"parameter_count\":%lu,\"parameter0\":%llu,\"parameter1\":%llu,\"active_cpu_identity\":%u,\"depth\":%u,\"fault_mxcsr\":%u,\"after_mxcsr\":%u,\"after_LastError\":%lu,\"admitted\":%d,\"resumed_to_return\":true}\n",
        fault_count,fault_code,(unsigned long long)fault_access,(unsigned long long)fault_address,fault_parameter_count,(unsigned long long)fault_parameter0,(unsigned long long)fault_parameter1,fault_identity,fault_depth,fault_host_mxcsr,after_mxcsr,after_error,admitted);
    return 0;
}
static int benchmark(int candidate) {
    void *code=NULL;
    if(candidate==3) {
        code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(code);
        memcpy(code,original_bytes,sizeof(original_bytes));DWORD old;
        assert(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),code,sizeof(original_bytes)));
        dt_code=(uintptr_t)code;native_mode=1;
    }
    reset_cpu(0,0,0x1f80);
    const unsigned count=200000,rounds=9;LARGE_INTEGER frequency,a,b;assert(QueryPerformanceFrequency(&frequency));
    uint64_t guest_rsp=cpu->rsp;
    void (*fn)(CPU *)=candidate==3?native_invoke:candidate==2?e35_legacy:candidate?e35_candidate:e35_baseline;
    for(unsigned round=0;round<rounds;++round) {
        _mm_setcsr(0x1f80);assert(QueryPerformanceCounter(&a));
        for(unsigned i=0;i<count;++i){cpu->rsp=guest_rsp;fn(cpu);}
        assert(QueryPerformanceCounter(&b));
        assert(cpu->rsp==guest_rsp+8);
        printf("{\"round\":%u,\"iterations\":%u,\"ticks\":%lld,\"frequency\":%lld,\"candidate\":%s}\n",round,count,b.QuadPart-a.QuadPart,frequency.QuadPart,candidate?"true":"false");
    }
    if(code){native_mode=0;assert(VirtualFree(code,0,MEM_RELEASE));}
    return 0;
}
int wmain(int argc,wchar_t **argv) {
    arena=VirtualAlloc((void *)(uintptr_t)UINT64_C(0x700000000),ARENA_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(arena);
    cpu=(CPU *)(arena+CPU_OFFSET);initial=malloc(ARENA_BYTES);expected=malloc(ARENA_BYTES);assert(initial && expected);
    if(argc==2 && !wcscmp(argv[1],L"--native"))return native_cases();
    if(argc==2 && !wcscmp(argv[1],L"--stack-fallback"))return stack_fallback_cases();
    if(argc==2 && !wcscmp(argv[1],L"--normal"))return normal_cases(0);
    if(argc==2 && !wcscmp(argv[1],L"--legacy"))return normal_cases(1);
    if(argc==2 && !wcscmp(argv[1],L"--admission"))return admission_cases();
    if(argc==4 && !wcscmp(argv[1],L"--fault"))return fault_case(!wcscmp(argv[2],L"candidate"),argv[3]);
    if(argc==3 && !wcscmp(argv[1],L"--bench"))return benchmark(!wcscmp(argv[2],L"native")?3:!wcscmp(argv[2],L"legacy")?2:!wcscmp(argv[2],L"candidate"));
    assert(!"unknown fixture mode");return 1;
}
