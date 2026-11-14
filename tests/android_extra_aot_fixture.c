#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "extra_aot.h"
typedef struct {uint64_t va,target;size_t size;const char *bytes;} test_case;
#include "windows_extra_aot_cases.h"
#include "windows_switch_aot_cases.h"
#include "windows_switch_extension_cases.h"
#include "android_mobile_callbacks.h"
#include "android_observed_callbacks.h"
int64_t g_image_delta;
static uint64_t tail;static unsigned calls;
void dispatch(CPU *cpu,uint64_t target){(void)cpu;tail=target;++calls;}
void dispatch_jmp(CPU *cpu,uint64_t target){dispatch(cpu,target);}
void frostpunk_unsupported(uint64_t va,const char *text){fprintf(stderr,"Unexpected trap %" PRIx64 " %s\n",va,text);abort();}
static void w16(unsigned char *p,uint16_t v){memcpy(p,&v,2);}
static void w32(unsigned char *p,uint32_t v){memcpy(p,&v,4);}
static void w64(unsigned char *p,uint64_t v){memcpy(p,&v,8);}
static unsigned lea_tail_original_oracle(const test_case *entry){
#ifdef _WIN32
    unsigned checked=0;
    for(unsigned relocation=0;relocation<2;++relocation){
        uintptr_t base=UINT64_C(0x140000000)+(uintptr_t)relocation*UINT64_C(0x10000000);
        void *image=VirtualAlloc((void*)base,0x4539000,MEM_RESERVE,PAGE_NOACCESS);assert((uintptr_t)image==base);
        uintptr_t source=base+entry->va-UINT64_C(0x140000000),target=base+entry->target-UINT64_C(0x140000000);
        assert(VirtualAlloc((void*)(source&~(uintptr_t)4095),4096,MEM_COMMIT,PAGE_READWRITE));
        assert(VirtualAlloc((void*)(target&~(uintptr_t)4095),4096,MEM_COMMIT,PAGE_READWRITE));
        memcpy((void*)source,entry->bytes,entry->size);
        // Only the already-translated tail is replaced by a capture. The callback's
        // original 12 bytes execute unchanged, including both RIP-relative offsets.
        memcpy((void*)target,"\x48\x8b\xc1\xc3",4);DWORD old;
        assert(VirtualProtect((void*)(source&~(uintptr_t)4095),4096,PAGE_EXECUTE_READ,&old));
        assert(VirtualProtect((void*)(target&~(uintptr_t)4095),4096,PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),image,0x4539000));
        for(unsigned i=0;i<16;++i){
            uint64_t result=((uint64_t(__fastcall*)(uint64_t))source)(UINT64_C(0x9876543210000000)+i);
            int32_t relative;memcpy(&relative,entry->bytes+3,4);
            assert(result==source+7+(int64_t)relative);++checked;
        }
        assert(VirtualFree(image,0,MEM_RELEASE));
    }
    return checked;
#else
    (void)entry;return 0;
#endif
}
static unsigned cleanup_original_oracle(const test_case *entry){
#ifdef _WIN32
    unsigned checked=0;
    for(unsigned relocation=0;relocation<2;++relocation){
        uintptr_t base=UINT64_C(0x140000000)+(uintptr_t)relocation*UINT64_C(0x10000000);
        void *image=VirtualAlloc((void*)base,0x4539000,MEM_RESERVE,PAGE_NOACCESS);assert((uintptr_t)image==base);
        uintptr_t source=base+entry->va-UINT64_C(0x140000000),target=base+UINT64_C(0xe26bf0);
        uintptr_t slot=base+UINT64_C(0x1cc72e8),critical=base+UINT64_C(0x296ff60);
        uintptr_t pages[]={source,target,slot,critical};
        for(unsigned i=0;i<4;++i)assert(VirtualAlloc((void*)(pages[i]&~(uintptr_t)4095),4096,MEM_COMMIT,PAGE_READWRITE));
        memcpy((void*)source,entry->bytes,entry->size);memcpy((void*)target,"\x48\xff\x25\xf1\x06\xea\x00",7);
        w64((unsigned char*)slot,(uintptr_t)DeleteCriticalSection);DWORD old;
        assert(VirtualProtect((void*)(source&~(uintptr_t)4095),4096,PAGE_EXECUTE_READ,&old));
        assert(VirtualProtect((void*)(target&~(uintptr_t)4095),4096,PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),image,0x4539000));
        for(unsigned i=0;i<16;++i){
            CRITICAL_SECTION *section=(CRITICAL_SECTION*)critical;
            InitializeCriticalSection(section);EnterCriticalSection(section);LeaveCriticalSection(section);
            // Both callback and import thunk execute unchanged; the original
            // imported Windows function performs the real resource cleanup.
            ((void(__fastcall*)(void))source)();
            InitializeCriticalSection(section);EnterCriticalSection(section);LeaveCriticalSection(section);DeleteCriticalSection(section);
            ++checked;
        }
        assert(VirtualFree(image,0,MEM_RELEASE));
    }
    return checked;
