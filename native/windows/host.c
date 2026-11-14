/* Windows APIs and native D3D11 around the existing static C lift. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <intrin.h>
#include <smmintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include <errno.h>
#include "cpu64.h"
#include "recomp_dispatch.h"
#include "recomp_supplemental.h"
#include "bridge.h"
#include "loader.h"
#include "performance.h"
#include "input.h"
#include "native_validation.h"
#include "extra_aot.h"
#include "shader_lookup.h"
#include "simd_integer.h"
#include "callback_stacks.h"
#include "simd_float.h"

#define STACK_BYTES (16u << 20)
#define CALLBACK_LIMIT FP_CALLBACK_STACK_LIMIT
static fp_image game;
int64_t g_image_delta;
extern uint64_t es3_eh_image_base;
extern void es3_eh_thread_reset(void);
static wchar_t game_exe[32768], profile[32768], command_w[32768];
static char game_exe_a[32768], command_a[32768];
static const char *winmain_command_a="";
static unsigned char guest_peb[4096];
static _Thread_local uint64_t tls_vector[64];
static _Thread_local unsigned char guest_teb[0x2000];
static _Thread_local int tls_ready, tls_attached, call_depth;
static _Thread_local uint64_t guest_stack_base, guest_stack_limit, guest_stack_owner_low;
static _Thread_local CPU *active_cpu;
static _Thread_local uint64_t last_target;
static _Thread_local unsigned native_reap_tick;
static volatile LONG64 callback_calls;
static ULONGLONG started;
static int trace_calls, self_test_mode;

static _Noreturn void stop(const char *format, ...) {
    va_list args; fputs("STOP ", stderr); va_start(args,format);
    vfprintf(stderr,format,args); va_end(args); fputc('\n',stderr); fflush(stderr);
    ExitProcess(73);
}
static int guest_options(int argc,wchar_t **argv,int *gfxconfig,int *novideo) {
    *gfxconfig=*novideo=0;if(argc<4 || argc>6)return 0;
    for(int i=4;i<argc;++i) {
        if(!wcscmp(argv[i],L"--gfxconfig") && !*gfxconfig)*gfxconfig=1;
        else if(!wcscmp(argv[i],L"--novideo") && !*novideo)*novideo=1;
        else return 0;
    }
    return 1;
}
static void build_guest_command(int gfxconfig,int novideo) {
    static const char *tails[]={"","-gfxconfig","-novideo","-gfxconfig -novideo"};
    static const wchar_t *wide_tails[]={L"",L"-gfxconfig",L"-novideo",L"-gfxconfig -novideo"};
    unsigned mode=(gfxconfig?1u:0u)|(novideo?2u:0u);winmain_command_a=tails[mode];
    int wide=swprintf_s(command_w,32768,L"\"%ls\"%ls%ls",game_exe,mode?L" ":L"",wide_tails[mode]);
    int narrow=snprintf(command_a,sizeof(command_a),"\"%s\"%s%s",game_exe_a,mode?" ":"",winmain_command_a);
    if(wide<0 || narrow<0 || (size_t)narrow>=sizeof(command_a))stop("guest command line capacity");
}
static int argument_read(uint64_t address,void *output,size_t bytes) {
    SIZE_T got=0;return address && ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)address,output,bytes,&got) && got==bytes;
}
static void argument_diagnostic(const char *label,uint64_t address) {
    DWORD saved=GetLastError();char text[128];unsigned count=0;int complete=0;
    while(count<sizeof(text)-1) {
        unsigned char unit;if(!argument_read(address+count,&unit,1))break;
        if(!unit){complete=1;break;}text[count++]=(unit>=32 && unit<127)?(char)unit:'.';
    }
    text[count]=0;fprintf(stderr,"GUEST_ARGUMENT label=%s pointer=0x%llx bytes=%u complete=%d text=\"%s\"\n",label,address,count,complete,text);
    SetLastError(saved);
}
typedef struct {
    uint64_t application,config,settings;
    uint32_t quality,mode,index,width,height,refresh,override_width,override_height,override_refresh;
    unsigned char vsync,selected,override_present;
} graphics_settings;
static int graphics_settings_read(uint64_t context,graphics_settings *s) {
    unsigned char basic[36],override[16];memset(s,0,sizeof(*s));
    if(!context || context>UINT64_MAX-0x38 || !argument_read(context+0x30,&s->application,8) ||
       !s->application || s->application>UINT64_MAX-0x30 || !argument_read(s->application+0x28,&s->config,8) ||
       !s->config || s->config>UINT64_MAX-0x150)return 0;
    s->settings=s->config+0xb0;
    if(!argument_read(s->settings+0x1c,basic,sizeof(basic)) ||
       !argument_read(s->settings+0x90,override,sizeof(override)))return 0;
    memcpy(&s->quality,basic,4);s->vsync=basic[4];memcpy(&s->mode,basic+12,4);
    s->selected=basic[16];memcpy(&s->index,basic+20,4);
    memcpy(&s->width,basic+24,4);memcpy(&s->height,basic+28,4);memcpy(&s->refresh,basic+32,4);
    s->override_present=override[0];memcpy(&s->override_width,override+4,4);
    memcpy(&s->override_height,override+8,4);memcpy(&s->override_refresh,override+12,4);return 1;
}
static void graphics_settings_diagnostic(const char *phase,uint64_t context) {
    DWORD saved=GetLastError();int saved_errno=errno;graphics_settings s;
    int valid=graphics_settings_read(context,&s);
    fprintf(stderr,"GUEST_GRAPHICS_SETTINGS phase=%s context=0x%llx application=0x%llx config=0x%llx settings=0x%llx readable=%d quality=%u vsync=%u mode=%u selected=%u index=%u width=%u height=%u refresh=%u override_present=%u override_width=%u override_height=%u override_refresh=%u\n",
        phase,context,s.application,s.config,s.settings,valid,s.quality,(unsigned)s.vsync,s.mode,(unsigned)s.selected,s.index,
        s.width,s.height,s.refresh,(unsigned)s.override_present,s.override_width,s.override_height,s.override_refresh);
    errno=saved_errno;SetLastError(saved);
}
static int xgetbv_available(void) {
    int features[4];__cpuidex(features,1,0);
    return (features[2]&((1u<<26)|(1u<<27)))==((1u<<26)|(1u<<27));
}
static int handle_xgetbv(CPU *c,uint64_t va,const char *text) {
    if(!c || va!=UINT64_C(0x141ae0d80) || strcmp(text,"xgetbv") || (uint32_t)c->rcx ||
       !fp_image_code(&game,GVA(va)) || !fp_image_contains(&game,GVA(va)+2) ||
       memcmp((void *)(uintptr_t)GVA(va),"\x0f\x01\xd0",3) || !xgetbv_available())return 0;
    uint64_t value=fp_xgetbv(0);
    SET32(c->rax,value);SET32(c->rdx,value>>32);return 1;
}
static const uint64_t pmovsxdq_sites[]={UINT64_C(0x141c6707a),UINT64_C(0x141c670a5),
    UINT64_C(0x141c670cd),UINT64_C(0x141c670f0),UINT64_C(0x141c67619),UINT64_C(0x141c67643),
    UINT64_C(0x141c67669),UINT64_C(0x141c6768c)};
static int handle_pmovsxdq(CPU *c,uint64_t va,const char *text) {
    if(!c || strcmp(text,"pmovsxdq xmm2, xmm1"))return 0;
    unsigned i;for(i=0;i<sizeof(pmovsxdq_sites)/sizeof(*pmovsxdq_sites);++i)if(va==pmovsxdq_sites[i])break;
    if(i==sizeof(pmovsxdq_sites)/sizeof(*pmovsxdq_sites) || !fp_image_code(&game,GVA(va)) ||
       !fp_image_code(&game,GVA(va)+4) || memcmp((void *)(uintptr_t)GVA(va),"\x66\x0f\x38\x25\xd1",5))return 0;
    c->xmm[2].i64[0]=(int64_t)c->xmm[1].i32[0];
    c->xmm[2].i64[1]=(int64_t)c->xmm[1].i32[1];return 1;
}
typedef struct {uint64_t va;const char *text,*bytes;unsigned size,destination,source,lane;} pextrd_site;
#define PEXTRD_SITE(va,text,bytes,size,destination,source,lane) {UINT64_C(va),text,bytes,size,offsetof(CPU,destination),source,lane}
static const pextrd_site pextrd_sites[]={
    PEXTRD_SITE(0x141bf74b0,"pextrd r11d, xmm0, 0","\x66\x41\x0f\x3a\x16\xc3\x00",7,r11,0,0),
    PEXTRD_SITE(0x141bf74bb,"pextrd esi, xmm0, 1","\x66\x0f\x3a\x16\xc6\x01",6,rsi,0,1),
    PEXTRD_SITE(0x141bf74c1,"pextrd r13d, xmm0, 2","\x66\x41\x0f\x3a\x16\xc5\x02",7,r13,0,2),
    PEXTRD_SITE(0x141bf74c8,"pextrd ebx, xmm0, 3","\x66\x0f\x3a\x16\xc3\x03",6,rbx,0,3),
    PEXTRD_SITE(0x141bf74ce,"pextrd r8d, xmm3, 0","\x66\x41\x0f\x3a\x16\xd8\x00",7,r8,3,0),
    PEXTRD_SITE(0x141bf74d5,"pextrd edx, xmm3, 1","\x66\x0f\x3a\x16\xda\x01",6,rdx,3,1),
    PEXTRD_SITE(0x141bf74db,"pextrd r9d, xmm3, 2","\x66\x41\x0f\x3a\x16\xd9\x02",7,r9,3,2),
    PEXTRD_SITE(0x141bf74e2,"pextrd r14d, xmm3, 3","\x66\x41\x0f\x3a\x16\xde\x03",7,r14,3,3),
    PEXTRD_SITE(0x141bf7913,"pextrd r10d, xmm0, 0","\x66\x41\x0f\x3a\x16\xc2\x00",7,r10,0,0),
    PEXTRD_SITE(0x141bf791a,"pextrd ebp, xmm0, 1","\x66\x0f\x3a\x16\xc5\x01",6,rbp,0,1),
    PEXTRD_SITE(0x141bf7920,"pextrd r13d, xmm0, 2","\x66\x41\x0f\x3a\x16\xc5\x02",7,r13,0,2),
    PEXTRD_SITE(0x141bf7927,"pextrd esi, xmm0, 3","\x66\x0f\x3a\x16\xc6\x03",6,rsi,0,3)
};
#undef PEXTRD_SITE
static int handle_pextrd(CPU *c,uint64_t va,const char *text) {
    if(!c)return 0;
    for(unsigned i=0;i<sizeof(pextrd_sites)/sizeof(*pextrd_sites);++i) {
        const pextrd_site *site=&pextrd_sites[i];
        if(va!=site->va)continue;
        uint64_t address=GVA(va);
        if(strcmp(text,site->text) || !fp_image_code(&game,address) ||
           !fp_image_code(&game,address+site->size-1) || memcmp((void *)(uintptr_t)address,site->bytes,site->size))return 0;
        uint64_t value=c->xmm[site->source].u32[site->lane];
        memcpy((unsigned char *)c+site->destination,&value,sizeof(value));return 1;
    }
    return 0;
}
void frostpunk_unsupported(uint64_t va,const char *text) {
    if(handle_xgetbv(active_cpu,va,text))return;
    if(handle_pmovsxdq(active_cpu,va,text))return;
    if(handle_pextrd(active_cpu,va,text))return;
    if(fp_simd_integer(active_cpu,&game,va,text))return;
    if(fp_simd_float(active_cpu,&game,va,text))return;
    stop("unsupported instruction va=0x%" PRIx64 " %s",va,text);
}
int es3_cs_depth_get(void) { return call_depth; }
void es3_cs_depth_set(int depth) {
    if(depth<0 || depth>call_depth)stop("invalid EH dispatch depth=%d",depth);
    call_depth=depth;
}
static void call_tls(CPU *c,unsigned reason);
static void native_stack_bounds(uint64_t base,uint64_t limit) {
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    tib->StackBase=(void *)(uintptr_t)base;tib->StackLimit=(void *)(uintptr_t)limit;
}

static void tls_initialize(void) {
    if(tls_ready) return;
    tls_ready=1;
    memcpy(guest_teb,NtCurrentTeb(),4096);
    if(game.tls_index) {
        size_t bytes=(size_t)(game.tls_end-game.tls_start), total=bytes+game.tls_zero;
        void *storage=calloc(total?total:1,1);
        if(!storage) stop("guest TLS allocation");
        if(bytes) memcpy(storage,(void *)(uintptr_t)game.tls_start,bytes);
        tls_vector[0]=(uintptr_t)storage;
    }
}
uint64_t frostpunk_teb_read(uint64_t off,unsigned bytes) {
    tls_initialize();
    if(bytes!=4 && bytes!=8) stop("TEB width %u",bytes);
    if(off>=0x2000 || off+bytes>0x2000) stop("TEB offset 0x%" PRIx64,off);
    if(off==0x10 && active_cpu && guest_stack_owner_low && guest_stack_owner_low<guest_stack_limit)
        {uint64_t limit=fp_native_stack_commit_limit(active_cpu->rsp,guest_stack_base,guest_stack_owner_low);
         if(!limit)stop("guest stack committed limit validation");guest_stack_limit=limit;}
    wr64((uintptr_t)guest_teb+0x30,(uintptr_t)guest_teb);
    wr64((uintptr_t)guest_teb+0x58,(uintptr_t)tls_vector);
    wr64((uintptr_t)guest_teb+0x60,(uintptr_t)guest_peb);
    wr64((uintptr_t)guest_teb+0x08,guest_stack_base?guest_stack_base:(uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase);
    wr64((uintptr_t)guest_teb+0x10,guest_stack_limit?guest_stack_limit:(uintptr_t)((NT_TIB *)NtCurrentTeb())->StackLimit);
    wr32((uintptr_t)guest_teb+0x68,GetLastError());
    return bytes==8?rd64((uintptr_t)guest_teb+off):rd32((uintptr_t)guest_teb+off);
}
void frostpunk_teb_write(uint64_t off,uint64_t value,unsigned bytes) {
    if((bytes!=4 && bytes!=8) || off>=0x2000 || off+bytes>0x2000) stop("TEB write bounds");
    if((off<0x38 && off+bytes>0x30) || (off<0x68 && off+bytes>0x58))stop("guest attempted to replace TLS/TEB/PEB identity");
    tls_initialize();memcpy(guest_teb+off,&value,bytes);
    if(off==0x68 && bytes==4)SetLastError((DWORD)value);
}

static void *checked(uint64_t address,size_t bytes,int write) {
    uint64_t end=address+bytes,cursor=address;
    if(end<address) stop("memory span overflow");
    if(fp_native_stack_cached(address,bytes,guest_stack_owner_low?guest_stack_owner_low:guest_stack_limit,guest_stack_base))return (void *)(uintptr_t)address;
    while(cursor<end) {
        MEMORY_BASIC_INFORMATION info;
        if(!VirtualQuery((void *)(uintptr_t)cursor,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) ||
           !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
            stop("unreadable memory 0x%" PRIx64,cursor);
        if(write && !(info.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
            stop("readonly memory 0x%" PRIx64,cursor);
        uint64_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        if(next<=cursor) stop("memory region overflow");
        cursor=next<end?next:end;
    }
    return (void *)(uintptr_t)address;
}

static void (*lookup(uint64_t target))(CPU *) {
    uint64_t original=target-g_image_delta;
    static _Thread_local struct {uint64_t target;void(*fn)(CPU*);} cache[1024];
    size_t slot=((original>>4)^(original>>14))&1023;
    if(cache[slot].fn && cache[slot].target==original) return cache[slot].fn;
    void(*fn)(CPU*)=fp_shader_lookup(original);
    if(!fn) {
        uint32_t lo=0,hi=recomp_dispatch_count;
        while(lo<hi) {uint32_t m=lo+(hi-lo)/2;if(recomp_dispatch_table[m].va<original)lo=m+1;else hi=m;}
        fn=lo<recomp_dispatch_count&&recomp_dispatch_table[lo].va==original?recomp_dispatch_table[lo].fn:
            frostpunk_supplemental_lookup(original);
    }
    if(!fn)fn=fp_extra_aot_lookup(original);
    if(fn) {cache[slot].target=original;cache[slot].fn=fn;}
    return fn;
}
static void returned(CPU *c,uint64_t value) {c->rax=value;c->rip=rd64((uintptr_t)checked(c->rsp,8,0));c->rsp+=8;}

typedef fp_native_hook hook;
static hook hooks[HLE_COUNT];
static unsigned hook_count;
static const char *hook_name(uint64_t target) {
    return fp_native_hook_name(hooks,hook_count,target);
}
static uint64_t qsort_u32_target,qsort_pointer_target,qsort_float_target;
static int __cdecl qsort_u32_desc(const void *a,const void *b) {
    uint32_t left=rd32((uintptr_t)a),right=rd32((uintptr_t)b);
    return (left<right)-(left>right);
}
static int __cdecl qsort_pointer_u32(const void *a,const void *b) {
    uint32_t left=rd32(rd64((uintptr_t)a)),right=rd32(rd64((uintptr_t)b));
    return (left>right)-(left<right);
}
int32_t fp_qsort_float_difference(uint32_t right_bits,uint32_t left_bits);
static int __cdecl qsort_float_records(const void *a,const void *b) {
    uint64_t right=rd64((uintptr_t)b),left=rd64((uintptr_t)a);
    uint32_t right_bits=rd32(right+0x200),left_bits=rd32(left+0x200);
    /* The NX callback restores its input MXCSR; retain that boundary behavior. */
    int32_t difference=fp_qsort_float_difference(right_bits,left_bits);
    if(difference)return difference>0?1:-1;
    uint32_t result=rd32((uintptr_t)b+0x10)-rd32((uintptr_t)a+0x10);
    if(!result)result=rd32((uintptr_t)b+0x18)-rd32((uintptr_t)a+0x18);
    if(!result)result=rd32((uintptr_t)b+0x20)-rd32((uintptr_t)a+0x20);
    return (int32_t)result;
}
static void initialize_qsort_callbacks(uint64_t target) {
    static const unsigned char original[]={0x8b,0x01,0x8b,0x0a,0x3b,0xc8,0x76,0x06,
        0xb8,0x01,0x00,0x00,0x00,0xc3,0x1b,0xc0,0xc3};
    uint64_t at=GVA(0x14102f320);qsort_u32_target=0;
    if(fp_image_code(&game,at) && fp_image_code(&game,at+sizeof(original)-1) &&
       !memcmp((void *)(uintptr_t)at,original,sizeof(original)))qsort_u32_target=target;
    static const unsigned char pointer_original[]={0x48,0x8b,0x01,0x44,0x8b,0x00,0x48,0x8b,
        0x02,0x33,0xd2,0x8b,0x08,0x8b,0xc2,0x44,0x3b,0xc1,0x0f,0x97,0xc0,0x0f,0x92,
        0xc2,0x2b,0xc2,0xc3};
    at=GVA(0x141c561a0);qsort_pointer_target=0;
    if(fp_image_code(&game,at) && fp_image_code(&game,at+sizeof(pointer_original)-1) &&
       !memcmp((void *)(uintptr_t)at,pointer_original,sizeof(pointer_original)))qsort_pointer_target=target;
    static const unsigned char float_original[]={0x48,0x8b,0x02,0x4c,0x8b,0x01,0xf3,0x0f,
        0x10,0x80,0x00,0x02,0x00,0x00,0xf3,0x41,0x0f,0x5c,0x80,0x00,0x02,0x00,0x00,
        0xf3,0x0f,0x2c,0xc0,0x85,0xc0,0x7e,0x06,0xb8,0x01,0x00,0x00,0x00,0xc3,0x79,
        0x06,0xb8,0xff,0xff,0xff,0xff,0xc3,0x8b,0x42,0x10,0x2b,0x41,0x10,0x75,0x0e,
        0x8b,0x42,0x18,0x2b,0x41,0x18,0x75,0x06,0x8b,0x42,0x20,0x2b,0x41,0x20,0xc3};
    at=GVA(0x1410310d0);qsort_float_target=0;
    if(fp_image_code(&game,at) && fp_image_code(&game,at+sizeof(float_original)-1) &&
       !memcmp((void *)(uintptr_t)at,float_original,sizeof(float_original)))qsort_float_target=target;
}
static int gfx_path_read(uint64_t address,int is_wide,char *utf8,int *complete) {
    wchar_t wide[1024];char narrow[1024];size_t length=0;*complete=0;
    while(address && length<1023) {
        unsigned char block[128];size_t unit=is_wide?2:1;
        if(address>UINT64_MAX-length*unit)return 0;
        uint64_t cursor=address+length*unit;
        size_t bytes=4096-(cursor&4095);if(bytes>sizeof(block))bytes=sizeof(block);
        if(bytes>(1023-length)*unit)bytes=(1023-length)*unit;
        bytes-=bytes%unit;if(!bytes)bytes=unit;
        if(!argument_read(cursor,block,bytes))return 0;
        for(size_t at=0;at<bytes;at+=unit) {
            unsigned value=block[at];if(is_wide)value|=(unsigned)block[at+1]<<8;
            if(!value){*complete=1;break;}
            wide[length]=(wchar_t)value;narrow[length]=(char)value;++length;
        }
        if(*complete)break;
    }
    int match=0;
    for(size_t at=0;at+9<=length;++at) {
        static const char key[]="gfxconfig";size_t n=0;
        while(n<9){unsigned v=wide[at+n];if(v>='A'&&v<='Z')v+='a'-'A';if(v!=(unsigned char)key[n])break;++n;}
        if(n==9){match=1;break;}
    }
    if(!match)return 0;
    int units=is_wide?(int)length:MultiByteToWideChar(CP_ACP,0,narrow,(int)length,wide,1024);
    int bytes=units?WideCharToMultiByte(CP_UTF8,0,wide,units,utf8,4095,NULL,NULL):0;utf8[bytes]=0;return 1;
}
static __declspec(noinline) void gfx_file_diagnostic_cold(const char *name,uint64_t address,int is_wide) {
    DWORD saved=GetLastError();int saved_errno=errno,complete;char text[4096];static volatile LONG count;
    if(gfx_path_read(address,is_wide,text,&complete) && InterlockedIncrement(&count)<=8)
        fprintf(stderr,"GUEST_GFX_FILE api=%s pointer=0x%llx complete=%d path=\"%s\"\n",name,address,complete,text);
    errno=saved_errno;SetLastError(saved);
}
static void gfx_file_diagnostic(const CPU *c,const char *name) {
    uint64_t address;int is_wide;
    switch(name[0]) {
    case 'C':
        if(strcmp(name,"CreateFileW"))return;address=c->rcx;is_wide=1;break;
    case 'G':
        if(strcmp(name,"GetFileAttributesW") && strcmp(name,"GetFileAttributesExW"))return;
        address=c->rcx;is_wide=1;break;
    case '_':
        if(!strcmp(name,"_wfopen")){address=c->rcx;is_wide=1;}
        else if(!strcmp(name,"_wfopen_s")){address=c->rdx;is_wide=1;}
        else return;break;
    case 'f':
        if(strcmp(name,"fopen_s"))return;address=c->rdx;is_wide=0;break;
    default:return;
    }
    gfx_file_diagnostic_cold(name,address,is_wide);
}
static __declspec(noinline) void native_diagnostic_cold(const CPU *c,const char *name,int debug,int exiting) {
    DWORD saved_error=GetLastError();
    if(debug) {
        wchar_t wide[4096];char narrow[4096],utf8[16384];size_t length=0;int is_wide=name[17]=='W';
        if(!c->rcx)fprintf(stderr,"GUEST_DEBUG_%c <NULL>\n",is_wide?'W':'A');
        else {
            for(;length<4095;++length) {
                unsigned unit;
                if(is_wide){unit=rd16((uintptr_t)checked(c->rcx+length*2,2,0));wide[length]=(wchar_t)unit;}
                else{unit=rd8((uintptr_t)checked(c->rcx+length,1,0));narrow[length]=(char)unit;}
                if(!unit)break;
            }
            int units=is_wide?(int)length:MultiByteToWideChar(CP_ACP,0,narrow,(int)length,wide,4096);
            int bytes=units?WideCharToMultiByte(CP_UTF8,0,wide,units,utf8,sizeof(utf8)-1,NULL,NULL):0;
            utf8[bytes]=0;
            fprintf(stderr,"GUEST_DEBUG_%c %s%s\n",is_wide?'W':'A',utf8,length==4095?" [truncated at 4095 units]":"");
        }
    } else if(exiting) {
        fprintf(stderr,"GUEST_EXIT api=%s code=%u return=0x%" PRIx64 "\n",name,(uint32_t)c->rcx,
            rd64((uintptr_t)checked(c->rsp,8,0)));
    } else if(!strcmp(name,"_cexit")) {
        fprintf(stderr,"GUEST_EXIT api=_cexit code=none return=0x%" PRIx64 "\n",rd64((uintptr_t)checked(c->rsp,8,0)));
    }
    SetLastError(saved_error);
}
static __declspec(noinline) void native_diagnostic(const CPU *c,const char *name) {
    switch(name[0]) {
    case 'O':
        if(!strcmp(name,"OutputDebugStringA") || !strcmp(name,"OutputDebugStringW")) {
            native_diagnostic_cold(c,name,1,0);return;
        }break;
    case 'E':
        if(!strcmp(name,"ExitProcess")){native_diagnostic_cold(c,name,0,1);return;}break;
    case 'e':
        if(!strcmp(name,"exit")){native_diagnostic_cold(c,name,0,1);return;}break;
    case '_':
        if(!strcmp(name,"_exit")){native_diagnostic_cold(c,name,0,1);return;}
        if(!strcmp(name,"_cexit")){native_diagnostic_cold(c,name,0,0);return;}break;
    }
    gfx_file_diagnostic(c,name);
}
static int hooked(CPU *c,uint64_t target,const char *name) {
    if(!name) return 0;
    native_diagnostic(c,name);
    if(!strcmp(name,"GetModuleHandleA") || !strcmp(name,"GetModuleHandleW")) {
        if(!c->rcx) {returned(c,(uintptr_t)game.base);return 1;}
    } else if(!strcmp(name,"GetModuleFileNameA") || !strcmp(name,"GetModuleFileNameW")) {
        if(!c->rcx || c->rcx==(uintptr_t)game.base) {
            size_t n=name[17]=='W'?wcslen(game_exe):strlen(game_exe_a),cap=(uint32_t)c->r8;
            size_t count=n<cap?n:cap;
            if(name[17]=='W') {if(count)memcpy(checked(c->rdx,count*2,1),game_exe,count*2);if(n<cap)wr16(c->rdx+n*2,0);}
            else {if(count)memcpy(checked(c->rdx,count,1),game_exe_a,count);if(n<cap)wr8(c->rdx+n,0);}
            if(n>=cap)SetLastError(ERROR_INSUFFICIENT_BUFFER);
            returned(c,count);return 1;
        }
    } else if(!strcmp(name,"GetCommandLineW")) {returned(c,(uintptr_t)command_w);return 1;}
    else if(!strcmp(name,"GetCommandLineA")) {returned(c,(uintptr_t)command_a);return 1;}
    else if(!strcmp(name,"_get_narrow_winmain_command_line")) {returned(c,(uintptr_t)winmain_command_a);return 1;}
    else if(!strcmp(name,"VirtualProtect") && c->rdx && c->rcx<(uintptr_t)game.base+game.size &&
        (c->rcx>=(uintptr_t)game.base || c->rdx>(uintptr_t)game.base-c->rcx)) {
        DWORD protection=(DWORD)c->r8;
        if(protection&PAGE_EXECUTE)protection=(protection&~PAGE_EXECUTE)|PAGE_READONLY;
        if(protection&PAGE_EXECUTE_READ)protection=(protection&~PAGE_EXECUTE_READ)|PAGE_READONLY;
        if(protection&PAGE_EXECUTE_READWRITE)protection=(protection&~PAGE_EXECUTE_READWRITE)|PAGE_READWRITE;
        if(protection&PAGE_EXECUTE_WRITECOPY)protection=(protection&~PAGE_EXECUTE_WRITECOPY)|PAGE_WRITECOPY;
        returned(c,VirtualProtect((void *)(uintptr_t)c->rcx,(SIZE_T)c->rdx,protection,(DWORD *)(uintptr_t)c->r9));return 1;
    } else if(!strcmp(name,"_CxxThrowException")) {
        uint64_t ret=pop64(c);
        if(!es3_eh_throw(c,ret,c->rcx,c->rdx))stop("uncaught guest C++ exception return=0x%" PRIx64,ret);
        return 1;
    } else if(!strcmp(name,"RaiseException") && (uint32_t)c->rcx==0x406d1388) {
        returned(c,0);return 1;
    } else if(!strcmp(name,"curl_easy_perform")) {returned(c,7);return 1;}
    else if(strstr(name,"?ReadJSONFromURL@NewsFeedDLL")) {returned(c,1);return 1;}
    return 0;
}

