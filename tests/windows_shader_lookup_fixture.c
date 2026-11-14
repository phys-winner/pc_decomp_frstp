#define _CRT_SECURE_NO_WARNINGS
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#endif
#include "cpu64.h"
#include "shader_lookup.h"
#include "windows_shader_lookup_baseline.inc"

int64_t g_image_delta;
static char default_empty[1];
static uint32_t state=0x72a94317u;
static uint32_t random32(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
typedef struct {
    uint64_t family[16],records[18*144],alternate[18*144],appended[144];
    unsigned char key[8192];
    uint64_t outputs[2],stack[1024];
} Arena;
typedef struct {uint64_t target;CPU cpu;} Event;
typedef struct {CPU cpu;Event event[4];unsigned events;int native_errno;uint32_t last_error,guest_errno,native_LastError;} Outcome;
static Arena arena,before,expected_memory;
static Event events[4];static unsigned event_count,mutation,record_trace=1;
static uint32_t last_error,guest_errno;static uint64_t cases,guard_cases;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"check line %d case %" PRIu64 ": %s\n",__LINE__,cases,#x);exit(1);}}while(0)
static uint64_t address(void *p){return (uint64_t)(uintptr_t)p;}

void dispatch(CPU *c,uint64_t target)
{
    if(record_trace){CHECK(event_count<4);events[event_count].target=target;events[event_count++].cpu=*c;}
    if(target==0x141058000ull){
        CHECK(c->rcx==address(arena.family));
        if(mutation&1)wr64(address(arena.family)+0x60,address(arena.alternate));
        wr32(address(arena.family)+0x68,rd32(address(arena.family)+0x68)+1u);
        wr64(address(arena.appended),0x8765432100012345ull);
        c->rax=address(arena.appended);c->rcx=0x11110000;c->rdx=0x22223333;c->r8=0x44445555;
        c->r9=0x66667777;c->r10=0x88889999;c->r11=0xaaaabbbb;
        c->cf=1;c->of=0;c->sf=1;c->zf=0;c->pf=0;c->af=1;
        c->rsp+=8;errno=2345;last_error=1234;guest_errno=3456;return;
    }
    if(target==0x141059750ull || target==0x141059890ull){
        if(mutation&2)wr64(address(arena.family)+0x60,address(arena.alternate));
        if(target==0x141059750ull)baseline_L_000141059750(c);else baseline_L_000141059890(c);
        return;
    }
    if(target==0x141059732ull){baseline_L_000141059732(c);return;}
    if(target==0x141059872ull){baseline_L_000141059872(c);return;}
    fprintf(stderr,"unexpected dispatch %" PRIx64 "\n",target);exit(2);
}
void dispatch_jmp(CPU*c,uint64_t target){dispatch(c,target);}

static CPU initialize(int32_t count,uint64_t index,int optional,uint32_t flag_seed)
{
    memset(&arena,0,sizeof(arena));CPU c;
    for(size_t n=0;n<sizeof(c);++n)((unsigned char*)&c)[n]=(unsigned char)random32();
    for(size_t n=0;n<sizeof(arena.stack)/sizeof(arena.stack[0]);++n)arena.stack[n]=((uint64_t)random32()<<32)|random32();
    c.cf=flag_seed&1u;c.zf=(flag_seed>>1)&1u;c.sf=(flag_seed>>2)&1u;
    c.of=(flag_seed>>3)&1u;c.pf=(flag_seed>>4)&1u;c.af=(flag_seed>>5)&1u;
    c.rsi=address(arena.family);c.r14=address(&arena.outputs[0]);c.rbx=optional?address(&arena.outputs[1]):0;
    c.r15=address(arena.key);c.rsp=address(&arena.stack[128]);
    c.r11=0xa5a5a5a500000000ull|(uint32_t)count;c.r9=index;
    wr64(address(arena.family)+0x60,address(arena.records));wr32(address(arena.family)+0x68,(uint32_t)count);
    return c;
}
static void invoke(CPU*c,unsigned flavor,unsigned optimized)
{
    if(optimized){if(flavor)frostpunk_shader_lookup_1410597e9(c);else frostpunk_shader_lookup_1410596a9(c);}
    else {if(flavor)baseline_L_0001410597E9(c);else baseline_L_0001410596A9(c);}
}
static Outcome outcome(CPU c,unsigned flavor,unsigned optimized)
{
    memset(events,0,sizeof(events));event_count=0;errno=1927;last_error=0x5a17;guest_errno=31337;
    SetLastError(0xa5b61327u);invoke(&c,flavor,optimized);
    Outcome result;memset(&result,0,sizeof(result));result.cpu=c;
    result.events=event_count;memcpy(result.event,events,sizeof(events));
    result.native_LastError=GetLastError();result.native_errno=errno;result.last_error=last_error;result.guest_errno=guest_errno;
    return result;
}
static void one(CPU c,unsigned flavor,int found,uint32_t found_index)
{
    ++cases;before=arena;Outcome reference=outcome(c,flavor,0);expected_memory=arena;
    CHECK(reference.events>=1);CHECK(reference.native_LastError==0xa5b61327u);
    CHECK(reference.event[0].target==(found?(flavor?0x141059890ull:0x141059750ull):0x141058000ull));
    if(found)CHECK((uint32_t)reference.event[0].cpu.r9==found_index);
    if(c.r14==address(&arena.outputs[0])){
        uint64_t base=found?rd64(address(before.family)+0x60)+(uint64_t)(int64_t)(int32_t)found_index*1152u:address(arena.appended);
        CHECK(arena.outputs[0]==base+(flavor?0x280u:0x200u));
        if(c.rbx==address(&arena.outputs[1])){
            uint64_t second=found?((mutation&2)?address(arena.alternate):rd64(address(before.family)+0x60))+(uint64_t)(int64_t)(int32_t)found_index*1152u:address(arena.appended);
            CHECK(arena.outputs[1]==second+(flavor?0x300u:0x400u));
        }
    }
    arena=before;Outcome candidate=outcome(c,flavor,1);
    if(memcmp(&reference,&candidate,sizeof(reference))){
        const unsigned char*a=(const unsigned char*)&reference,*b=(const unsigned char*)&candidate;
        for(size_t n=0;n<sizeof(reference);++n)if(a[n]!=b[n]){fprintf(stderr,"outcome byte %zu old %u new %u\n",n,a[n],b[n]);break;}
        CHECK(0);
    }
    CHECK(!memcmp(&arena,&expected_memory,sizeof(arena)));
}
static void semantic(void)
{
    for(unsigned flavor=0;flavor<2;++flavor){
        for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b){
            CPU c=initialize(1,0xa5a5a5a500000000ull,1,(a^b)&63u);
            ((unsigned char*)arena.records)[0]=(unsigned char)a;arena.key[0]=(unsigned char)b;
            mutation=(a+b)&3u;one(c,flavor,a==b,0);
        }
        for(unsigned flags=0;flags<64;++flags)for(unsigned optional=0;optional<2;++optional){
            for(int32_t count=-2;count<=16;++count){
                CPU c=initialize(count,0,optional,flags);mutation=flags&3u;
                for(int32_t n=0;n<count;++n){memset((unsigned char*)arena.records+(size_t)n*1152,'q',127);((unsigned char*)arena.records)[(size_t)n*1152+127]=(unsigned char)('A'+n);}
                memset(arena.key,'q',127);arena.key[127]=(unsigned char)('A'+(count>0?count-1:0));
                one(c,flavor,count>0,count>0?(uint32_t)(count-1):0);
            }
            CPU c=initialize(3,0xa5a5a5a500000000ull,optional,flags);mutation=0;
            strcpy((char*)arena.records,"same");strcpy((char*)arena.records+1152,"same");strcpy((char*)arena.records+2304,"same");strcpy((char*)arena.key,"same");
            one(c,flavor,1,0);
            c=initialize(3,0x5a5a5a5a00000002ull,optional,flags);strcpy((char*)arena.key,"tail");strcpy((char*)arena.records+2304,"tail");one(c,flavor,1,2);
            c=initialize(1,0xa5a5a5a5ffffffffull,optional,flags);wr64(address(arena.family)+0x60,address(arena.records)+1152u);one(c,flavor,1,UINT32_MAX);
            c=initialize(1,0,optional,flags);c.r15=0;one(c,flavor,1,0);
        }
        const size_t lengths[]={0,1,2,15,16,31,63,127,255,511,1023,4095};
        for(size_t i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i){
            size_t length=lengths[i];CPU c=initialize(1,0,1,(uint32_t)i);mutation=0;
            memset(arena.records,0x81,length);memset(arena.key,0x81,length);one(c,flavor,1,0);
            c=initialize(1,0,1,(uint32_t)i);memset(arena.records,'a',length);memset(arena.key,'a',length);arena.key[length]='b';one(c,flavor,0,0);
        }
        /* A mutation between calls must be visible; there is no output/key cache. */
        CPU c=initialize(2,0,1,0);mutation=0;strcpy((char*)arena.records,"A");strcpy((char*)arena.key,"A");one(c,flavor,1,0);
        ((char*)arena.records)[0]='B';one(c,flavor,0,0);
        c=initialize(1,0,1,0);c.r14=address(arena.records);c.rbx=address(arena.records);one(c,flavor,1,0);
    }
}

