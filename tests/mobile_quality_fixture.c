#include "cpu64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void L_000140F752A0(CPU *);
void L_0001410343F0(CPU *);
int fp_mobile_particle_fill_disabled(void);
static int mobile, calls;
int frostpunk_dxvk_mobile_effects(void) { return mobile; }
static void original(CPU *c, unsigned kind) {
    ++calls;
    c->rax=UINT64_C(0x123456789abcde01);c->r8^=kind;c->rip+=kind;c->rsp+=8;
    c->xmm[5].u64[1]^=UINT64_C(0x2233445566778899);c->af=1;
}
void fp_quality_original_L_000140F752A0(CPU *c) { original(c,1); }
void fp_quality_original_L_0001410343F0(CPU *c) { original(c,2); }
int main(int argc,char **argv) {
    int override=argc>1?atoi(argv[1]):-1;
    int particles=argc>2?atoi(argv[2]):-1;
    unsigned cases=0;
    for(mobile=0;mobile<2;++mobile)for(unsigned iteration=0;iteration<256;++iteration)for(unsigned kind=1;kind<=2;++kind){
        CPU c,expected;
        for(unsigned i=0;i<sizeof(c);++i)((unsigned char*)&c)[i]=(unsigned char)(i*17+iteration*11);
        expected=c;original(&expected,kind);calls=0;
        unsigned mode=override>=0?(unsigned)override:(mobile?3u:0u);
        if(mode&kind)SET8L(expected.rax,0);
        errno=731;
        if(kind==1)L_000140F752A0(&c);else L_0001410343F0(&c);
        if(calls!=1||memcmp(&c,&expected,sizeof(c))||errno!=731)abort();
        unsigned disabled=particles>=0?(unsigned)particles:mode==3u;
        errno=731;
        if(fp_mobile_particle_fill_disabled()!=(int)disabled||errno!=731)abort();
        ++cases;
    }
    printf("{\"success\":true,\"cases\":%u,\"live_mobile_setting\":true,\"full_cpu\":true,\"original_called_once\":true,\"errno_preserved\":true}\n",cases);
    return 0;
}
