/* Real VEH fallback and qsort fast path against exact original comparator bytes. */
#define wmain fp_unused_host_wmain
#include "../native/windows/host.c"
#undef wmain
#include <assert.h>
#include "qsort_original_comparator.h"

static unsigned comparisons,descending_comparisons,float_comparisons;
static void comparator_entry(CPU *c) {
    CPU before=*c;
    uint32_t left=**(uint32_t **)(uintptr_t)c->rcx,right=**(uint32_t **)(uintptr_t)c->rdx;
    L_000141C561A0(c);
    assert((int32_t)c->rax==(left>right)-(left<right));
    assert(c->rsp==before.rsp+8);
    before.rax=c->rax;before.rcx=c->rcx;before.rdx=c->rdx;before.r8=c->r8;
    before.rsp=c->rsp;before.rip=c->rip;
    before.cf=c->cf;before.zf=c->zf;before.sf=c->sf;before.of=c->of;before.pf=c->pf;before.af=c->af;
    assert(!memcmp(&before,c,sizeof(*c)));++comparisons;
}
static void descending_entry(CPU *c) {
    CPU before=*c;
    uint32_t left=rd32(c->rcx),right=rd32(c->rdx);
    L_00014102F320(c);
    assert((int32_t)c->rax==(left<right)-(left>right));
    assert(c->rsp==before.rsp+8);
    before.rax=c->rax;before.rcx=c->rcx;before.rsp=c->rsp;before.rip=c->rip;
    before.cf=c->cf;before.zf=c->zf;before.sf=c->sf;before.of=c->of;before.pf=c->pf;before.af=c->af;
    assert(!memcmp(&before,c,sizeof(*c)));++descending_comparisons;
}
static __declspec(noinline) void float_entry(CPU *c) {
    CPU before=*c;L_0001410310D0(c);assert(c->rsp==before.rsp+8);
    before.rax=c->rax;before.r8=c->r8;before.xmm[0]=c->xmm[0];before.rsp=c->rsp;before.rip=c->rip;
    before.cf=c->cf;before.zf=c->zf;before.sf=c->sf;before.of=c->of;before.pf=c->pf;before.af=c->af;
    assert(!memcmp(&before,c,sizeof(*c)));++float_comparisons;
}
static unsigned expected_static_depth;
static void static_leaf(CPU *c) {
    assert(active_cpu==c && call_depth==(int)expected_static_depth);
    assert(GetLastError()==0xe5ef1234 && last_target==GVA(0x141c56220));
    returned(c,c->rcx^UINT64_C(0xe5ef1234));
    SetLastError(0xe5ef7654);
}
static void static_nested(CPU *c) {
    assert(active_cpu==c && call_depth==4 && GetLastError()==0xe5ef1234);
    uint64_t sp=c->rsp;push64(c,UINT64_C(0xe5efd00d));
    dispatch(c,GVA(0x141c56220));
    assert(c->rsp==sp && c->rip==UINT64_C(0xe5efd00d));
    assert(active_cpu==c && call_depth==4 && GetLastError()==0xe5ef7654);
    returned(c,c->rax+7);
}
const recomp_entry_t recomp_dispatch_table[]={
    {UINT64_C(0x14102f320),descending_entry},{UINT64_C(0x14102f360),descending_entry},
    {UINT64_C(0x1410310d0),float_entry},{UINT64_C(0x141031150),float_entry},
    {UINT64_C(0x141c561a0),comparator_entry},{UINT64_C(0x141c561e0),comparator_entry},
    {UINT64_C(0x141c56220),static_leaf},{UINT64_C(0x141c56240),static_nested}};
const uint32_t recomp_dispatch_count=8;
const hle_import_t recomp_imports[HLE_COUNT]={{0}};
frostpunk_supplemental_fn frostpunk_supplemental_lookup(uint64_t target) {(void)target;return NULL;}
static int unsigned_compare(const void *a,const void *b) {
    uint32_t left=*(const uint32_t *)a,right=*(const uint32_t *)b;
    return (left>right)-(left<right);
}
typedef void (__cdecl *sort_fn)(void *,size_t,size_t,int (__cdecl *)(const void *,const void *));
typedef int (__cdecl *compare_fn)(const void *,const void *);