typedef struct {unsigned char*base,*middle;size_t page;} Guard;
static Guard allocate_guard(void)
{
    Guard g;
#ifdef _WIN32
    SYSTEM_INFO s;GetSystemInfo(&s);g.page=s.dwPageSize;
    g.base=VirtualAlloc(NULL,g.page*3,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS);CHECK(g.base);
    DWORD old;g.middle=g.base+g.page;CHECK(VirtualProtect(g.middle,g.page,PAGE_READWRITE,&old));
#else
    g.page=(size_t)sysconf(_SC_PAGESIZE);g.base=mmap(NULL,g.page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(g.base!=MAP_FAILED);
    g.middle=g.base+g.page;CHECK(mprotect(g.middle,g.page,PROT_READ|PROT_WRITE)==0);
#endif
    return g;
}
static void guard_permission(Guard g,int writable)
{
#ifdef _WIN32
    DWORD old;CHECK(VirtualProtect(g.middle,g.page,writable?PAGE_READWRITE:PAGE_READONLY,&old));
#else
    CHECK(mprotect(g.middle,g.page,writable?PROT_READ|PROT_WRITE:PROT_READ)==0);
#endif
}
static void free_guard(Guard g)
{
#ifdef _WIN32
    CHECK(VirtualFree(g.base,0,MEM_RELEASE));
#else
    CHECK(munmap(g.base,g.page*3)==0);
#endif
}
static void guards(void)
{
    Guard record=allocate_guard(),key=allocate_guard();
    const size_t lengths[]={0,1,15,31,127,511};
    for(unsigned flavor=0;flavor<2;++flavor)for(unsigned mode=0;mode<3;++mode)for(size_t i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i){
        size_t n=lengths[i];guard_permission(record,1);guard_permission(key,1);
        unsigned char*r=record.middle+record.page-n-1,*k=key.middle+key.page-n-1;
        memset(r,0x91,n);memset(k,0x91,n);r[n]=k[n]=0;
        if(mode==1){r[n]=0x80;k[n]=0x81;} /* mismatch at the last readable byte */
        if(mode==2 && n){r[0]=0x80;k[0]=0x81;}
        guard_permission(record,0);guard_permission(key,0);
        CPU c=initialize(1,0,1,(uint32_t)i);mutation=0;
        wr64(address(arena.family)+0x60,address(r));c.r15=address(k);
        one(c,flavor,mode==0 || (mode==2 && n==0),0);++guard_cases;
    }
    free_guard(record);free_guard(key);
}
static uint64_t nanoseconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER now,frequency;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    return(uint64_t)((long double)now.QuadPart*1000000000.0L/frequency.QuadPart);
#else
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
#endif
}
static void benchmark(void)
{
    const unsigned counts[]={1024,4096,8192};
    for(unsigned group=0;group<3;++group){
        unsigned count=counts[group];unsigned char*records=calloc(count,1152);CHECK(records);
        CPU initial=initialize((int32_t)count,0,1,0);mutation=0;record_trace=0;
        wr64(address(arena.family)+0x60,address(records));
        for(unsigned n=0;n<count;++n){memset(records+(size_t)n*1152,'x',64);sprintf((char*)records+(size_t)n*1152+64,"%08x",n);}
        memcpy(arena.key,records+(size_t)(count-1)*1152,73);
        uint64_t times[2];
        for(unsigned optimized=0;optimized<2;++optimized){
            uint64_t start=nanoseconds();
            for(unsigned call=0;call<32;++call){CPU c=initial;invoke(&c,0,optimized);CHECK(arena.outputs[0]==address(records)+(uint64_t)(count-1)*1152u+0x200u);}
            times[optimized]=nanoseconds()-start;
        }
        printf("{\"benchmark_records\":%u,\"queries_each\":32,\"before_ns\":%" PRIu64 ",\"candidate_ns\":%" PRIu64 "}\n",count,times[0],times[1]);
        free(records);record_trace=1;
    }
}
int retained_v1_main(void)
{
    g_image_delta=(int64_t)address(default_empty)-(int64_t)0x1421ECEBAull;
    semantic();guards();benchmark();
    printf("{\"success\":true,\"differential_cases\":%" PRIu64 ",\"guard_page_readonly_cases\":%" PRIu64 ",\"whole_CPU_stack_memory_errors\":true,\"exact_original_boundary_traces\":true,\"all_byte_pairs\":true,\"no_cache_mutation_cases\":true,\"game_executed\":false}\n",cases,guard_cases);
    return 0;
}