static int self_target(uint64_t target) {
    uint64_t offset=target-(uintptr_t)game.base;
    return self_test_mode && (offset==0 || offset==16 || offset==32 || offset==48 || offset==64 || offset==80);
}
static volatile LONG self_tls_attach,self_tls_process,self_nested;
static _Thread_local unsigned self_tls_thread;
static __declspec(noinline) uint64_t self_native_nested(uint64_t target,uint64_t value) {
    return ((uint64_t(*)(uint64_t))(uintptr_t)target)(value)+11;
}
static __declspec(noinline) void self_function(CPU *c,uint64_t target) {
    uint64_t offset=target-(uintptr_t)game.base;
    if(offset==0) {
        uint64_t result=c->rcx+c->rdx+c->r8+c->r9;
        for(unsigned i=0;i<8;++i)result+=rd64(c->rsp+40+i*8);
        returned(c,result);
    } else if(offset==16) {
        int a=*(int *)(uintptr_t)c->rcx,b=*(int *)(uintptr_t)c->rdx;
        returned(c,(uint32_t)((a>b)-(a<b)));
    } else if(offset==32) {
        CPU saved=*c;
        c->rsp=(c->rsp-128)&~UINT64_C(15);uint64_t before=c->rsp;
        c->rcx=(uintptr_t)game.base+48;c->rdx=saved.rcx;
        push64(c,UINT64_C(0xe5ef0020));
        dispatch(c,(uintptr_t)self_native_nested);
        if(c->rsp!=before)stop("nested native stack imbalance");
        uint64_t result=c->rax;*c=saved;InterlockedIncrement(&self_nested);returned(c,result+3);
    } else if(offset==48) {
        returned(c,c->rcx+7);
    } else if(offset==64) {
        if(c->rdx==DLL_PROCESS_ATTACH)InterlockedIncrement(&self_tls_process);
        else if(c->rdx==DLL_THREAD_ATTACH){++self_tls_thread;InterlockedIncrement(&self_tls_attach);}
        else stop("unexpected TLS self-test reason");
        returned(c,1);
    } else if(offset==80) {
        SetLastError((DWORD)c->rcx);returned(c,1);
    } else stop("unknown self-test callback");
}
static __declspec(noinline) void dispatch_static_cold(CPU *c,uint64_t target,void (*fn)(CPU *),int self) {
    if(++call_depth>8192)stop("dispatch recursion limit target=0x%" PRIx64,target);
    CPU *saved=active_cpu;active_cpu=c;
    if(trace_calls)fprintf(stderr,"STATIC target=0x%" PRIx64 " depth=%d\n",target,call_depth);
    uint64_t argument_context=0,window_context=0;static volatile LONG window_diagnostic_count;
    int window_diagnostic=target==GVA(0x141add740) && InterlockedIncrement(&window_diagnostic_count)<=4;
    if(window_diagnostic){window_context=c->rcx;graphics_settings_diagnostic("before_window_init",window_context);}
    if(target==GVA(0x140e02420)) {
        fprintf(stderr,"WINMAIN_ENTERED elapsed_ms=%llu\n",GetTickCount64()-started);
        argument_diagnostic("WinMain.lpCmdLine.R8",c->r8);
    }
    if(target==GVA(0x141adea60)) {
        DWORD saved_error=GetLastError();uint64_t input=0,raw=0;argument_context=c->rcx;
        if(argument_read(argument_context+0x30,&input,8))argument_read(input,&raw,8);
        argument_diagnostic("engine.parse.raw",raw);SetLastError(saved_error);
    }
    if(self)self_function(c,target);else fn(c);
    if(window_diagnostic)graphics_settings_diagnostic("after_window_init",window_context);
    if(argument_context) {
        DWORD saved_error=GetLastError();uint64_t options=0;unsigned char flag=0;
        int valid=argument_read(argument_context+0x38,&options,8) && argument_read(options+0x20,&flag,1);
        fprintf(stderr,"GUEST_ARGUMENT_PARSE context=0x%llx options=0x%llx readable=%d gfxconfig_flag=%u\n",argument_context,options,valid,(unsigned)flag);
        SetLastError(saved_error);
    }
    active_cpu=saved;--call_depth;return;
}
static __forceinline void dispatch_static(CPU *c,uint64_t target,void (*fn)(CPU *),int self) {
    if(self || trace_calls || target==GVA(0x141add740) || target==GVA(0x140e02420) || target==GVA(0x141adea60)) {
        dispatch_static_cold(c,target,fn,self);return;
    }
    if(++call_depth>8192)stop("dispatch recursion limit target=0x%" PRIx64,target);
    CPU *saved=active_cpu;active_cpu=c;
    fn(c);
    active_cpu=saved;--call_depth;
}
static __declspec(noinline) void dispatch_native(CPU *c,uint64_t target) {
    if(fp_image_contains(&game,target))stop("missing static target=0x%" PRIx64 " return=0x%" PRIx64,target,rd64(c->rsp));
    DWORD native_error=GetLastError();
    int mutation=fp_native_memory_mutation(target);
    if(mutation)fp_native_memory_mutation_begin(target,c->rcx,c->rdx);
    const char *name=hook_name(target);
    if(fp_windows_input_call(c,target,name)) {
        if(mutation)fp_native_memory_mutation_end();return;
    }
    if(hooked(c,target,name)) {if(mutation)fp_native_memory_mutation_end();return;}
    if(!fp_native_target_valid(target))
        stop("invalid native target=0x%" PRIx64 " guest_rip=0x%" PRIx64,target,c->rip);
    unsigned bytes=0;
    if(fp_native_stack_prepare(c->rsp,40+FP_NATIVE_MAX_STACK_BYTES,guest_stack_owner_low?guest_stack_owner_low:guest_stack_limit,guest_stack_base))bytes=FP_NATIVE_MAX_STACK_BYTES;
    else {
        checked(c->rsp,40,0);
        while(bytes<256) {
            MEMORY_BASIC_INFORMATION info;
            if(!VirtualQuery((void *)(uintptr_t)(c->rsp+40+bytes),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
               (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) ||
               !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))break;
            uint64_t available=(uintptr_t)info.BaseAddress+info.RegionSize-(c->rsp+40+bytes);
            unsigned add=available<256-bytes?(unsigned)available:256-bytes;
            bytes+=add;if(!add)break;
        }
    }
    if(!(++native_reap_tick&4095u))fp_callback_stacks_reap();
    if(trace_calls)fprintf(stderr,"NATIVE target=0x%" PRIx64 " name=%s\n",target,name?name:"dynamic");
    fp_performance_native_begin(c,target);
    uint64_t comparator=0;
    if(qsort_u32_target && target==qsort_u32_target && c->r9==GVA(0x14102f320))comparator=(uintptr_t)qsort_u32_desc;
    else if(qsort_pointer_target && target==qsort_pointer_target && c->r9==GVA(0x141c561a0))comparator=(uintptr_t)qsort_pointer_u32;
    /* shortcut: unmasked FP exceptions retain NX callbacks until guest SSE exceptions are bridged. */
    else if(qsort_float_target && target==qsort_float_target && c->r9==GVA(0x1410310d0) &&
            (_mm_getcsr()&0x1f80)==0x1f80)comparator=(uintptr_t)qsort_float_records;
    SetLastError(native_error);
    if(comparator) {
        /* Keep the guest callback pointer intact even if the native call faults. */
        CPU native=*c;native.r9=comparator;
        fp_native_call(&native,target,bytes&~7u);native.r9=c->r9;*c=native;
    } else fp_native_call(c,target,bytes&~7u);
    DWORD result_error=GetLastError();if(mutation)fp_native_memory_mutation_end();
    fp_performance_native_return(c,target);SetLastError(result_error);
}
void dispatch(CPU *c,uint64_t target) {
    last_target=target;
    int self=self_target(target);
    void(*fn)(CPU*)=fp_image_code(&game,target)&&!self?lookup(target):NULL;
    if(fn || self) {dispatch_static(c,target,fn,self);return;}
    dispatch_native(c,target);
}

