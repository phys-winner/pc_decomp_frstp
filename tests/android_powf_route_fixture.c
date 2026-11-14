/* Paired exact current/candidate routers with real stdio/string/math. */
#include "win64_math.h"
static int routed_math(CPU*,const char*);
#define main unused_previous_crt_main
#include "android_crt_hotpath_fixture.c"
#undef main
#if defined(_WIN32)
#include <xmmintrin.h>
#endif

static unsigned math_calls,force_decline;
static CPU negative_initial;
static uint64_t negative_environment[2];
static int negative_active;
uint64_t win64_math_error_handler(void){return 0;}
static int routed_math(CPU*c,const char*name){++math_calls;return force_decline?0:win64_math_call(c,name);}
static void environment(uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(value[0]),"=r"(value[1]));
#else
    value[0]=_mm_getcsr();value[1]=0;
#endif
}
static void set_environment(const uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
_Noreturn void powf_route_fixture_stop(int code){
    uint64_t now[2];environment(now);
    check(negative_active&&active_cpu&&!memcmp(active_cpu,&negative_initial,sizeof(CPU)),"negative whole CPU unchanged");
    check(active_cpu->rsp==original_rsp&&registrations==4,"negative cold FILE registration and no RSP pop");
    check(!memcmp(now,negative_environment,sizeof(now)),"negative native FP preservation");
    fprintf(stderr,"POWF_ROUTE_NEGATIVE full_CPU=1 registrations=4 RSP_unpopped=1 FP_preserved=1\n");
    _Exit(code);
}
static CPU input(uint32_t x,uint32_t y,unsigned mode,unsigned pattern){
    CPU c;memset(&c,(int)(0xa5u^pattern*37u),sizeof(c));c.rsp=UINT64_C(0x123456789abcd000);
    c.xmm[0].u32[0]=x;c.xmm[1].u32[0]=y;
    c.mxcsr=0x1f80u|((mode&3u)<<13)|(mode&4u?64u:0)|(mode&8u?32768u:0)|(pattern&1u?0x15u:0);
    return c;
}
static void native_pattern(unsigned pattern,uint64_t saved[2]){
    uint64_t value[2];environment(saved);value[0]=saved[0];value[1]=saved[1];
#if defined(__aarch64__)
    value[0]=(value[0]&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|
        ((uint64_t)(pattern&3u)<<22)|(UINT64_C(1)<<24);
    value[1]=0x15;
#else
    value[0]=0x9f95u|((pattern&3u)<<13);
#endif
    set_environment(value);
}
static uint64_t pairs;
static void pair_math(const char*dll,CPU c,int cold){
    uint64_t native[2],after[2];environment(native);
    math_calls=0;Result a=invoke(0,dll,"powf",c);environment(after);
    check(math_calls==1&&!memcmp(native,after,sizeof(native)),"baseline math/native FP");
    math_calls=0;Result b=invoke(1,dll,"powf",c);environment(after);
    check(math_calls==1&&!memcmp(native,after,sizeof(native)),"candidate math/native FP");
    check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU)),"paired whole CPU");
    check(a.native_errno==b.native_errno&&a.native_errno==E2BIG&&a.crt_errno==b.crt_errno&&
        a.crt_errno==31337&&a.error==b.error&&a.error==0x51cafe,"paired native/guest errno and LastError");
    CPU expected=c;expected.rsp+=8;expected.xmm[0].u32[0]=b.cpu.xmm[0].u32[0];expected.mxcsr=b.cpu.mxcsr;
    check(!memcmp(&expected,&b.cpu,sizeof(CPU)),"only XMM0low/MXCSR/RSP change");
    check(a.negative_providers>0&&b.negative_providers==0,"candidate skips pure provider declines");
    check(!memcmp(a.bytes,b.bytes,sizeof(a.bytes)),"unrelated memory unchanged");
    if(cold)check(a.registration_count==4&&b.registration_count==4,"first cold FILE registration preserved");
    ++pairs;
}
int main(int argc,char**argv){
    const char*dlls[]={"ucrtbase.dll","msvcrt.dll","api-ms-win-crt-math-l1-1-0.dll","vcruntime140.dll","UcRtBaSe.DlL"};
    win64_region_add((uintptr_t)destination,sizeof(destination),1);win64_region_add((uintptr_t)source,sizeof(source),1);
    memcpy(source,"Frostpunk shader family",23);source[23]=0;
    if(argc==4&&!strcmp(argv[1],"--negative")){
        CPU c=input(0x40000000,0x3f2aaaab,0,0);const char*name=argv[3];
        if(!strcmp(name,"precision"))c.mxcsr&=~4096u;
        else if(!strcmp(name,"pole")){c.xmm[0].u32[0]=0;c.xmm[1].u32[0]=0xbf800000;}
        else if(!strcmp(name,"negative"))c.xmm[0].u32[0]=0xbf800000;
        else if(!strcmp(name,"subnormal_base"))c.xmm[0].u32[0]=1;
        else if(!strcmp(name,"subnormal_power"))c.xmm[1].u32[0]=1;
        else if(!strcmp(name,"NaN"))c.xmm[0].u32[0]=0x7fc12345;
        else if(!strcmp(name,"infinity"))c.xmm[1].u32[0]=0x7f800000;
        else if(!strcmp(name,"lower_band"))c.xmm[1].u32[0]=0xc2fc0000;
        else if(!strcmp(name,"upper_band"))c.xmm[1].u32[0]=0x42ff0000;
        else if(!strcmp(name,"underflow"))c.xmm[1].u32[0]=0xc3160000;
        else if(!strcmp(name,"overflow"))c.xmm[1].u32[0]=0x43000000;
        else return 2;
        negative_active=1;negative_initial=c;environment(negative_environment);
        (void)invoke(!strcmp(argv[2],"candidate"),dlls[2],"powf",c);return 99;
    }
    CPU cold=input(0x3fc00000,0x40000000,0,0);
    if(argc==3&&!strcmp(argv[1],"--cold")){
        pair_math(argv[2],cold,1);aliases();
        printf("{\"success\":true,\"cold_DLL\":\"%s\",\"FILE_registrations_each\":4,\"readonly_and_writable_aliases\":true,\"whole_CPU_errno_FP\":true}\n",argv[2]);return 0;
    }
    if(argc!=1)return 2;
    pair_math(dlls[0],cold,1);
    /* Reuse real FILE writable/readonly aliases and current memcpy route. */
    aliases();
    const uint32_t xs[]={0,0x80000000,0x3e800000,0x3f000000,0x3f800000,0x40000000,
        0x40800000,0x41000000,0x41800000,0x42c80000,0x3f400000,0x3fc00000};
    const uint32_t ys[]={0,0x80000000,0x3f800000,0xbf800000,0x40000000,0xc0000000,
        0x3f000000,0xbf000000,0x40400000,0xc0400000,0x40800000,0xc0800000};
    for(unsigned x=0;x<12;++x)for(unsigned y=0;y<12;++y){
        if(!(xs[x]&0x7fffffffu)&&(ys[y]&0x7fffffffu)&&(ys[y]>>31))continue;
        for(unsigned mode=0;mode<16;++mode)for(unsigned pattern=0;pattern<2;++pattern){
            uint64_t saved[2];native_pattern(x+y+mode+pattern,saved);
            pair_math(dlls[(x+y+mode+pattern)%5],input(xs[x],ys[y],mode,pattern),0);set_environment(saved);
        }
    }
    unsigned declines_checked=0;
    const char*names[]={"powff","Powf","POWF","__powf"};
    for(unsigned i=0;i<5;++i)for(unsigned variant=0;variant<2;++variant){
        CPU c=input(0x40000000,0x40000000,0,0),before=c;errno=E2BIG;guest_errno=31337;
        const char*dll=i==4?"unrelated.dll":dlls[0];const char*name=i==4?"powf":names[i];
        int yes=variant?candidate_route(&c,dll,name):baseline_route(&c,dll,name);
        check(!yes&&!memcmp(&before,&c,sizeof(CPU))&&errno==E2BIG&&guest_errno==31337,"nonmatching exactname/DLL decline");++declines_checked;
    }
    force_decline=1;
    for(unsigned variant=0;variant<2;++variant){CPU c=cold,before=c;errno=E2BIG;guest_errno=31337;
        int yes=variant?candidate_route(&c,dlls[2],"powf"):baseline_route(&c,dlls[2],"powf");
        check(!yes&&!memcmp(&before,&c,sizeof(CPU))&&errno==E2BIG&&guest_errno==31337,"negative math-provider no-pop decline");++declines_checked;}
    force_decline=0;
    const unsigned iterations=100000;uint64_t elapsed[2];
    for(unsigned variant=0;variant<2;++variant){uint64_t begin=nanos();
        for(unsigned i=0;i<iterations;++i){CPU c=input(0x3fc00000,0x3f000000,0,i&1);
            int yes=variant?candidate_route(&c,dlls[2],"powf"):baseline_route(&c,dlls[2],"powf");check(yes,"benchmark route");}
        elapsed[variant]=nanos()-begin;
    }
    printf("{\"benchmark_calls_each\":%u,\"baseline_ns\":%" PRIu64 ",\"candidate_ns\":%" PRIu64 ",\"unrelated_providers_stubbed\":true,\"FPS_or_startup_gain_proven\":false}\n",iterations,elapsed[0],elapsed[1]);
    printf("{\"success\":true,\"paired_powf_calls\":%" PRIu64 ",\"negative_provider_cases\":%u,\"whole_CPU_XMM_upper_RSP\":true,\"native_FP_errno_LastError\":true,\"first_FILE_registration\":true,\"FILE_aliases\":true,\"math_algorithm_unchanged\":true,\"unrelated_providers_stubbed\":true,\"production_edited\":false}\n",pairs,declines_checked);
    return 0;
}
