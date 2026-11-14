#include "cpu64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void L_000140DFA630(CPU *);
void fp_original_L_000140DFA630(CPU *);
static unsigned char arena[4096],before[4096],expected[4096];
static CPU current,initial,reference;
static uint64_t seed=UINT64_C(0x4d61747269782d31),cases;
static uint32_t random32(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return (uint32_t)seed;}
static uint64_t fpcr(void){
#ifdef __aarch64__
    uint64_t value;__asm__ volatile("mrs %0, fpcr":"=r"(value));return value;
#else
    return 0;
#endif
}
static uint64_t fpsr(void){
#ifdef __aarch64__
    uint64_t value;__asm__ volatile("mrs %0, fpsr":"=r"(value));return value;
#else
    return 0;
#endif
}
static void floating(uint64_t control,uint64_t status){
#ifdef __aarch64__
    __asm__ volatile("msr fpcr, %0\nmsr fpsr, %1\nisb"::"r"(control),"r"(status):"memory");
#else
    (void)control;(void)status;
#endif
}
static void check(unsigned mode,unsigned kind,unsigned iteration){
    static const uint32_t special[]={0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
        0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,
        0x7fc12345,0xffc54321,0x7f812345,0xff854321};
    for(size_t i=0;i<sizeof(arena);i+=4){uint32_t value=random32();
        if(iteration<16)value=special[(iteration+i/4)%16];
        else if(iteration&1)value=(value&UINT32_C(0x807fffff))|UINT32_C(0x3f000000);
        memcpy(arena+i,&value,4);
    }
    for(size_t i=0;i<sizeof(current);++i)((unsigned char *)&current)[i]=(unsigned char)random32();
    uint64_t left=(uintptr_t)arena+128+(iteration&15),right=(uintptr_t)arena+512+(iteration&15);
    uint64_t output=(uintptr_t)arena+1024+(iteration&15),stack=(uintptr_t)arena+3584;
    switch(kind){
    case 1:output=left;break;
    case 2:output=right;break;
    case 3:output=right+4;break;
    case 4:output=right-4;break;
    case 5:output=left+16;break;
    case 6:right=left+4;break;
    case 7:left=stack-56;break;
    case 8:right=stack-40;break;
    case 9:output=stack-56;break;
    case 10:output=(uintptr_t)&current.xmm[9];break;
    case 11:left=(uintptr_t)&current.xmm[9];break;
    case 12:right=(uintptr_t)&current.xmm[9];break;
    case 13:output=right+1;break;
    case 14:output=right-1;break;
    case 15:output=right+15;break;
    case 16:output=right+63;break;
    case 17:output=right-63;break;
    case 18:output=left+1;break;
    case 19:left=right+1;break;
    case 20:output=(uintptr_t)&current.xmm[9]+1;break;
    default:break;
    }
    current.rcx=output;current.rdx=left;current.r8=right;current.rsp=stack;
    current.mxcsr=UINT32_C(0x1f80)|((mode&3)<<13)|((mode&4)?64:0)|((mode&8)?32768:0);
    initial=current;memcpy(before,arena,sizeof(arena));
    uint64_t control=((uint64_t)(mode&3)<<22)|((mode&4)?UINT64_C(1)<<24:0)|((mode&8)?UINT64_C(1)<<25:0);
    floating(control,0);fp_original_L_000140DFA630(&current);uint64_t old_status=fpsr();
    reference=current;memcpy(expected,arena,sizeof(arena));
    current=initial;memcpy(arena,before,sizeof(arena));
    floating(control,0);L_000140DFA630(&current);uint64_t new_status=fpsr();
    if(memcmp(&current,&reference,sizeof(current))||memcmp(arena,expected,sizeof(arena))||old_status!=new_status){
        floating(0,0);
        size_t cpu_at=0,memory_at=0;
        while(cpu_at<sizeof(current)&&((unsigned char *)&current)[cpu_at]==((unsigned char *)&reference)[cpu_at])++cpu_at;
        while(memory_at<sizeof(arena)&&arena[memory_at]==expected[memory_at])++memory_at;
        fprintf(stderr,"MATRIX_MISMATCH mode=%u kind=%u iteration=%u cpu_offset=%zu memory_offset=%zu fpsr=%llx/%llx\n",
            mode,kind,iteration,cpu_at,memory_at,(unsigned long long)old_status,(unsigned long long)new_status);
        if(cpu_at<sizeof(current))fprintf(stderr,"CPU bytes expected=%02x actual=%02x\n",((unsigned char *)&reference)[cpu_at],((unsigned char *)&current)[cpu_at]);
        exit(1);
    }
    ++cases;
}
static uint64_t cpu_ns(void){
#ifdef _WIN32
    return (uint64_t)clock()*UINT64_C(1000000000)/CLOCKS_PER_SEC;
#else
    struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return (uint64_t)t.tv_sec*UINT64_C(1000000000)+t.tv_nsec;
#endif
}
static double measure(void (*fn)(CPU *),unsigned count){
    uint64_t stack=(uintptr_t)arena+3584;
    uint64_t start=cpu_ns();
    for(unsigned i=0;i<count;++i){current.rsp=stack;fn(&current);}
    return (double)(cpu_ns()-start)/count;
}
int main(int argc,char **argv){
    uint64_t saved_control=fpcr(),saved_status=fpsr();
    if(argc>1&&!strcmp(argv[1],"--benchmark")){
        floating(0,0);memset(&current,0,sizeof(current));memset(arena,0,sizeof(arena));
        for(unsigned i=0;i<32;++i){float value=(float)(i+1)/32;memcpy(arena+128+i*4,&value,4);memcpy(arena+512+i*4,&value,4);}
        current.rcx=(uintptr_t)arena+1024;current.rdx=(uintptr_t)arena+128;current.r8=(uintptr_t)arena+512;
        measure(fp_original_L_000140DFA630,10000);measure(L_000140DFA630,10000);
        printf("{\"success\":true,\"pairs\":[");
        for(unsigned i=0;i<8;++i){double old_time,new_time;
            if(i&1){new_time=measure(L_000140DFA630,200000);old_time=measure(fp_original_L_000140DFA630,200000);}
            else{old_time=measure(fp_original_L_000140DFA630,200000);new_time=measure(L_000140DFA630,200000);}
            printf("%s{\"original_ns\":%.3f,\"native_ns\":%.3f}",i?",":"",old_time,new_time);
        }
        puts("]}");
    }else{
        unsigned modes=1;const char *fp_status_checked="false";
#ifdef __aarch64__
        modes=16;fp_status_checked="true";
#endif
        for(unsigned mode=0;mode<modes;++mode)for(unsigned kind=0;kind<21;++kind)
            for(unsigned iteration=0;iteration<512;++iteration)check(mode,kind,iteration);
        printf("{\"success\":true,\"cases\":%llu,\"alias_kinds\":21,\"native_fp_modes\":%u,\"full_cpu_and_memory\":true,\"native_fp_status\":%s}\n",(unsigned long long)cases,modes,fp_status_checked);
    }
    floating(saved_control,saved_status);return 0;
}