/* Image metadata is immutable while guest threads run. Initialize before guest
 * callbacks; reset before replacement/unload, after all guest threads quiesce. */
static uint64_t direct_code_begin,direct_code_size;
static void direct_image_reset(void) {direct_code_size=0;direct_code_begin=0;}
static void direct_image_init(void) {
    direct_image_reset();
    if(!game.base || !game.sections || (uintptr_t)game.base>UINT64_MAX-game.size)return;
    for(size_t i=0;i<game.section_count;++i) {
        const fp_section *section=&game.sections[i];
        if((section->characteristics&IMAGE_SCN_MEM_EXECUTE) && section->size>direct_code_size &&
           section->rva<=game.size && section->size<=game.size-section->rva) {
            direct_code_begin=(uintptr_t)game.base+section->rva;direct_code_size=section->size;
        }
    }
}
static __forceinline int direct_code_contains(uint64_t target) {
    return target-direct_code_begin<direct_code_size;
}
void dispatch_jmp(CPU *c,uint64_t target) {dispatch(c,target);}

typedef struct {
    CONTEXT resume;
    CPU cpu;
    void *stack;
    uint64_t native_base,native_limit,native_owner_low,guest_base,guest_limit,guest_owner_low;
    DWORD last_error;
    int depth;
    CPU *previous_cpu;
    es3_ehframe_t eh_boundary,*previous_eh;
} callback;
static _Thread_local callback callbacks[CALLBACK_LIMIT];
static _Thread_local unsigned callback_depth;