#else
    (void)entry;return 0;
#endif
}
static unsigned array_tail_original_oracle(const test_case *entry){
#ifdef _WIN32
    unsigned checked=0;assert(entry->size==28);
    for(unsigned relocation=0;relocation<2;++relocation){
        uintptr_t base=UINT64_C(0x140000000)+(uintptr_t)relocation*UINT64_C(0x10000000);
        void *image=VirtualAlloc((void*)base,0x4539000,MEM_RESERVE,PAGE_NOACCESS);assert((uintptr_t)image==base);
        uintptr_t source=base+entry->va-UINT64_C(0x140000000),target=base+entry->target-UINT64_C(0x140000000);
        uintptr_t wrapper=(source&~(uintptr_t)4095)+0x800;
        assert(VirtualAlloc((void*)(source&~(uintptr_t)4095),4096,MEM_COMMIT,PAGE_READWRITE));
        assert(VirtualAlloc((void*)(target&~(uintptr_t)4095),4096,MEM_COMMIT,PAGE_READWRITE));
        memcpy((void*)source,entry->bytes,entry->size);
        memcpy((void*)target,"\x49\x89\x0a\x49\x89\x52\x08\x4d\x89\x42\x10\x4d\x89\x4a\x18\xc3",16);
        unsigned char call[]={0x4c,0x8b,0xd1,0x48,0xb8,0,0,0,0,0,0,0,0,0x48,0x83,0xec,0x28,0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3};
        memcpy(call+5,&source,8);memcpy((void*)wrapper,call,sizeof(call));DWORD old;
        assert(VirtualProtect((void*)(source&~(uintptr_t)4095),4096,PAGE_EXECUTE_READ,&old));
        assert(VirtualProtect((void*)(target&~(uintptr_t)4095),4096,PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),image,0x4539000));
        int32_t element,object;memcpy(&element,entry->bytes+8,4);memcpy(&object,entry->bytes+15,4);
        for(unsigned i=0;i<16;++i){
            uint64_t captured[4]={0};((void(__fastcall*)(void*))wrapper)(captured);
            assert(captured[0]==source+19+(int64_t)object&&captured[1]==8&&captured[2]==2&&captured[3]==source+12+(int64_t)element);++checked;
        }
        assert(VirtualFree(image,0,MEM_RELEASE));
    }
    return checked;
#else
    (void)entry;return 0;