static void sort_bridge(sort_fn sort,void *base,size_t count,size_t width,uint64_t comparator,uint64_t *stack) {
    guest_stack_limit=(uintptr_t)stack;guest_stack_base=(uintptr_t)stack+65536;
    CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);*(uint64_t *)(uintptr_t)c.rsp=0xe5efcafe;
    c.rcx=(uintptr_t)base;c.rdx=count;c.r8=width;c.r9=comparator;
    CPU before=c;CPU *previous=active_cpu;dispatch(&c,(uintptr_t)sort);
    assert(c.rsp==before.rsp+8 && c.rip==0xe5efcafe);
    before.rax=c.rax;before.rdx=c.rdx;before.xmm[0]=c.xmm[0];before.xmm[1]=c.xmm[1];
    before.rsp=c.rsp;before.rip=c.rip;assert(!memcmp(&before,&c,sizeof(c)));
    assert(active_cpu==previous && !callback_depth && !call_depth);
}

static void static_dispatch_cases(uint64_t *stack) {
    CPU outer={0};CPU *saved_cpu=active_cpu;int saved_depth=call_depth;
    unsigned saved_mxcsr=_mm_getcsr();
    for(unsigned nested=0;nested<2;++nested) {
        CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);
        c.rcx=UINT64_C(0x123456789abcdef0);wr64(c.rsp,UINT64_C(0xe5efcafe));
        CPU expected=c;unsigned char expected_stack[128];
        memcpy(expected_stack,(void *)(uintptr_t)(c.rsp-64),sizeof(expected_stack));
        if(nested)wr64((uintptr_t)(expected_stack+56),UINT64_C(0xe5efd00d));
        expected.rax=(c.rcx^UINT64_C(0xe5ef1234))+(nested?7:0);
        expected.rsp+=8;expected.rip=UINT64_C(0xe5efcafe);
        active_cpu=&outer;call_depth=3;expected_static_depth=4+nested;
        SetLastError(0xe5ef1234);
        dispatch(&c,GVA(nested?0x141c56240:0x141c56220));
        assert(!memcmp(&c,&expected,sizeof(c)));
        assert(!memcmp(expected_stack,(void *)(uintptr_t)(expected.rsp-8-64),sizeof(expected_stack)));
        assert(GetLastError()==0xe5ef7654 && _mm_getcsr()==saved_mxcsr);
        assert(active_cpu==&outer && call_depth==3 && last_target==GVA(0x141c56220));
    }
    active_cpu=saved_cpu;call_depth=saved_depth;
}