static void cpu_from_context(CPU *c,const CONTEXT *r) {
    memset(c,0,sizeof(*c));
    c->rax=r->Rax;c->rcx=r->Rcx;c->rdx=r->Rdx;c->rbx=r->Rbx;c->rsp=r->Rsp;c->rbp=r->Rbp;c->rsi=r->Rsi;c->rdi=r->Rdi;
    c->r8=r->R8;c->r9=r->R9;c->r10=r->R10;c->r11=r->R11;c->r12=r->R12;c->r13=r->R13;c->r14=r->R14;c->r15=r->R15;c->rip=r->Rip;
    eflags_unpack(c,r->EFlags);memcpy(c->xmm,&r->Xmm0,sizeof(c->xmm));c->mxcsr=r->MxCsr;
}
static _Noreturn void callback_entry(callback *frame) {
    tls_initialize();
    CPU *c=&frame->cpu;uint64_t sp=c->rsp,ret=rd64((uintptr_t)checked(sp,8,0));
    active_cpu=c;
    es3_eh_push(&frame->eh_boundary,0,NULL);
    frame->previous_eh=frame->eh_boundary.prev;frame->eh_boundary.prev=NULL;
    if(!tls_attached){tls_attached=1;call_tls(c,DLL_THREAD_ATTACH);}
    SetLastError(frame->last_error);
    dispatch(c,c->rip);
    DWORD result_error=GetLastError();
    if(c->rsp!=sp+8)stop("callback stack imbalance target=0x%" PRIx64 " delta=%" PRId64,frame->resume.Rip,(int64_t)(c->rsp-sp));
    CONTEXT *r=&frame->resume;
    r->Rax=c->rax;r->Rcx=c->rcx;r->Rdx=c->rdx;r->Rbx=c->rbx;r->Rsp=c->rsp;r->Rbp=c->rbp;r->Rsi=c->rsi;r->Rdi=c->rdi;
    r->R8=c->r8;r->R9=c->r9;r->R10=c->r10;r->R11=c->r11;r->R12=c->r12;r->R13=c->r13;r->R14=c->r14;r->R15=c->r15;
    r->Rip=ret;r->EFlags=(r->EFlags&~0x8d5u)|((DWORD)eflags_pack(c)&0x8d5u);r->MxCsr=c->mxcsr;memcpy(&r->Xmm0,c->xmm,sizeof(c->xmm));
    /* The copied CONTEXT has no trailing XSTATE blob. */
    r->ContextFlags=CONTEXT_ALL;
    es3_ehframe_t check;es3_eh_push(&check,0,NULL);
    if(check.prev!=&frame->eh_boundary)stop("callback leaked guest EH frames");
    es3_eh_pop(&check);frame->eh_boundary.prev=frame->previous_eh;es3_eh_pop(&frame->eh_boundary);
    guest_stack_base=frame->guest_base;guest_stack_limit=frame->guest_limit;guest_stack_owner_low=frame->guest_owner_low;call_depth=frame->depth;
    active_cpu=frame->previous_cpu;
    uint64_t restored_limit=fp_native_stack_commit_limit(sp,frame->native_base,frame->native_owner_low);
    if(!restored_limit)stop("native callback stack committed restore validation");
    native_stack_bounds(frame->native_base,restored_limit);
    --callback_depth;SetLastError(result_error);
    RtlRestoreContext(r,NULL);
    stop("RtlRestoreContext returned");
}
static void fault_registers(const CONTEXT *r) {
    fprintf(stderr,"HOST_NATIVE_GPR rax=%016llx rcx=%016llx rdx=%016llx rbx=%016llx rsp=%016llx rbp=%016llx rsi=%016llx rdi=%016llx r8=%016llx r9=%016llx r10=%016llx r11=%016llx r12=%016llx r13=%016llx r14=%016llx r15=%016llx eflags=%08lx mxcsr=%08lx\n",
        r->Rax,r->Rcx,r->Rdx,r->Rbx,r->Rsp,r->Rbp,r->Rsi,r->Rdi,r->R8,r->R9,r->R10,r->R11,r->R12,r->R13,r->R14,r->R15,r->EFlags,r->MxCsr);
    CPU snapshot;SIZE_T got=0;
    if(!active_cpu || !ReadProcessMemory(GetCurrentProcess(),active_cpu,&snapshot,sizeof(snapshot),&got) || got!=sizeof(snapshot))return;
    const CPU *c=&snapshot;
    fprintf(stderr,"HOST_GUEST_GPR rip=%016" PRIx64 " rax=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 " rbx=%016" PRIx64 " rsp=%016" PRIx64 " rbp=%016" PRIx64 " rsi=%016" PRIx64 " rdi=%016" PRIx64 " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64 " r11=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64 " r15=%016" PRIx64 "\n",
        c->rip,c->rax,c->rcx,c->rdx,c->rbx,c->rsp,c->rbp,c->rsi,c->rdi,c->r8,c->r9,c->r10,c->r11,c->r12,c->r13,c->r14,c->r15);
    fprintf(stderr,"HOST_GUEST_FLAGS cf=%" PRIu64 " zf=%" PRIu64 " sf=%" PRIu64 " of=%" PRIu64 " pf=%" PRIu64 " af=%" PRIu64 " mxcsr=%08x\n",c->cf,c->zf,c->sf,c->of,c->pf,c->af,c->mxcsr);
    for(unsigned i=0;i<16;++i)fprintf(stderr,"HOST_GUEST_XMM%u=%016" PRIx64 ":%016" PRIx64 "\n",i,c->xmm[i].u64[1],c->xmm[i].u64[0]);
    unsigned char object[0x58];got=0;
    if(c->rbx && c->rbx<=UINT64_MAX-sizeof(object)) {
        int complete=ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)c->rbx,object,sizeof(object),&got)!=0;
        if(got>sizeof(object))got=sizeof(object);
        fprintf(stderr,"HOST_GUEST_OBJECT rbx=%016" PRIx64 " bytes=%zu complete=%d data=",c->rbx,(size_t)got,complete);
        for(SIZE_T i=0;i<got;++i)fprintf(stderr,"%02x",object[i]);fputc('\n',stderr);
    }
}
static void callback_stack_identity_diagnostic(const CONTEXT *r,const callback *frame) {
    DWORD saved=GetLastError();CONTEXT here={0};RtlCaptureContext(&here);
    uint64_t addresses[]={r->Rsp,frame->native_base?frame->native_base-1:0,here.Rsp};
    const char *labels[]={"exception_rsp","captured_StackBase_minus1","current_native_rsp"};
    fprintf(stderr,"CALLBACK_STACK_IDENTITY_REJECT target=0x%llx exception_rsp=0x%llx current_native_rsp=0x%llx captured_base=0x%llx captured_limit=0x%llx current_TEB_base=0x%llx current_TEB_limit=0x%llx owner_low=0x%llx tid=%lu callback_depth=%u call_depth=%d last_target=0x%llx last_error=%lu\n",
        r->Rip,r->Rsp,here.Rsp,frame->native_base,frame->native_limit,__readgsqword(8),__readgsqword(0x10),
        frame->native_owner_low,GetCurrentThreadId(),callback_depth,call_depth,last_target,saved);
    for(unsigned i=0;i<3;++i) {
        MEMORY_BASIC_INFORMATION info={0};SetLastError(0);
        SIZE_T count=VirtualQuery((void *)(uintptr_t)addresses[i],&info,sizeof(info));DWORD query_error=GetLastError();
        fprintf(stderr,"CALLBACK_STACK_VQ role=%s address=0x%llx result_bytes=%zu error=%lu region=0x%llx region_bytes=%zu allocation_base=0x%llx allocation_protect=%08lx state=%08lx protect=%08lx type=%08lx\n",
            labels[i],addresses[i],(size_t)count,query_error,(uintptr_t)info.BaseAddress,(size_t)info.RegionSize,
            (uintptr_t)info.AllocationBase,info.AllocationProtect,info.State,info.Protect,info.Type);
    }
    SetLastError(saved);
}
static LONG CALLBACK exceptions(EXCEPTION_POINTERS *exception) {
    EXCEPTION_RECORD *e=exception->ExceptionRecord;CONTEXT *r=exception->ContextRecord;
    if(e->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && e->NumberParameters>=2 && e->ExceptionInformation[0]==8 &&
       fp_image_code(&game,r->Rip)) {
        if(!self_target(r->Rip) && !lookup(r->Rip))stop("untranslated NX callback target=0x%llx",r->Rip);
        if(callback_depth>=CALLBACK_LIMIT)stop("callback nesting limit");
        fp_callback_stacks_reap();
        DWORD error=GetLastError();callback *frame=&callbacks[callback_depth++];
        if(!frame->stack) {
            frame->stack=fp_callback_stack_alloc(callback_depth-1,STACK_BYTES);
            DWORD old;if(frame->stack && !VirtualProtect(frame->stack,4096,PAGE_NOACCESS,&old))stop("callback stack guard");
        }
        if(!frame->stack)stop("callback host stack allocation");
        frame->resume=*r;cpu_from_context(&frame->cpu,r);
        frame->native_base=__readgsqword(0x08);frame->native_limit=__readgsqword(0x10);
        uint64_t committed_limit=0;
        if(!fp_native_stack_reservation(r->Rsp,frame->native_base,&frame->native_owner_low) ||
           !(committed_limit=fp_native_stack_commit_limit(r->Rsp,frame->native_base,frame->native_owner_low))) {
            callback_stack_identity_diagnostic(r,frame);stop("native callback stack reservation identity");
        }
        frame->native_limit=committed_limit;
        frame->guest_base=guest_stack_base;frame->guest_limit=guest_stack_limit;frame->guest_owner_low=guest_stack_owner_low;frame->depth=call_depth;frame->last_error=error;
        frame->previous_cpu=active_cpu;call_depth=0;
        guest_stack_base=frame->native_base;guest_stack_limit=frame->native_limit;guest_stack_owner_low=frame->native_owner_low;
        uint64_t top=(uintptr_t)frame->stack+STACK_BYTES;
        native_stack_bounds(top,(uintptr_t)frame->stack+4096);
        r->Rsp=top-40;r->Rip=(uintptr_t)callback_entry;r->Rcx=(uintptr_t)frame;
        InterlockedIncrement64(&callback_calls);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if(e->ExceptionCode==0x406d1388)return EXCEPTION_CONTINUE_EXECUTION;
    if(e->ExceptionCode==EXCEPTION_ACCESS_VIOLATION || e->ExceptionCode==EXCEPTION_ILLEGAL_INSTRUCTION || e->ExceptionCode==EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr,"HOST_FAULT code=%08lx pc=0x%llx address=0x%llx access=%llu last_target=0x%llx guest_rip=0x%llx guest_rsp=0x%llx depth=%d\n",
            e->ExceptionCode,r->Rip,e->NumberParameters>=2?e->ExceptionInformation[1]:0,e->NumberParameters?e->ExceptionInformation[0]:0,
            last_target,active_cpu?active_cpu->rip:0,active_cpu?active_cpu->rsp:0,call_depth);fflush(stderr);
        fault_registers(r);fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void initialize_hooks(void) {
    for(unsigned i=0;i<HLE_COUNT;++i) {
        uint64_t target=rd64(GVA(recomp_imports[i].slot));
        hooks[hook_count++]=(hook){target,recomp_imports[i].name};
        if(!strcmp(recomp_imports[i].name,"qsort"))initialize_qsort_callbacks(target);
    }
    fp_native_hooks_sort(hooks,hook_count);
}
static void call_tls(CPU *c,unsigned reason) {
    if(!game.tls_callbacks)return;
    for(unsigned i=0;i<64;++i) {
        uint64_t target=rd64(game.tls_callbacks+i*8);if(!target)return;
        if(!fp_image_code(&game,target) || (!self_target(target) && !lookup(target)))stop("invalid TLS callback");
        CPU saved=*c;c->rsp=(c->rsp-64)&~UINT64_C(15);c->rcx=(uintptr_t)game.base;c->rdx=reason;c->r8=0;
        uint64_t before=c->rsp;checked(before-8,72,1);
        push64(c,UINT64_C(0xe5ef0010));dispatch(c,target);
        if(c->rsp!=before)stop("TLS callback stack imbalance");*c=saved;
    }
    stop("unterminated TLS callbacks");
}

static DWORD WINAPI self_thread(void *parameter) {
    uint64_t native_base=__readgsqword(8),native_limit=__readgsqword(0x10),native_tls=__readgsqword(0x58);
    if(tls_ready || tls_attached)stop("new thread guest TLS was not isolated");
    uint64_t result=((uint64_t(*)(uint64_t))((char *)game.base+32))((uintptr_t)parameter);
    if(result!=(uintptr_t)parameter+21 || self_tls_thread!=1 || callback_depth || call_depth || active_cpu ||
       __readgsqword(8)!=native_base || __readgsqword(0x10)>native_limit || __readgsqword(0x58)!=native_tls)
        stop("new thread nested callback/TLS restoration");
    if(memcmp((void *)(uintptr_t)tls_vector[0],(void *)(uintptr_t)game.tls_start,16) || rd64(tls_vector[0]+16))
        stop("new thread TLS template/zero initialization");
    return 0;
}
#if defined(__clang__)
__attribute__((target("xsave")))
#endif
static uint64_t self_xgetbv_oracle(void) {return _xgetbv(0);}
static int self_xgetbv(void) {
    int64_t old_delta=g_image_delta;CPU *old_cpu=active_cpu;
    const uint64_t site=UINT64_C(0x141ae0d80);
    g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)site;
    memcpy(game.base,"\x0f\x01\xd0",3);
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=UINT64_C(0xfeca123400000000);before=c;
    int enabled=xgetbv_available();
    if(enabled) {
        uint64_t expected=self_xgetbv_oracle();
        if(fp_xgetbv(0)!=expected)stop("XGETBV native oracle mismatch");
        active_cpu=&c;frostpunk_unsupported(site,"xgetbv");
        if(c.rax!=(uint32_t)expected || c.rdx!=(uint32_t)(expected>>32))stop("XGETBV result zero extension");
        c.rax=before.rax;c.rdx=before.rdx;
        if(memcmp(&c,&before,sizeof(c)))stop("XGETBV full CPU/flags preservation");
    }
    c=before;c.rcx=1;CPU rejected=c;
    if(handle_xgetbv(&c,site,"xgetbv") || memcmp(&c,&rejected,sizeof(c)))stop("XGETBV selector gate");
    c=before;
    if(handle_xgetbv(NULL,site,"xgetbv") || handle_xgetbv(&c,site+1,"xgetbv") ||
       handle_xgetbv(&c,site,"other") || memcmp(&c,&before,sizeof(c)))stop("XGETBV site/context gate");
    memset(game.base,0,3);
    if(handle_xgetbv(&c,site,"xgetbv") || memcmp(&c,&before,sizeof(c)))stop("XGETBV original opcode gate");
    active_cpu=old_cpu;g_image_delta=old_delta;return enabled;
}
#if defined(__clang__)
__attribute__((target("sse4.1")))
#endif
static XMM self_pmovsxdq_oracle(XMM input) {
    __m128i native;memcpy(&native,&input,16);native=_mm_cvtepi32_epi64(native);
    XMM result;memcpy(&result,&native,16);return result;
}
static unsigned self_pmovsxdq(void) {
    int features[4];__cpuidex(features,1,0);
    if(!(features[2]&(1u<<19)))return 0;
    const uint32_t edge[]={0,1,UINT32_MAX,0x7fffffff,0x80000000,0x80000001,0xffff0000,0x0000ffff};
    int64_t old_delta=g_image_delta;CPU *old_cpu=active_cpu;unsigned tested=0;
    for(unsigned site=0;site<sizeof(pmovsxdq_sites)/sizeof(*pmovsxdq_sites);++site) {
        uint64_t va=pmovsxdq_sites[site],random=UINT64_C(0x91acfecb12345678);
        g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)va;memcpy(game.base,"\x66\x0f\x38\x25\xd1",5);
        for(unsigned i=0;i<4160;++i) {
            random^=random<<13;random^=random>>7;random^=random<<17;
            CPU c,expected;memset(&c,0xa5,sizeof(c));
            c.xmm[1].u32[0]=i<64?edge[i/8]:(uint32_t)random;
            c.xmm[1].u32[1]=i<64?edge[i%8]:(uint32_t)(random>>32);
            c.xmm[1].u64[1]=~random;expected=c;expected.xmm[2]=self_pmovsxdq_oracle(c.xmm[1]);
            active_cpu=&c;frostpunk_unsupported(va,"pmovsxdq xmm2, xmm1");
            if(memcmp(&c,&expected,sizeof(c)))stop("PMOVSXDQ differential/full CPU/flags mismatch");
            ++tested;
        }
        CPU c,before;memset(&c,0xa5,sizeof(c));before=c;
        if(handle_pmovsxdq(NULL,va,"pmovsxdq xmm2, xmm1") ||
           handle_pmovsxdq(&c,va+1,"pmovsxdq xmm2, xmm1") ||
           handle_pmovsxdq(&c,va,"pmovsxdq xmm1, xmm2") || memcmp(&c,&before,sizeof(c)))
            stop("PMOVSXDQ site/context/operand gate");
        memset(game.base,0,5);
        if(handle_pmovsxdq(&c,va,"pmovsxdq xmm2, xmm1") || memcmp(&c,&before,sizeof(c)))stop("PMOVSXDQ original opcode gate");
    }
    active_cpu=old_cpu;g_image_delta=old_delta;return tested;
}
#if defined(__clang__)
__attribute__((target("sse4.1")))
#endif
static uint32_t self_pextrd_oracle(XMM input,unsigned lane) {
    __m128i native;memcpy(&native,&input,16);
    switch(lane) {
    case 0:return (uint32_t)_mm_extract_epi32(native,0);
    case 1:return (uint32_t)_mm_extract_epi32(native,1);
    case 2:return (uint32_t)_mm_extract_epi32(native,2);
    default:return (uint32_t)_mm_extract_epi32(native,3);
    }
}
static unsigned self_pextrd(void) {
    int features[4];__cpuidex(features,1,0);if(!(features[2]&(1u<<19)))return 0;
    const uint32_t edge[]={0,1,UINT32_MAX,0x7fffffff,0x80000000,0x80000001,0xffff0000,0x0000ffff};
    int64_t old_delta=g_image_delta;CPU *old_cpu=active_cpu;unsigned tested=0;
    for(unsigned i=0;i<sizeof(pextrd_sites)/sizeof(*pextrd_sites);++i) {
        const pextrd_site *site=&pextrd_sites[i];
        unsigned modrm=(unsigned char)site->bytes[site->size-2];
        unsigned rex=site->size==7?(unsigned char)site->bytes[1]:0;
        if((modrm&0xc0)!=0xc0 || site->destination!=8*((modrm&7)+((rex&1)?8:0)) ||
           site->source!=((modrm>>3)&7) || site->lane!=((unsigned char)site->bytes[site->size-1]&3))stop("PEXTRD manifest operand decoding");
        uint64_t random=UINT64_C(0x8675309abcdf2468);
        g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)site->va;memcpy(game.base,site->bytes,site->size);
        for(unsigned j=0;j<4160;++j) {
            CPU c,expected;memset(&c,0xa5,sizeof(c));
            for(unsigned lane=0;lane<4;++lane) {
                random^=random<<13;random^=random>>7;random^=random<<17;
                c.xmm[site->source].u32[lane]=j<64?edge[(j+lane)%8]:(uint32_t)random;
            }
            expected=c;uint64_t value=self_pextrd_oracle(c.xmm[site->source],site->lane);
            memcpy((unsigned char *)&expected+site->destination,&value,sizeof(value));
            active_cpu=&c;frostpunk_unsupported(site->va,site->text);
            if(memcmp(&c,&expected,sizeof(c)))stop("PEXTRD differential/full CPU/flags mismatch");++tested;
        }
        CPU c,before;memset(&c,0xa5,sizeof(c));before=c;
        if(handle_pextrd(NULL,site->va,site->text) || handle_pextrd(&c,site->va+1,site->text) ||
           handle_pextrd(&c,site->va,"pextrd eax, xmm0, 0") || memcmp(&c,&before,sizeof(c)))stop("PEXTRD context/site/operand gate");
        for(unsigned byte=0;byte<site->size;++byte) {
            ((unsigned char *)game.base)[byte]^=1;
            if(handle_pextrd(&c,site->va,site->text) || memcmp(&c,&before,sizeof(c)))stop("PEXTRD original opcode gate");
            ((unsigned char *)game.base)[byte]^=1;
        }
        uint32_t old_size=game.sections[0].size;game.sections[0].size=site->size-1;
        if(handle_pextrd(&c,site->va,site->text) || memcmp(&c,&before,sizeof(c)))stop("PEXTRD section endpoint gate");
        game.sections[0].size=old_size;
    }
    active_cpu=old_cpu;g_image_delta=old_delta;return tested;
}
static int self_test(void) {
    fp_section section={0,4096,IMAGE_SCN_MEM_EXECUTE};
    game.base=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);game.size=4096;game.sections=&section;game.section_count=1;
    if(!game.base)stop("self-test NX allocation");self_test_mode=1;
    int xgetbv_checked=self_xgetbv();
    unsigned pmovsxdq_checked=self_pmovsxdq();
    unsigned pextrd_checked=self_pextrd();
    game.tls_start=(uintptr_t)game.base+256;game.tls_end=game.tls_start+16;game.tls_zero=16;
    game.tls_index=(uintptr_t)game.base+128;game.tls_callbacks=(uintptr_t)game.base+160;
    memset((void *)(uintptr_t)game.tls_start,0x5a,16);wr64(game.tls_callbacks,(uintptr_t)game.base+64);
    wr64((uintptr_t)guest_peb+0x10,(uintptr_t)game.base);
    uint64_t native_base=__readgsqword(8),native_limit=__readgsqword(0x10),native_tls=__readgsqword(0x58);
    tls_initialize();tls_attached=1;
    uint64_t initial_stack[32];CPU initial={0};initial.rsp=((uintptr_t)(initial_stack+24))&~UINT64_C(15);
    initial.mxcsr=0x1f80;call_tls(&initial,DLL_PROCESS_ATTACH);
    typedef uint64_t(*sum_fn)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
    uint64_t value=((sum_fn)game.base)(1,2,3,4,5,6,7,8,9,10,11,12);
    if(value!=78)stop("NX callback scalar result=%llu",value);
    int values[]={4,-9,12,0,4,-1};qsort(values,6,sizeof(int),(int(*)(const void*,const void*))((char*)game.base+16));
    int expected[]={-9,-1,0,4,4,12};if(memcmp(values,expected,sizeof(values)))stop("NX qsort callback ordering");
    if(frostpunk_teb_read(0x58,8)!=(uintptr_t)tls_vector || __readgsqword(0x58)!=native_tls)stop("guest TLS corrupted host TLS");
    uint64_t teb=frostpunk_teb_read(0x30,8);
    if(teb==(uintptr_t)NtCurrentTeb() || rd64(teb+0x30)!=teb || rd64(teb+0x60)!=(uintptr_t)guest_peb ||
       rd64(rd64(teb+0x60)+0x10)!=(uintptr_t)game.base || rd64(teb+0x58)!=(uintptr_t)tls_vector)
        stop("guest TEB self/PEB/static TLS identity");
    es3_ehframe_t sentinel,check;es3_eh_push(&sentinel,UINT64_C(0x141000000),NULL);
    CPU outer={0};active_cpu=&outer;call_depth=3;SetLastError(9876);
    value=((uint64_t(*)(uint64_t))((char *)game.base+32))(21);
    if(value!=42 || GetLastError()!=9876 || call_depth!=3 || active_cpu!=&outer || callback_depth ||
       __readgsqword(8)!=native_base || __readgsqword(0x10)>native_limit)stop("nested callback/native state restoration");
    es3_eh_push(&check,0,NULL);if(check.prev!=&sentinel)stop("callback lost outer guest EH head");
    es3_eh_pop(&check);es3_eh_pop(&sentinel);active_cpu=NULL;call_depth=0;
    ((uint64_t(*)(uint64_t))((char *)game.base+80))(4567);
    if(GetLastError()!=4567)stop("callback callee LastError propagation");
    CPU native={0};native.rsp=((uintptr_t)(initial_stack+24))&~UINT64_C(15);
    push64(&native,UINT64_C(0xe5ef0010));SetLastError(8765);dispatch(&native,(uintptr_t)GetLastError);
    if(native.rax!=8765 || GetLastError()!=8765)stop("native LastError input preservation");
    native.rcx=7654;push64(&native,UINT64_C(0xe5ef0010));dispatch(&native,(uintptr_t)SetLastError);
    if(GetLastError()!=7654)stop("native LastError result preservation");
    CPU diagnostic=native;diagnostic.rsp-=8;wr64(diagnostic.rsp,UINT64_C(0xe5ef0010));
    diagnostic.rcx=(uintptr_t)"host diagnostic ANSI";CPU snapshot=diagnostic;
    SetLastError(6543);native_diagnostic(&diagnostic,"OutputDebugStringA");
    if(GetLastError()!=6543 || memcmp(&diagnostic,&snapshot,sizeof(diagnostic)))stop("ANSI diagnostic CPU/LastError preservation");
    diagnostic.rcx=(uintptr_t)L"host diagnostic Unicode \x0416";snapshot=diagnostic;
    native_diagnostic(&diagnostic,"OutputDebugStringW");
    if(GetLastError()!=6543 || memcmp(&diagnostic,&snapshot,sizeof(diagnostic)))stop("Unicode diagnostic CPU/LastError preservation");
    diagnostic.rcx=17;native_diagnostic(&diagnostic,"exit");native_diagnostic(&diagnostic,"_cexit");
    if(GetLastError()!=6543)stop("exit diagnostic LastError preservation");
    unsigned char settings_context[0x40]={0},settings_application[0x30]={0},settings_config[0x150]={0};
    wr64((uintptr_t)settings_context+0x30,(uintptr_t)settings_application);
    wr64((uintptr_t)settings_application+0x28,(uintptr_t)settings_config);
    uint64_t settings=(uintptr_t)settings_config+0xb0;
    wr32(settings+0x1c,1);wr8(settings+0x20,0);wr32(settings+0x28,0);wr8(settings+0x2c,1);
    wr32(settings+0x30,7);wr32(settings+0x34,1280);wr32(settings+0x38,720);wr32(settings+0x3c,60);
    wr8(settings+0x90,1);wr32(settings+0x94,1280);wr32(settings+0x98,720);wr32(settings+0x9c,60);
    graphics_settings settings_copy;
    if(!graphics_settings_read((uintptr_t)settings_context,&settings_copy) || settings_copy.quality!=1 ||
       settings_copy.mode || !settings_copy.selected || settings_copy.index!=7 || settings_copy.width!=1280 ||
       settings_copy.height!=720 || settings_copy.refresh!=60 || !settings_copy.override_present ||
       settings_copy.override_width!=1280 || settings_copy.override_height!=720 || settings_copy.override_refresh!=60 ||
       graphics_settings_read(1,&settings_copy) || graphics_settings_read(UINT64_MAX-8,&settings_copy))
        stop("graphics settings bounded snapshot");
    SetLastError(6543);errno=EDOM;snapshot=diagnostic;
    graphics_settings_diagnostic("self_test",(uintptr_t)settings_context);
    graphics_settings_diagnostic("self_test_invalid",1);
    if(GetLastError()!=6543 || errno!=EDOM || memcmp(&diagnostic,&snapshot,sizeof(diagnostic)))
        stop("graphics settings diagnostic CPU/LastError/errno preservation");
    char gfx_path[4096];int complete;
    if(!gfx_path_read((uintptr_t)L"C:\\own\\GfXcOnFiG.ini",1,gfx_path,&complete) || !complete ||
       strcmp(gfx_path,"C:\\own\\GfXcOnFiG.ini") || gfx_path_read((uintptr_t)"another.ini",0,gfx_path,&complete) ||
       gfx_path_read(1,1,gfx_path,&complete))stop("scoped graphics path read");
    diagnostic.rcx=(uintptr_t)L"C:\\own\\GfxConfig.ini";diagnostic.rdx=(uintptr_t)"C:\\own\\gfxconfig.ini";snapshot=diagnostic;
    SetLastError(6543);errno=EDOM;native_diagnostic(&diagnostic,"CreateFileW");native_diagnostic(&diagnostic,"fopen_s");
    native_diagnostic(&diagnostic,"_wfopen");native_diagnostic(&diagnostic,"GetFileAttributesW");
    native_diagnostic(&diagnostic,"GetFileAttributesExW");
    diagnostic.rdx=diagnostic.rcx;snapshot=diagnostic;native_diagnostic(&diagnostic,"_wfopen_s");
    if(GetLastError()!=6543 || errno!=EDOM || memcmp(&diagnostic,&snapshot,sizeof(diagnostic)))
        stop("graphics file diagnostic CPU/LastError/errno preservation");
    wchar_t shell_path[MAX_PATH],native_shell_path[MAX_PATH];memset(shell_path,0xa5,sizeof(shell_path));
    diagnostic.rcx=0;diagnostic.rdx=(uintptr_t)shell_path;diagnostic.r8=0x1a;diagnostic.r9=0;snapshot=diagnostic;
    const char *shell_names[]={"SHGetFolderPathW","SHGetSpecialFolderPathW","SHGetKnownFolderPath"};
    for(unsigned i=0;i<3;++i) {
        hooks[0]=(hook){UINT64_C(0x12345678),shell_names[i]};hook_count=1;
        SetLastError(6543);
        if(hooked(&diagnostic,hooks[0].target,hook_name(hooks[0].target)) || memcmp(&snapshot,&diagnostic,sizeof(snapshot)) || GetLastError()!=6543)
            stop("system shell API must pass through unchanged");
    }
    SetLastError(6543);
    if(!SHGetSpecialFolderPathW(NULL,native_shell_path,CSIDL_APPDATA,FALSE))stop("system AppData query");
    SetLastError(6543);
    dispatch(&diagnostic,(uintptr_t)SHGetSpecialFolderPathW);
    if(!diagnostic.rax || wcscmp(shell_path,native_shell_path) || diagnostic.rsp!=snapshot.rsp+8)
        stop("system AppData native bridge");
    wcscpy_s(game_exe,32768,L"C:\\game path\\Frostpunk.exe");strcpy_s(game_exe_a,sizeof(game_exe_a),"C:\\game path\\Frostpunk.exe");
    const char *command_names[]={"GetCommandLineA","GetCommandLineW","_get_narrow_winmain_command_line"};
    const char *expected_commands[]={"\"C:\\game path\\Frostpunk.exe\"","\"C:\\game path\\Frostpunk.exe\" -gfxconfig",
        "\"C:\\game path\\Frostpunk.exe\" -novideo","\"C:\\game path\\Frostpunk.exe\" -gfxconfig -novideo"};
    const wchar_t *expected_wide_commands[]={L"\"C:\\game path\\Frostpunk.exe\"",L"\"C:\\game path\\Frostpunk.exe\" -gfxconfig",
        L"\"C:\\game path\\Frostpunk.exe\" -novideo",L"\"C:\\game path\\Frostpunk.exe\" -gfxconfig -novideo"};
    const char *expected_tails[]={"","-gfxconfig","-novideo","-gfxconfig -novideo"};
    for(unsigned mode=0;mode<4;++mode) {
        build_guest_command(mode&1,mode&2);
        if(strcmp(command_a,expected_commands[mode]) || wcscmp(command_w,expected_wide_commands[mode]) ||
           strcmp(winmain_command_a,expected_tails[mode]))stop("guest command line construction");
        wchar_t *test_args[]={L"fixture",L"--run",L"working",L"profile",NULL,NULL};int test_argc=4,gfx,video;
        if(mode&1)test_args[test_argc++]=L"--gfxconfig";
        if(mode&2)test_args[test_argc++]=L"--novideo";
        if(!guest_options(test_argc,test_args,&gfx,&video) || gfx!=(int)(mode&1) || video!=(int)!!(mode&2))
            stop("guest command line flag parsing");
        for(unsigned name=0;name<3;++name) {
            CPU actual=native;actual.rsp-=8;wr64(actual.rsp,UINT64_C(0xe5ef0010));CPU wanted=actual;
            wanted.rax=(uintptr_t)(name==0?(void*)command_a:name==1?(void*)command_w:(void*)winmain_command_a);
            wanted.rsp+=8;wanted.rip=UINT64_C(0xe5ef0010);hooks[0]=(hook){UINT64_C(0x12345678),command_names[name]};
            SetLastError(6543);
            if(!hooked(&actual,hooks[0].target,hook_name(hooks[0].target)) || memcmp(&actual,&wanted,sizeof(actual)) || GetLastError()!=6543)
                stop("guest command line hook CPU/LastError/single pop");
        }
    }
    wchar_t *reversed[]={L"fixture",L"--run",L"working",L"profile",L"--novideo",L"--gfxconfig"};int gfx,video;
    if(!guest_options(6,reversed,&gfx,&video) || !gfx || !video)stop("guest option order");
    const wchar_t *invalid[]={L"--gfxconfig",L"--novideo",L"--unknown"};
    for(unsigned i=0;i<3;++i) {
        wchar_t *bad[]={L"fixture",L"--run",L"working",L"profile",(wchar_t*)invalid[i],(wchar_t*)invalid[i]};
        if(guest_options(i==2?5:6,bad,&gfx,&video))stop("duplicate/unknown guest option accepted");
    }
    if(guest_options(3,reversed,&gfx,&video) || guest_options(7,reversed,&gfx,&video))stop("guest option count");
    game_exe[0]=game_exe_a[0]=0;build_guest_command(0,0);hook_count=0;profile[0]=0;
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,self_thread,(void *)(uintptr_t)(100+i),0,NULL);if(!threads[i])stop("self-test CreateThread");}
    if(WaitForMultipleObjects(4,threads,TRUE,10000)!=WAIT_OBJECT_0)stop("self-test threads timeout");
    for(unsigned i=0;i<4;++i){DWORD status;if(!GetExitCodeThread(threads[i],&status)||status)stop("self-test thread result");CloseHandle(threads[i]);}
    fp_callback_stacks_reap();
    if(self_tls_attach!=4 || self_tls_process!=1 || self_nested!=5 || self_tls_thread)stop("TLS process/thread callback cardinality");
    fprintf(stderr,"WINDOWS_HOST_SELF_TEST passed NX_callback_args12=1 qsort_callbacks=1 alternate_stacks=1 host_TLS_preserved=1 guest_TEB_PEB_identity=1 nested_native_callbacks=5 guest_EH_boundary_restored=1 lazy_TLS_threads=4 process_attach=1 LastError_input_output=1 guest_command_alias_cases=12 guest_flags_independent=1 guest_duplicate_unknown_rejected=1 xgetbv_XCR0=%d pmovsxdq_native_cases=%u pextrd_native_cases=%u full_CPU_flags=1 callbacks=%lld original_bytes_executed=0\n",xgetbv_checked,pmovsxdq_checked,pextrd_checked,callback_calls);
    direct_image_reset();VirtualFree(game.base,0,MEM_RELEASE);memset(&game,0,sizeof(game));return 0;
}

