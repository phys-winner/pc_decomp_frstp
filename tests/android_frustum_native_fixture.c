#include "cpu64.h"
#include "aot_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <xmmintrin.h>
#endif
void L_000140E2C890(CPU *);
void fp_frustum_original(CPU *);
int64_t g_image_delta;
static unsigned char arena[2048],before[2048],expected[2048];
static CPU current,initial,reference;
static uint64_t seed=UINT64_C(0x6672757374756d31),cases;
static uint32_t random32(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return(uint32_t)seed;}
static uint64_t fpcr(void){
#ifdef __aarch64__
    uint64_t v;__asm__ volatile("mrs %0,fpcr":"=r"(v));return v;
#else
    return _mm_getcsr();
#endif
}
static uint64_t fpsr(void){
#ifdef __aarch64__
    uint64_t v;__asm__ volatile("mrs %0,fpsr":"=r"(v));return v;
#else
    return _mm_getcsr();
#endif
}
static void floating(uint64_t control,uint64_t status){
#ifdef __aarch64__
    __asm__ volatile("msr fpcr,%0\nmsr fpsr,%1\nisb"::"r"(control),"r"(status):"memory");
#else
    (void)control;_mm_setcsr((unsigned)status);
#endif
}
static void check(unsigned mode,unsigned kind,unsigned iteration){
    static const uint32_t special[]={0,0x80000000,1,0x80000001,0x007fffff,0x00800000,0x3f800000,0xbf800000,
      0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0xffc54321,0x7f812345,0xff854321};
    for(unsigned i=0;i<sizeof(arena);i+=4){uint32_t bits=random32();
        if(iteration<16)bits=special[(iteration+i/4)%16];
        else if(iteration&1)bits=(bits&0x807fffffu)|0x3f000000u;
        memcpy(arena+i,&bits,4);
    }
    for(unsigned i=0;i<sizeof(current);++i)((unsigned char*)&current)[i]=(unsigned char)random32();
    current.rcx=(uintptr_t)arena+128+(iteration&15);current.rdx=(uintptr_t)arena+512+(iteration&15);
    current.rsp=random32()|((uint64_t)random32()<<32);current.mxcsr=0x1f80u|((mode&3)<<13)|(mode&4?64:0)|(mode&8?32768:0)|(iteration&63);
    if(kind==1)current.rdx=current.rcx;
    else if(kind==2)current.rdx=current.rcx+1;
    else if(kind==3)current.rcx=(uintptr_t)&current.xmm[0];
    else if(kind==4)current.rdx=(uintptr_t)&current.xmm[0];
    else if(kind==5)current.rdx=(uintptr_t)&current.xmm[4]+1;
    else if(kind>=6){
        float box[8]={-1,-1,-1,1,1,1,1,1};float planes[24]={0};
        for(unsigned p=0;p<6;++p)planes[p*4+3]=1;
        if(kind<=8)planes[(kind-6)*8+3]=-2;
        memcpy((void*)(uintptr_t)current.rcx,box,sizeof(box));memcpy((void*)(uintptr_t)current.rdx,planes,sizeof(planes));
    }
    initial=current;memcpy(before,arena,sizeof(arena));
    uint64_t ctl=0,st=0;
#ifdef __aarch64__
    ctl=((uint64_t)(mode&3)<<22)|(mode&4?UINT64_C(1)<<24:0)|(mode&8?UINT64_C(1)<<25:0);st=0x15;
#else
    ctl=st=0x1f80;
#endif
    errno=1797;floating(ctl,st);fp_frustum_original(&current);uint64_t old_status=fpsr();int old_errno=errno;reference=current;memcpy(expected,arena,sizeof(arena));
    current=initial;memcpy(arena,before,sizeof(arena));
    errno=1797;floating(ctl,st);L_000140E2C890(&current);uint64_t new_status=fpsr();int new_errno=errno;
    if(memcmp(&current,&reference,sizeof(current))||memcmp(arena,expected,sizeof(arena))||old_status!=new_status||old_errno!=new_errno){
        floating(0,0x1f80);size_t at=0;
        while(at<sizeof(current)&&((unsigned char*)&current)[at]==((unsigned char*)&reference)[at])++at;
        fprintf(stderr,"FRUSTUM_MISMATCH mode=%u kind=%u iteration=%u cpu_offset=%zu fpsr=%llx/%llx\n",mode,kind,iteration,at,
          (unsigned long long)old_status,(unsigned long long)new_status);
        if(at<sizeof(current))fprintf(stderr,"expected=%02x actual=%02x\n",((unsigned char*)&reference)[at],((unsigned char*)&current)[at]);
        exit(1);
    }
    ++cases;
}
static uint64_t cpu_ns(void){
#ifdef _WIN32
    return(uint64_t)clock()*1000000000/CLOCKS_PER_SEC;
#else
    struct timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
#endif
}
static double measure(void(*fn)(CPU*),unsigned kind){
    memset(&current,0,sizeof(current));current.mxcsr=0x1f80;float box[8]={-1,-1,-1,1,1,1,1,1},planes[24]={0};
    for(unsigned i=0;i<6;++i){planes[i*4]=.5f;planes[i*4+3]=1;}
    if(kind<3)planes[kind*8+3]=-3;
    memcpy(arena+128,box,sizeof(box));memcpy(arena+512,planes,sizeof(planes));
    uint64_t start=cpu_ns();
    for(unsigned i=0;i<200000;++i){current.rcx=(uintptr_t)arena+128;current.rdx=(uintptr_t)arena+512;fn(&current);}
    return(double)(cpu_ns()-start)/200000;
}
int main(int argc,char**argv){
    if(argc<2)return 2;
    FILE*f=fopen(argv[1],"rb");if(!f||fseek(f,0,SEEK_END))abort();long size=ftell(f);rewind(f);
    void*image=malloc((size_t)size);if(!image||fread(image,1,(size_t)size,f)!=(size_t)size||fclose(f)||!fp_aot_cpu_init(image,(size_t)size))abort();
    free(image);uint64_t saved_control=fpcr(),saved_status=fpsr();
    if(argc>2&&!strcmp(argv[2],"--benchmark")){
        floating(0,0x1f80);printf("{\"success\":true,\"pairs\":[");
        for(unsigned i=0;i<8;++i)for(unsigned kind=0;kind<4;++kind){double a,b;
            if(i&1){b=measure(L_000140E2C890,kind);a=measure(fp_frustum_original,kind);}
            else{a=measure(fp_frustum_original,kind);b=measure(L_000140E2C890,kind);}
            printf("%s{\"kind\":%u,\"original_ns\":%.3f,\"native_ns\":%.3f}",i||kind?",":"",kind,a,b);
        }puts("]}");
    }else{
        unsigned modes=1;
#ifdef __aarch64__
        modes=16;
#endif
        for(unsigned mode=0;mode<modes;++mode)for(unsigned kind=0;kind<10;++kind)
            for(unsigned iteration=0;iteration<512;++iteration)check(mode,kind,iteration);
        printf("{\"success\":true,\"cases\":%llu,\"fp_modes\":%u,\"full_cpu_and_memory\":true,\"native_fp_status\":true,\"native_errno\":true,\"production_packed_compare\":true}\n",(unsigned long long)cases,modes);
    }
    floating(saved_control,saved_status);return 0;
}
