#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if defined(_WIN32)
#include <xmmintrin.h>
#endif
static unsigned char *allowed;
static size_t allowed_bytes;
void win64_stdio_set_errno(int value){(void)value;abort();}
uint64_t win64_math_error_handler(void){return 0;}
void *win64_pointer(uint64_t address,size_t bytes,int writable){
    if(address!=(uintptr_t)allowed||!allowed||bytes!=4||allowed_bytes!=4||writable){
        fputs("STOP fdtest input span\n",stderr);exit(73);
    }
    errno=999;return allowed;
}
static int import_math(CPU *c,const char *name){int crt=1;
#include "fdtest_route.inc"
    return 0;
}
static void environment(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
int main(int argc,char **argv){
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"--null")||!strcmp(argv[1],"--short")){
        uint32_t input=0;CPU c={0};allowed=(unsigned char*)&input;allowed_bytes=3;
        c.rcx=!strcmp(argv[1],"--null")?0:(uintptr_t)allowed;
        win64_math_call(&c,"_fdtest");return 99;
    }
    FILE *file=fopen(argv[1],"rb");if(!file)return 3;
    uint32_t row[2];unsigned long cases=0;
    while(fread(row,sizeof(row),1,file)==1){
        for(unsigned mode=0;mode<16;++mode){
            unsigned char storage[12],before[12];memset(storage,0x5d,sizeof(storage));
            allowed=storage+4+(mode&3);allowed_bytes=4;memcpy(allowed,row,4);memcpy(before,storage,12);
            CPU c,expected;memset(&c,0xa7,sizeof(c));c.rcx=(uintptr_t)allowed;
            c.mxcsr=(((cases>>4)&63u)<<7)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u)|0x15u;
            expected=c;expected.rax=row[1];if(mode&1)expected.rsp+=8;
            uint64_t old[2],now[2];environment(old);errno=2718;
            int handled=mode&1?import_math(&c,"_fdtest"):win64_math_call(&c,"_fdtest");
            int native_errno=errno;environment(now);
            if(!handled||memcmp(&c,&expected,sizeof(c))||memcmp(storage,before,12)||native_errno!=2718||memcmp(old,now,sizeof(old))){
                fprintf(stderr,"FAIL fdtest bits=%08x mode=%u result=%llx expected=%x\n",row[0],mode,(unsigned long long)c.rax,row[1]);return 4;
            }
            ++cases;
        }
    }
    if(ferror(file)||!feof(file)||!cases)return 5;fclose(file);
    allowed=NULL;allowed_bytes=0;
    for(unsigned mode=0;mode<1024;++mode){
        CPU c,expected;memset(&c,0xa7,sizeof(c));c.rcx=mode&1?UINT64_MAX:0;
        c.mxcsr=((mode&63u)<<7)|(((mode>>6)&3u)<<13)|((mode&256u)?64u:0u)|((mode&512u)?32768u:0u)|0x15u;
        expected=c;expected.xmm[0].u64[0]=UINT64_C(0x7ff8000000000000);expected.xmm[0].u64[1]=0;
        if(mode&1)expected.rsp+=8;
        uint64_t before[2],after[2];environment(before);errno=2718;
        int handled=mode&1?import_math(&c,"nan"):win64_math_call(&c,"nan");
        int native_errno=errno;environment(after);
        if(!handled||memcmp(&c,&expected,sizeof(c))||native_errno!=2718||memcmp(before,after,sizeof(before)))return 7;
    }
    CPU c,old;memset(&c,0xa7,sizeof(c));old=c;
    if(win64_math_call(&c,"unknown_fdtest")||memcmp(&c,&old,sizeof(c)))return 6;
    printf("{\"cases\":%lu,\"nan_cases\":1024,\"failures\":0,\"full_cpu_memory_errno_native_fp\":true,\"direct_and_import\":true}\n",cases);return 0;
}