int wmain(void) {
    setvbuf(stderr,NULL,_IONBF,0);fp_native_validation_init();
    void *handler=AddVectoredExceptionHandler(1,exceptions);assert(handler);
    unsigned char *image=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(image);
    fp_section section={0,4096,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ};
    game=(fp_image){0};game.base=image;game.size=4096;game.sections=&section;game.section_count=1;
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x141c561a0);self_test_mode=0;tls_attached=1;
    MEMORY_BASIC_INFORMATION memory;assert(VirtualQuery(image,&memory,sizeof(memory)));
    assert(memory.Protect==PAGE_READWRITE && fp_image_code(&game,(uintptr_t)image));
    HMODULE crt=LoadLibraryW(L"api-ms-win-crt-utility-l1-1-0.dll");assert(crt);
    sort_fn sort=(sort_fn)GetProcAddress(crt,"qsort");assert(sort);
    HMODULE implementation;assert(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR)(uintptr_t)sort,&implementation));
    wchar_t module_path[32768];assert(GetModuleFileNameW(implementation,module_path,32768));
    wchar_t system[32768];UINT length=GetSystemDirectoryW(system,32768);assert(length && length<32768);
    assert(!_wcsnicmp(module_path,system,length) && module_path[length]==L'\\');
    uint64_t *stack=VirtualAlloc(NULL,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(stack);
    guest_stack_limit=(uintptr_t)stack;guest_stack_base=(uintptr_t)stack+65536;
    CPU error_call={0};error_call.rsp=(uintptr_t)(stack+4096);error_call.rcx=0xe5ef7654;
    wr64(error_call.rsp,0xe5efcafe);self_test_mode=1;CPU *previous_cpu=active_cpu;
    SetLastError(0xe5ef1234);dispatch(&error_call,(uintptr_t)image+80);
    assert(GetLastError()==0xe5ef7654 && error_call.rax==1 && error_call.rip==0xe5efcafe);
    assert(error_call.rsp==(uintptr_t)(stack+4097) && active_cpu==previous_cpu && !call_depth);
    self_test_mode=0;static_dispatch_cases(stack);
    unsigned char *oracle=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(oracle);
    memcpy(oracle,qsort_u32_original_bytes,sizeof(qsort_u32_original_bytes));
    memcpy(oracle+256,qsort_pointer_original_bytes,sizeof(qsort_pointer_original_bytes));
    memcpy(oracle+512,qsort_float_original_bytes,sizeof(qsort_float_original_bytes));
    DWORD old;assert(VirtualProtect(oracle,4096,PAGE_EXECUTE_READ,&old));
    assert(FlushInstructionCache(GetCurrentProcess(),oracle,4096));
    compare_fn original_compare=(compare_fn)oracle,original_pointer_compare=(compare_fn)(oracle+256);
    compare_fn original_float_compare=(compare_fn)(oracle+512);
    memcpy(image,qsort_pointer_original_bytes,sizeof(qsort_pointer_original_bytes));
    memcpy(image+64,qsort_pointer_original_bytes,sizeof(qsort_pointer_original_bytes));
    initialize_qsort_callbacks((uintptr_t)sort);assert(qsort_pointer_target==(uintptr_t)sort);
    image[0]^=1;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_pointer_target);image[0]^=1;
    section.size=26;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_pointer_target);section.size=4096;
    unsigned cases=0;uint32_t random=0xcafebabe;
    static const unsigned sizes[]={0,1,2,3,7,16,31,64,127,257,512};
    uint32_t values[512],expected[512];uint32_t *pointers[512],*wanted_pointers[512];
    LARGE_INTEGER frequency,start,end;assert(QueryPerformanceFrequency(&frequency));double pointer_elapsed[4]={0};
    for(unsigned mode=0;mode<4;++mode)for(unsigned trial=0;trial<66;++trial) {
        random=0xcafebabe+trial;
        unsigned count=sizes[trial%(sizeof(sizes)/sizeof(*sizes))];
        for(unsigned i=0;i<count;++i) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            uint32_t value=trial%3==0?random:trial%3==1?(random&7u):UINT32_MAX-i;
            if(i%17==0)value=UINT32_C(0x80000000);if(i%19==0)value=UINT32_MAX;
            values[i]=expected[i]=value;pointers[i]=wanted_pointers[i]=&values[i];
        }
        qsort(expected,count,sizeof(*expected),unsigned_compare);
        sort(wanted_pointers,count,sizeof(*pointers),original_pointer_compare);
        if(mode<2)qsort_pointer_target=0;else initialize_qsort_callbacks((uintptr_t)sort);
        LONG64 callbacks_before=callback_calls;SetLastError(0xe5ef6789);assert(QueryPerformanceCounter(&start));
        if(!mode) {
            guest_stack_base=(uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
            guest_stack_limit=(uintptr_t)((NT_TIB *)NtCurrentTeb())->StackLimit;
            sort(pointers,count,sizeof(*pointers),(int(__cdecl *)(const void *,const void *))image);
        } else {
            sort_bridge(sort,pointers,count,8,(uintptr_t)image+(mode==3?64:0),stack);
        }
        assert(GetLastError()==0xe5ef6789 && QueryPerformanceCounter(&end));
        pointer_elapsed[mode]+=(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart;
        if(mode==2)assert(callback_calls==callbacks_before);else if(count>1)assert(callback_calls>callbacks_before);
        assert(!memcmp(pointers,wanted_pointers,count*sizeof(*pointers)));
        for(unsigned i=0;i<count;++i) {
            assert(pointers[i]>=values && pointers[i]<values+count);
            assert(*pointers[i]==expected[i]);
        }
        assert(!callback_depth && !call_depth);++cases;
    }
    assert(comparisons && comparisons==(unsigned)callback_calls);
    assert(VirtualQuery(image,&memory,sizeof(memory)) && memory.Protect==PAGE_READWRITE);
    memcpy(wanted_pointers,pointers,sizeof(pointers));sort(wanted_pointers,512,sizeof(*pointers),original_pointer_compare);
    image[0]^=1;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_pointer_target);
    unsigned pointer_before=comparisons;sort_bridge(sort,pointers,512,8,(uintptr_t)image,stack);
    assert(comparisons>pointer_before && !memcmp(pointers,wanted_pointers,sizeof(pointers)));image[0]^=1;

    unsigned pointer_pairs=0;unsigned saved_mxcsr=_mm_getcsr();
    static const uint32_t edges[]={0,1,2,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
    for(unsigned state=0;state<64;++state)for(unsigned trial=0;trial<4096+49;++trial) {
        unsigned control=((state&1)?0:0x1f80)|((state&6)<<12)|((state&8)?0x40:0)|
            ((state&16)?0x8000:0)|((state&32)?0x3f:0);
        unsigned char unaligned[48];uint32_t left,right;
        if(trial<49){left=edges[trial/7];right=edges[trial%7];}
        else {random^=random<<13;random^=random>>17;random^=random<<5;left=random;
              random^=random<<13;random^=random>>17;random^=random<<5;right=random;}
        memcpy(unaligned+33,&left,4);memcpy(unaligned+39,&right,4);
        wr64((uintptr_t)(unaligned+1),(uintptr_t)(unaligned+33));
        wr64((uintptr_t)(unaligned+17),(uintptr_t)(unaligned+39));
        CPU c={0};c.rcx=(uintptr_t)(unaligned+1);c.rdx=(uintptr_t)(unaligned+17);
        c.rsp=(uintptr_t)(stack+4096);c.mxcsr=control;_mm_setcsr(control);
        comparator_entry(&c);assert(_mm_getcsr()==control);
        int result=original_pointer_compare(unaligned+1,unaligned+17);assert(_mm_getcsr()==control);
        assert(result==(int32_t)c.rax && result==qsort_pointer_u32(unaligned+1,unaligned+17));
        assert(_mm_getcsr()==control);++pointer_pairs;
    }
    _mm_setcsr(saved_mxcsr);

    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x14102f320);
    memcpy(image,qsort_u32_original_bytes,sizeof(qsort_u32_original_bytes));
    memcpy(image+64,qsort_u32_original_bytes,sizeof(qsort_u32_original_bytes));
    unsigned pair_cases=0;
    for(unsigned trial=0;trial<4096+49;++trial) {
        unsigned char unaligned[16];uint32_t left,right;
        if(trial<49){left=edges[trial/7];right=edges[trial%7];}
        else {random^=random<<13;random^=random>>17;random^=random<<5;left=random;
              random^=random<<13;random^=random>>17;random^=random<<5;right=random;}
        memcpy(unaligned+1,&left,4);memcpy(unaligned+7,&right,4);
        CPU c={0};c.rcx=(uintptr_t)(unaligned+1);c.rdx=(uintptr_t)(unaligned+7);c.rsp=(uintptr_t)(stack+4096);
        descending_entry(&c);
        int expected=original_compare(unaligned+1,unaligned+7);
        assert(expected==(int32_t)c.rax && expected==qsort_u32_desc(unaligned+1,unaligned+7));++pair_cases;
    }
    initialize_qsort_callbacks((uintptr_t)sort);assert(qsort_u32_target==(uintptr_t)sort);
    image[0]^=1;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_u32_target);image[0]^=1;
    section.size=16;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_u32_target);section.size=4096;

    typedef struct {uint32_t key,payload;} record;
    record records[512],wanted[512];
    double elapsed[3]={0};unsigned descending_cases=0;
    for(unsigned mode=0;mode<3;++mode)for(unsigned trial=0;trial<66;++trial) {
        unsigned count=sizes[trial%(sizeof(sizes)/sizeof(*sizes))];random=0x12345678+trial;
        for(unsigned i=0;i<count;++i) {
            random^=random<<13;random^=random>>17;random^=random<<5;
            uint32_t key=trial%3==0?random:trial%3==1?(random&7u):UINT32_MAX-i;
            if(i%17==0)key=UINT32_C(0x80000000);if(i%19==0)key=UINT32_MAX;
            records[i]=wanted[i]=(record){key,i};
        }
        SetLastError(0xe5ef1234);sort(wanted,count,sizeof(*wanted),original_compare);
        DWORD expected_error=GetLastError();assert(expected_error==0xe5ef1234);
        if(mode==1){qsort_u32_target=0;}else initialize_qsort_callbacks((uintptr_t)sort);
        unsigned before=descending_comparisons;LONG64 before_callbacks=callback_calls;
        SetLastError(0xe5ef1234);assert(QueryPerformanceCounter(&start));
        sort_bridge(sort,records,count,8,(uintptr_t)image+(mode==2?64:0),stack);
        DWORD result_error=GetLastError();assert(QueryPerformanceCounter(&end));
        elapsed[mode]+=(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart;
        assert(result_error==expected_error && !memcmp(records,wanted,count*sizeof(*records)));
        if(!mode){assert(before==descending_comparisons && before_callbacks==callback_calls);}
        else if(count>1){assert(descending_comparisons>before && callback_calls>before_callbacks);}
        ++descending_cases;
    }
    memcpy(wanted,records,sizeof(records));sort(wanted,512,sizeof(*wanted),original_compare);
    image[0]^=1;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_u32_target);
    unsigned before=descending_comparisons;
    sort_bridge(sort,records,512,8,(uintptr_t)image,stack);
    assert(descending_comparisons>before && !memcmp(records,wanted,sizeof(records)));image[0]^=1;
    initialize_qsort_callbacks((uintptr_t)sort);assert(qsort_u32_target);

    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x1410310d0);
    memcpy(image,qsort_float_original_bytes,sizeof(qsort_float_original_bytes));
    memcpy(image+128,qsort_float_original_bytes,sizeof(qsort_float_original_bytes));
    initialize_qsort_callbacks((uintptr_t)sort);assert(qsort_float_target==(uintptr_t)sort);
    image[0]^=1;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_float_target);image[0]^=1;
    section.size=67;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_float_target);section.size=4096;
    static const uint32_t float_bits[]={0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,
        0x00800000,0x80800000,0x3f000000,0xbf000000,0x3f800000,0xbf800000,0x3fc00000,
        0xbfc00000,0x3f7fffff,0x3f800001,0x4effffff,0x4f000000,0xcf000000,0xcf000001,
        0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,0xffc12345,0x7f800001,
        0xff800001,0x00800001,0x007ffffe,0x4f000001,0xceffffff};
    unsigned float_pairs=0,original_status_differences=0;
    for(unsigned state=0;state<32;++state)for(unsigned trial=0;trial<1024+4096;++trial) {
        unsigned control=0x1f80|((state&3)<<13)|((state&4)?0x40:0)|((state&8)?0x8000:0)|((state&16)?0x3f:0);
        unsigned char objects[2][520],items[2][50];memset(items,0,sizeof(items));
        uint32_t left_bits,right_bits;
        if(trial<1024){left_bits=float_bits[trial/32];right_bits=float_bits[trial%32];}
        else {random^=random<<13;random^=random>>17;random^=random<<5;left_bits=random;
              random^=random<<13;random^=random>>17;random^=random<<5;right_bits=random;}
        uint64_t a=(uintptr_t)(items[0]+1),b=(uintptr_t)(items[1]+1);
        wr64(a,(uintptr_t)(objects[0]+1));wr64(b,(uintptr_t)(objects[1]+3));
        wr32(rd64(a)+0x200,left_bits);wr32(rd64(b)+0x200,right_bits);
        if(trial%5==1){wr32(a+0x18,1);wr32(b+0x18,0xffffffff);}
        if(trial%5==2){wr32(a+0x10,0xffffffff);wr32(b+0x10,0);}
        if(trial%5==3){wr32(a+0x20,0x80000000);wr32(b+0x20,0);}
        if(trial%5==4){wr32(a+0x10,random);wr32(b+0x10,random^0x80000000);}
        _mm_setcsr(control);int expected=original_float_compare((void *)(uintptr_t)a,(void *)(uintptr_t)b);
        unsigned original_status=_mm_getcsr();assert((original_status&~0x3f)==(control&~0x3f));
        if(original_status!=control)++original_status_differences;
        if(!state && !left_bits && right_bits==0x3fc00000)assert(expected==1 && original_status==0x1fa0);
        if(!state && !left_bits && right_bits==0x7f800000)assert(expected==-1 && original_status==0x1f81);
        if(!state && left_bits==0xff7fffff && right_bits==0x7f7fffff)assert(expected==-1 && original_status==0x1fa9);
        CPU c={0};c.rcx=a;c.rdx=b;c.rsp=(uintptr_t)(stack+4096);c.mxcsr=control;
        _mm_setcsr(control);float_entry(&c);assert((int32_t)c.rax==expected && c.mxcsr==control);
        _mm_setcsr(control);int actual=qsort_float_records((void *)(uintptr_t)a,(void *)(uintptr_t)b);
        assert(actual==expected && _mm_getcsr()==control);++float_pairs;
    }
    assert(original_status_differences);_mm_setcsr(saved_mxcsr);
    unsigned char float_objects[64][520],float_records[64][48],float_wanted[64][48];
    double float_elapsed[3]={0};unsigned float_cases=0;
    static const unsigned float_sizes[]={0,1,2,3,16,64};
    for(unsigned mode=0;mode<3;++mode)for(unsigned state=0;state<32;++state)for(unsigned trial=0;trial<6;++trial) {
        unsigned count=float_sizes[trial];
        unsigned control=0x1f80|((state&3)<<13)|((state&4)?0x40:0)|((state&8)?0x8000:0)|((state&16)?0x3f:0);
        for(unsigned i=0;i<count;++i) {
            memset(float_records[i],(int)i,48);wr64((uintptr_t)float_records[i],(uintptr_t)float_objects[i]);
            wr32((uintptr_t)float_objects[i]+0x200,float_bits[(i*13+trial+state)%32]);
            wr32((uintptr_t)float_records[i]+0x10,i%3==0?0:UINT32_MAX-i);
            wr32((uintptr_t)float_records[i]+0x18,i%3==1?0:i*UINT32_C(0x80000001));
            wr32((uintptr_t)float_records[i]+0x20,i%3==2?0:i);
        }
        memcpy(float_wanted,float_records,sizeof(float_records));
        _mm_setcsr(control);sort(float_wanted,count,48,original_float_compare);
        if(mode==1)qsort_float_target=0;else initialize_qsort_callbacks((uintptr_t)sort);
        LONG64 previous_callbacks=callback_calls;_mm_setcsr(control);SetLastError(0xe5ef3210);
        assert(QueryPerformanceCounter(&start));
        sort_bridge(sort,float_records,count,48,(uintptr_t)image+(mode==2?128:0),stack);
        assert(GetLastError()==0xe5ef3210 && _mm_getcsr()==control && QueryPerformanceCounter(&end));
        float_elapsed[mode]+=(double)(end.QuadPart-start.QuadPart)/frequency.QuadPart;
        assert(!memcmp(float_records,float_wanted,count*48));
        if(!mode)assert(callback_calls==previous_callbacks);else if(count>1)assert(callback_calls>previous_callbacks);
        ++float_cases;
    }
    for(unsigned i=0;i<64;++i)wr32((uintptr_t)float_objects[i]+0x200,0);
    memcpy(float_wanted,float_records,sizeof(float_records));_mm_setcsr(0);
    sort(float_wanted,16,48,original_float_compare);
    initialize_qsort_callbacks((uintptr_t)sort);LONG64 previous_callbacks=callback_calls;
    sort_bridge(sort,float_records,16,48,(uintptr_t)image,stack);
    assert(_mm_getcsr()==0 && callback_calls>previous_callbacks && !memcmp(float_records,float_wanted,16*48));
    for(unsigned exception=0;exception<6;++exception) {
        unsigned control=0x1f80&~(0x80u<<exception);
        memcpy(float_wanted,float_records,sizeof(float_records));_mm_setcsr(control);
        sort(float_wanted,16,48,original_float_compare);previous_callbacks=callback_calls;
        sort_bridge(sort,float_records,16,48,(uintptr_t)image,stack);
        assert(_mm_getcsr()==control && callback_calls>previous_callbacks && !memcmp(float_records,float_wanted,16*48));
    }
    _mm_setcsr(saved_mxcsr);memcpy(float_wanted,float_records,sizeof(float_records));
    sort(float_wanted,16,48,original_float_compare);_mm_setcsr(saved_mxcsr);
    image[0]^=1;initialize_qsort_callbacks((uintptr_t)sort);assert(!qsort_float_target);
    previous_callbacks=callback_calls;sort_bridge(sort,float_records,16,48,(uintptr_t)image,stack);
    assert(callback_calls>previous_callbacks && !memcmp(float_records,float_wanted,16*48));image[0]^=1;
    _mm_setcsr(saved_mxcsr);
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x14102f320);
    memcpy(image,qsort_u32_original_bytes,sizeof(qsort_u32_original_bytes));initialize_qsort_callbacks((uintptr_t)sort);
    assert(VirtualQuery(image,&memory,sizeof(memory)) && memory.Protect==PAGE_READWRITE);
    assert(RemoveVectoredExceptionHandler(handler));
    CPU failed={0};failed.rsp=(uintptr_t)(stack+4096);*(uint64_t *)(uintptr_t)failed.rsp=0xe5efcafe;
    failed.rcx=1;failed.rdx=2;failed.r8=8;failed.r9=(uintptr_t)image;
    CPU failed_before=failed;int caught=0;SetLastError(0xe5ef5678);
    __try {dispatch(&failed,(uintptr_t)sort);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {caught=1;}
    assert(caught && !memcmp(&failed,&failed_before,sizeof(failed)) && GetLastError()==0xe5ef5678);
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x141c561a0);
    memcpy(image,qsort_pointer_original_bytes,sizeof(qsort_pointer_original_bytes));
    initialize_qsort_callbacks((uintptr_t)sort);assert(qsort_pointer_target);
    uint64_t bad_pointees[2]={(uintptr_t)values,1};failed=failed_before;
    failed.rcx=(uintptr_t)bad_pointees;failed.r9=(uintptr_t)image;failed_before=failed;caught=0;
    SetLastError(0xe5ef9876);
    __try {dispatch(&failed,(uintptr_t)sort);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {caught=1;}
    assert(caught && !memcmp(&failed,&failed_before,sizeof(failed)) && GetLastError()==0xe5ef9876);
    g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x1410310d0);
    memcpy(image,qsort_float_original_bytes,sizeof(qsort_float_original_bytes));initialize_qsort_callbacks((uintptr_t)sort);
    memset(float_records,0,sizeof(float_records));wr64((uintptr_t)float_records[0],1);
    wr64((uintptr_t)float_records[1],(uintptr_t)float_objects[0]);failed=failed_before;
    failed.rcx=(uintptr_t)float_records;failed.r8=48;failed.r9=(uintptr_t)image;failed_before=failed;caught=0;
    SetLastError(0xe5ef4567);_mm_setcsr(saved_mxcsr);
    __try {dispatch(&failed,(uintptr_t)sort);}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {caught=1;}
    assert(caught && !memcmp(&failed,&failed_before,sizeof(failed)) && GetLastError()==0xe5ef4567 && _mm_getcsr()==saved_mxcsr);
    printf("{\"success\":true,\"table_static_full_CPU_LastError_nesting_cases\":2,\"cases\":%u,\"comparisons\":%u,\"direct_and_native_bridge\":true,\"actual_System32_qsort\":true,\"actual_VEH_NX_callback\":true,\"exact_AOT_unsigned_pointer_comparator\":true,\"highbit_duplicate_values\":true,\"CPU_nonvolatile_preserved\":true,\"LastError_preserved\":true,\"native_fault_CPU_LastError_preserved\":true,\"descending_pair_cases\":%u,\"descending_sort_cases\":%u,\"unknown_comparator_NX_fallback\":true,\"original_bytes_gate\":true,\"byte_mismatch_NX_fallback\":true,\"fastpath_NX_callbacks\":0,\"guest_image_original_bytes_executed\":false,\"isolated_oracle_original_bytes_executed\":true,\"fastpath_seconds\":%.9f,\"NX_baseline_seconds\":%.9f,\"unknown_NX_seconds\":%.9f,\"microbenchmark_speedup\":%.3f,\"pointer_pair_cases\":%u,\"pointer_full_identity_sort_cases\":%u,\"pointer_MXCSR_control_and_flags_states\":64,\"pointer_MXCSR_preserved\":true,\"pointer_byte_span_and_unknown_VA_gates\":true,\"pointer_fastpath_NX_callbacks\":0,\"pointer_native_fault_CPU_LastError_preserved\":true,\"pointer_fastpath_seconds\":%.9f,\"pointer_NX_seconds\":%.9f,\"pointer_microbenchmark_speedup\":%.3f",
        cases,comparisons,pair_cases,descending_cases,elapsed[0],elapsed[1],elapsed[2],elapsed[1]/elapsed[0],
        pointer_pairs,cases,pointer_elapsed[2],pointer_elapsed[1],pointer_elapsed[1]/pointer_elapsed[2]);
    printf(",\"float_pair_cases\":%u,\"float_full_record_sort_cases\":%u,\"float_MXCSR_control_and_flags_states\":32,\"float_original_status_differences\":%u,\"float_original_and_lifted_return_values_exact\":true,\"float_current_NX_MXCSR_boundary_preserved\":true,\"float_unmasked_NX_fallback\":true,\"float_byte_span_and_unknown_VA_gates\":true,\"float_fastpath_NX_callbacks\":0,\"float_native_fault_CPU_LastError_MXCSR_preserved\":true,\"float_fastpath_seconds\":%.9f,\"float_NX_seconds\":%.9f,\"float_microbenchmark_speedup\":%.3f}\n",
        float_pairs,float_cases,original_status_differences,float_elapsed[0],float_elapsed[1],float_elapsed[1]/float_elapsed[0]);
    VirtualFree(oracle,0,MEM_RELEASE);
    VirtualFree(stack,0,MEM_RELEASE);VirtualFree(image,0,MEM_RELEASE);
    FreeLibrary(implementation);FreeLibrary(crt);return 0;
}