int wmain(int argc,wchar_t **argv) {
    setvbuf(stderr,NULL,_IONBF,0);started=GetTickCount64();
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    fp_native_validation_init();
    if(!fp_callback_stacks_init())stop("callback stack cleanup initialization error=%lu",GetLastError());
    if(!fp_simd_float_init())stop("floating SIMD initialization");
    if(!AddVectoredExceptionHandler(1,exceptions))stop("AddVectoredExceptionHandler");
    if(argc==2 && !wcscmp(argv[1],L"--self-test"))return self_test();
    int gfxconfig,novideo;
    if(!guest_options(argc,argv,&gfxconfig,&novideo) || (wcscmp(argv[1],L"--inspect") && wcscmp(argv[1],L"--run"))) {
        fputs("Usage: Frostpunk.exe --self-test | --inspect|--run <working-copy-exe> <diagnostic-directory> [--gfxconfig] [--novideo]\n",stderr);return 64;
    }
    if(!GetFullPathNameW(argv[2],32768,game_exe,NULL) || !GetFullPathNameW(argv[3],32768,profile,NULL))stop("absolute paths");
    wchar_t directory[32768];wcscpy_s(directory,32768,game_exe);wchar_t *slash=wcsrchr(directory,L'\\');if(!slash)stop("EXE directory");*slash=0;
    if(!SetCurrentDirectoryW(directory) || !SetDllDirectoryW(directory))stop("game working directory");
    if(!SetEnvironmentVariableW(L"SteamAppPath",directory) || !SetEnvironmentVariableW(L"SteamAppId",L"323190") ||
       !SetEnvironmentVariableW(L"SteamGameId",L"323190"))stop("offline Steam environment");
    CreateDirectoryW(profile,NULL);
    if(!SetEnvironmentVariableW(L"FROSTPUNK_CAPTURE_DIRECTORY",profile))stop("diagnostic capture directory");
    WideCharToMultiByte(CP_ACP,0,game_exe,-1,game_exe_a,sizeof(game_exe_a),NULL,NULL);
    build_guest_command(gfxconfig,novideo);
    fprintf(stderr,"WINDOWS_GUEST_COMMAND %s\n",command_a);
    direct_image_reset();
    if(!fp_load_image(game_exe,&game))return 74;
    g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)game.preferred_base;
    if(g_image_delta)stop("preferred guest address occupied; full relocation not validated");
    if(!fp_extra_aot_init(&game))stop("extra AOT original opcode mismatch");
    if(!fp_shader_lookup_init(&game))stop("shader lookup original opcode mismatch");
    direct_image_init();
    fp_windows_input_init((uintptr_t)game.base);
    es3_eh_image_base=(uintptr_t)game.base;
    memcpy(guest_peb,(void *)(uintptr_t)__readgsqword(0x60),sizeof(guest_peb));wr64((uintptr_t)guest_peb+0x10,(uintptr_t)game.base);
    if(game.tls_index)wr32(game.tls_index,0);tls_initialize();tls_attached=1;initialize_hooks();
    fprintf(stderr,"WINDOWS_PE_READY base=0x%llx bytes=%zu static_entries=%u image_NX=1 original_bytes_executed=0 native_D3D11=1\n",(uintptr_t)game.base,game.size,recomp_dispatch_count);
    if(!wcscmp(argv[1],L"--inspect"))return 0;
    fp_performance_init();
    char trace[8];trace_calls=GetEnvironmentVariableA("FROSTPUNK_TRACE_DISPATCH",trace,sizeof(trace)) && !strcmp(trace,"1");
    void *stack=VirtualAlloc(NULL,STACK_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!stack)stop("guest stack allocation");
    guest_stack_limit=(uintptr_t)stack;guest_stack_owner_low=(uintptr_t)stack;guest_stack_base=(uintptr_t)stack+STACK_BYTES;
    CPU c={0};c.rsp=(guest_stack_base-512)&~UINT64_C(15);c.mxcsr=0x1f80;call_tls(&c,DLL_PROCESS_ATTACH);
    push64(&c,UINT64_C(0xe5ef0010));
    fputs("STATIC_CRT_ENTRY target=0x141ae13e0 original_wrapper_skipped=1\n",stderr);
    dispatch(&c,GVA(0x141ae13e0));
    fprintf(stderr,"STATIC_ENTRY_RETURN rax=%llu callbacks=%lld elapsed_ms=%llu\n",c.rax,callback_calls,GetTickCount64()-started);
    return (int)c.rax;
}