#endif
}
int main(void) {
    const uint64_t preferred=UINT64_C(0x140000000);const size_t size=0x4539000;
    unsigned char *image=calloc(1,size);assert(image);w16(image,0x5a4d);w32(image+0x3c,0x80);
    memcpy(image+0x80,"PE\0\0",4);w16(image+0x84,0x8664);w16(image+0x86,1);w16(image+0x94,0xf0);
    w16(image+0x98,0x20b);w64(image+0xb0,preferred);w32(image+0xd0,(uint32_t)size);
    w32(image+0x188+8,(uint32_t)size);w32(image+0x188+36,0x20000000);
    test_case all[36+sizeof(mobile_cases)/sizeof(*mobile_cases)];unsigned count=0;
#define ADD_GROUP(a) do{for(unsigned i=0;i<sizeof(a)/sizeof(*(a));++i)all[count++]=(a)[i];}while(0)
    ADD_GROUP(cases);ADD_GROUP(switch_cases);ADD_GROUP(switch_extension_cases);ADD_GROUP(callback_cases);
    ADD_GROUP(scene_cases);ADD_GROUP(compare_cases);ADD_GROUP(bounded_cases);
    all[count++]=(test_case){UINT64_C(0x1413c2038),0,9,"\x48\x8b\x01\xff\xa0\x08\x04\x00\x00"};assert(count==36);
    ADD_GROUP(mobile_cases);
    for(unsigned i=0;i<count;++i)memcpy(image+all[i].va-preferred,all[i].bytes,all[i].size);
    assert(fp_android_extra_aot_init(image,size));unsigned gates=0,boundaries=0;
    for(unsigned i=0;i<count;++i) {
        assert(fp_android_extra_aot_lookup(all[i].va));++boundaries;
        for(size_t n=0;n<all[i].size;++n) {
            unsigned char *byte=image+all[i].va-preferred+n;*byte^=1;
            assert(!fp_android_extra_aot_init(image,size));assert(!fp_android_extra_aot_lookup(all[i].va));
            *byte^=1;++gates;
        }
        assert(fp_android_extra_aot_init(image,size));
    }
    unsigned expected_gates=2065;
    for(unsigned i=0;i<sizeof(mobile_cases)/sizeof(*mobile_cases);++i)expected_gates+=(unsigned)mobile_cases[i].size;
    assert(gates==expected_gates);w32(image+0x188+36,0);assert(!fp_android_extra_aot_init(image,size));
    w32(image+0x188+36,0x20000000);w64(image+0xb0,preferred+1);assert(!fp_android_extra_aot_init(image,size));
    w64(image+0xb0,preferred);assert(!fp_android_extra_aot_init(image,63));
    assert(fp_android_extra_aot_init(image,size));
    void(*add)(CPU*)=fp_android_extra_aot_lookup(UINT64_C(0x140fee310));assert(add);
    uint64_t random=UINT64_C(0x0123456789abcdef);unsigned cpu_cases=0;
    for(unsigned i=0;i<8192;++i) {
        random^=random<<13;random^=random>>7;random^=random<<17;
        uint64_t a=i<64?UINT64_MAX-i:random,result=a+24;CPU cpu,wanted;memset(&cpu,0xa5,sizeof(cpu));
        cpu.rcx=a;wanted=cpu;wanted.rip=UINT64_C(0x140fee310);wanted.rcx=result;
        wanted.cf=result<a;wanted.of=((~(a^UINT64_C(24))&(a^result))>>63)&1;
        wanted.af=((a^UINT64_C(24)^result)>>4)&1;wanted.sf=result>>63;wanted.zf=result==0;
        unsigned parity=(unsigned)(result&255);parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;wanted.pf=(~parity)&1;
        calls=0;add(&cpu);assert(calls==1 && tail==UINT64_C(0x140e52950) && !memcmp(&cpu,&wanted,sizeof(cpu)));++cpu_cases;
    }
    void(*callback)(CPU*)=fp_android_extra_aot_lookup(UINT64_C(0x141947e84));assert(callback);
    for(unsigned i=0;i<8192;++i) {
        uint64_t vtable[34]={0},object=(uintptr_t)vtable;vtable[33]=UINT64_C(0x1234567800000000)+i;
        CPU cpu,wanted;memset(&cpu,0x5a,sizeof(cpu));cpu.rcx=(uintptr_t)&object;wanted=cpu;
        wanted.rip=UINT64_C(0x141947e87);wanted.rax=(uintptr_t)vtable;calls=0;
        callback(&cpu);assert(calls==1 && tail==vtable[33] && !memcmp(&cpu,&wanted,sizeof(cpu)));++cpu_cases;
    }
    callback=fp_android_extra_aot_lookup(UINT64_C(0x1413c2038));assert(callback);
    for(unsigned i=0;i<8192;++i){
        uint64_t vtable[130]={0},object=(uintptr_t)vtable;vtable[129]=UINT64_C(0x9876543200000000)+i;
        CPU cpu,wanted;memset(&cpu,0x5a,sizeof(cpu));cpu.rcx=(uintptr_t)&object;wanted=cpu;
        wanted.rip=UINT64_C(0x1413c203b);wanted.rax=(uintptr_t)vtable;calls=0;
        callback(&cpu);assert(calls==1 && tail==vtable[129] && !memcmp(&cpu,&wanted,sizeof(cpu)));++cpu_cases;
    }
    callback=fp_android_extra_aot_lookup(UINT64_C(0x1419c8310));assert(callback);
    for(unsigned i=0;i<8192;++i){
        CPU cpu,wanted;unsigned char *bytes=(unsigned char*)&cpu;
        for(unsigned n=0;n<sizeof(cpu);++n){random^=random<<13;random^=random>>7;random^=random<<17;bytes[n]=(unsigned char)random;}
        g_image_delta=(i&1)?INT64_C(0x10000000):0;wanted=cpu;
        wanted.rcx=GVA(UINT64_C(0x142981fe0));wanted.rip=UINT64_C(0x1419c8310);calls=0;
        callback(&cpu);assert(calls==1&&tail==UINT64_C(0x140e74180)&&!memcmp(&cpu,&wanted,sizeof(cpu)));++cpu_cases;
    }
    g_image_delta=0;unsigned quit_oracle_cases=0;
    for(unsigned i=0;i<sizeof(observed_callback_cases)/sizeof(*observed_callback_cases);++i)
        if(observed_callback_cases[i].va==UINT64_C(0x1419c8310))quit_oracle_cases=lea_tail_original_oracle(&observed_callback_cases[i]);
    callback=fp_android_extra_aot_lookup(UINT64_C(0x141ca4040));assert(callback);
    for(unsigned i=0;i<8192;++i){
        CPU cpu,wanted;unsigned char *bytes=(unsigned char*)&cpu;
        for(unsigned n=0;n<sizeof(cpu);++n){random^=random<<13;random^=random>>7;random^=random<<17;bytes[n]=(unsigned char)random;}
        g_image_delta=(i&1)?INT64_C(0x10000000):0;wanted=cpu;
        wanted.rcx=GVA(UINT64_C(0x14296ff60));wanted.rip=UINT64_C(0x141ca4040);calls=0;
        callback(&cpu);assert(calls==1&&tail==UINT64_C(0x140e26bf0)&&!memcmp(&cpu,&wanted,sizeof(cpu)));++cpu_cases;
    }
    g_image_delta=0;unsigned cleanup_oracle_cases=0;
    for(unsigned i=0;i<count;++i)if(all[i].va==UINT64_C(0x141ca4040))cleanup_oracle_cases=cleanup_original_oracle(&all[i]);
    unsigned observed_cpu_cases=2*8192,observed_oracle_cases=quit_oracle_cases;
    for(unsigned entry=0;entry<sizeof(observed_callback_cases)/sizeof(*observed_callback_cases);++entry){
        const test_case *test=&observed_callback_cases[entry];
        if(test->va!=UINT64_C(0x1419c8310))observed_oracle_cases+=test->size==12?lea_tail_original_oracle(test):array_tail_original_oracle(test);
        if(test->va==UINT64_C(0x1419c8310)||test->va==UINT64_C(0x141ca4040))continue;
        callback=fp_android_extra_aot_lookup(test->va);assert(callback&&(test->size==12||test->size==28));
        int32_t relative;memcpy(&relative,test->bytes+3,4);
        for(unsigned i=0;i<8192;++i){
            CPU cpu,wanted;unsigned char *bytes=(unsigned char*)&cpu;
            for(unsigned n=0;n<sizeof(cpu);++n){random^=random<<13;random^=random>>7;random^=random<<17;bytes[n]=(unsigned char)random;}
            g_image_delta=(i&1)?INT64_C(0x10000000):0;wanted=cpu;
            wanted.rcx=GVA(test->va+7+(int64_t)relative);
            if(test->size==28){
                int32_t object,element;memcpy(&object,test->bytes+15,4);memcpy(&element,test->bytes+8,4);
                wanted.rcx=GVA(test->va+19+(int64_t)object);wanted.rdx=8;wanted.r8=2;wanted.r9=GVA(test->va+12+(int64_t)element);
            }
            wanted.rip=test->va;calls=0;
            callback(&cpu);assert(calls==1&&tail==test->target&&!memcmp(&cpu,&wanted,sizeof(cpu)));++cpu_cases;++observed_cpu_cases;
        }
    }
    g_image_delta=0;
    callback=fp_android_extra_aot_lookup(UINT64_C(0x141a601c0));assert(callback);
    unsigned char owner[0x1c0]={0},items[3*128]={0};
    uint64_t wanted_id=7,other_id=9,request[2]={0,(uintptr_t)&wanted_id};
    w64(owner+0x1b0,(uintptr_t)items);
    for(unsigned length=0;length<=3;++length)for(int match=-1;match<(int)length;++match){
        w32(owner+0x1b8,length);
        for(unsigned i=0;i<length;++i)w64(items+i*128+0x10,(uintptr_t)((int)i==match?&wanted_id:&other_id));
        CPU cpu={.rcx=(uintptr_t)owner,.rdx=(uintptr_t)request,.rsp=0x12345000};calls=0;
        callback(&cpu);
        assert(calls==(unsigned)(match>=0));
        if(match>=0)assert(tail==UINT64_C(0x141a608c0)&&cpu.rcx==(uintptr_t)owner&&cpu.rdx==(uintptr_t)(items+match*128));
        else assert(cpu.rsp==0x12345008);
        ++cpu_cases;
    }
    free(image);printf("{\"success\":true,\"entries\":%u,\"opcode_mutations\":%u,\"lookup_boundaries\":%u,\"full_CPU_cases\":%u,\"law_callback_cases\":10,\"quit_callback_CPU_cases\":8192,\"quit_callback_original_byte_oracles\":%u,\"cleanup_callback_CPU_cases\":8192,\"cleanup_callback_original_byte_oracles\":%u,\"observed_callback_CPU_cases\":%u,\"observed_callback_original_byte_oracles\":%u,\"original_bytes_executed\":false}\n",count,gates,boundaries,cpu_cases,quit_oracle_cases,cleanup_oracle_cases,observed_cpu_cases,observed_oracle_cases);return 0;
}