static void nonfinal_pairs(void)
{
    for(unsigned flavor=0;flavor<2;++flavor)for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b){
        CPU c=initialize(2,0xa5a5a5a500000000ull,1,(a^b)&63u);
        ((unsigned char*)arena.records)[0]=(unsigned char)a;
        ((unsigned char*)arena.records)[1152]=(unsigned char)a;arena.key[0]=(unsigned char)b;
        mutation=(a+b)&3u;one(c,flavor,a==b,0);
    }
}
static void nonfinal_guards(void)
{
    Guard record=allocate_guard(),key=allocate_guard();
    const size_t lengths[]={0,1,15,31,127,511};
    for(unsigned flavor=0;flavor<2;++flavor){
        for(size_t i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i){
            size_t n=lengths[i];guard_permission(record,1);guard_permission(key,1);
            unsigned char*r=record.middle+record.page-n-1,*k=key.middle+key.page-n-1;
            memset(r,0x91,n);memset(k,0x91,n);r[n]=k[n]=0;
            guard_permission(record,0);guard_permission(key,0);
            CPU c=initialize(2,0xa5a5a5a500000000ull,1,(uint32_t)i);mutation=0;
            wr64(address(arena.family)+0x60,address(r));c.r15=address(k);
            
            one(c,flavor,1,0);++guard_cases;
        }
        /* The key has no readable terminator; both record candidates differ
         * at the last readable key byte, so the original never reads farther.
         * This catches a native comparison that crosses the guard on mismatch. */
        for(unsigned value=0;value<256;++value){
            guard_permission(key,1);unsigned char*k=key.middle+key.page-1;*k=(unsigned char)value;guard_permission(key,0);
            CPU c=initialize(2,0,1,value&63u);mutation=0;
            ((unsigned char*)arena.records)[0]=(unsigned char)(value^1u);
            ((unsigned char*)arena.records)[1152]=(unsigned char)(value^1u);c.r15=address(k);
            
            one(c,flavor,0,0);++guard_cases;
        }
    }
    free_guard(record);free_guard(key);
}
#include "windows_shader_lookup_gates.h"
static void byte_gates(void)
{
    const size_t bytes=0x1060000;
    void *memory=VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(memory);
    fp_section section={0,(uint32_t)bytes,0x20000000u};
    fp_image image={0};image.base=memory;image.size=bytes;image.preferred_base=UINT64_C(0x140000000);
    image.sections=&section;image.section_count=1;
    const size_t offsets[]={0x10596a9,0x10597e9};
    const unsigned char *expected[]={expected_gate_0,expected_gate_1};
    for(unsigned n=0;n<2;++n)memcpy((unsigned char*)memory+offsets[n],expected[n],167);
    CHECK(fp_shader_lookup_init(&image));
    CHECK(fp_shader_lookup(UINT64_C(0x1410596a9))==frostpunk_shader_lookup_1410596a9);
    CHECK(fp_shader_lookup(UINT64_C(0x1410597e9))==frostpunk_shader_lookup_1410597e9);
    CHECK(!fp_shader_lookup(UINT64_C(0x1410596aa)));
    for(unsigned n=0;n<2;++n)for(unsigned i=0;i<167;++i){
        unsigned char *p=(unsigned char*)memory+offsets[n]+i;*p^=1;
        CHECK(!fp_shader_lookup_init(&image));
        CHECK(!fp_shader_lookup(UINT64_C(0x1410596a9)) && !fp_shader_lookup(UINT64_C(0x1410597e9)));
        *p^=1;CHECK(fp_shader_lookup_init(&image));
    }
    image.preferred_base++;CHECK(!fp_shader_lookup_init(&image));image.preferred_base--;
    section.characteristics=0;CHECK(!fp_shader_lookup_init(&image));section.characteristics=0x20000000u;
    image.size=1;CHECK(!fp_shader_lookup_init(&image));image.size=bytes;
    CHECK(fp_shader_lookup_init(&image));
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    g_image_delta=(int64_t)address(default_empty)-(int64_t)0x1421ECEBAull;
    byte_gates();
    fprintf(stderr,"phase old semantic\n");semantic();
    fprintf(stderr,"phase old guards\n");guards();
    fprintf(stderr,"phase benchmark\n");benchmark();
    fprintf(stderr,"phase nonfinal pairs\n");nonfinal_pairs();
    fprintf(stderr,"phase nonfinal guards\n");nonfinal_guards();
    printf("{\"success\":true,\"differential_cases\":%" PRIu64 ",\"guard_page_readonly_cases\":%" PRIu64 ",\"whole_CPU_stack_memory_errors\":true,\"exact_original_boundary_traces\":true,\"all_byte_pairs\":true,\"no_cache_mutation_cases\":true,\"game_executed\":false}\n",cases,guard_cases);
    return 0;
}
