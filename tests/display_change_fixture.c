/* Single virtual mode validation; no native display-mode calls. */
#define main old_display_matrix_main
#include "display_fixture.c"
#undef main
#include <errno.h>
static unsigned change_calls;
static uint64_t change(uint64_t device,uint64_t mode,uint64_t hwnd,uint64_t flags,uint64_t reserved,uint32_t expected){
    uint64_t stack[8]={UINT64_C(0x1122334455667788),0,0,0,0,reserved,0,UINT64_C(0x8877665544332211)};
    win64_region_add((uintptr_t)stack,sizeof(stack),0);
    CPU cpu,before;memset(&cpu,0xa7,sizeof(cpu));cpu.rsp=(uintptr_t)stack;cpu.rcx=device;cpu.rdx=mode;cpu.r8=hwnd;cpu.r9=flags;before=cpu;before.rax=expected;
    win64_set_error(0x5a17);errno=1927;
    require(win64_display_call(&cpu,"ChangeDisplaySettingsExW"),"display change handled");
    require(!memcmp(&cpu,&before,sizeof(cpu)),"display change fullCPU/EAX32/no-pop preservation");
    require(win64_get_error()==0x5a17&&errno==1927,"display change nativeerrno/LastError preserved");
    require(stack[0]==UINT64_C(0x1122334455667788)&&stack[7]==UINT64_C(0x8877665544332211),"display argument stack canaries");
    win64_region_remove((uintptr_t)stack);++change_calls;return cpu.rax;
}
static void mode_init(unsigned char *mode,uint16_t size,uint32_t fields){
    memset(mode,0,220);put16(mode,68,size);put32(mode,72,fields);put32(mode,168,32);put32(mode,172,960);put32(mode,176,540);put32(mode,184,60);
}
int main(int argc,char **argv){
    if(argc==2){
        win64_display_configure(960,540,60);unsigned char mode[220];mode_init(mode,188,0x7c0000);win64_region_add((uintptr_t)mode,sizeof(mode),0);
        if(!strcmp(argv[1],"--flags"))change(0,0,0,1,0,0);
        else if(!strcmp(argv[1],"--reserved"))change(0,0,0,2,1,0);
        else if(!strcmp(argv[1],"--size")){put16(mode,68,219);change(0,(uintptr_t)mode,0,2,0,0);}
        else if(!strcmp(argv[1],"--extra")){put16(mode,70,1);change(0,(uintptr_t)mode,0,2,0,0);}
        else if(!strcmp(argv[1],"--fields")){put32(mode,72,0);change(0,(uintptr_t)mode,0,2,0,0);}
        else return 3;
        return 99;
    }
    require(argc==1,"display fixture arguments");int old=old_display_matrix_main();if(old)return old;
    CPU disabled,expected;memset(&disabled,0xa7,sizeof(disabled));disabled.rsp=123;expected=disabled;expected.rax=UINT32_MAX;
    win64_set_error(0x5a17);errno=1927;require(win64_display_call(&disabled,"ChangeDisplaySettingsExW")&&!memcmp(&disabled,&expected,sizeof(disabled))&&win64_get_error()==50&&errno==1927,"disabled profile has no mode success/read");
    win64_display_configure(960,540,60);
    uint16_t adapter[32],lower[32],child[48],unknown[16];wide(adapter,"\\\\.\\DISPLAY1");wide(lower,"\\\\.\\display1");wide(child,"\\\\.\\DISPLAY1\\Monitor0");wide(unknown,"UNKNOWN");
    win64_region_add((uintptr_t)adapter,sizeof(adapter),0);win64_region_add((uintptr_t)lower,sizeof(lower),0);win64_region_add((uintptr_t)child,sizeof(child),0);win64_region_add((uintptr_t)unknown,sizeof(unknown),0);
    unsigned char query[220],original_query[220];memset(query,0xa5,sizeof(query));put16(query,68,220);put16(query,70,0);win64_region_add((uintptr_t)query,sizeof(query),1);
    require(api("EnumDisplaySettingsW",3,V((uintptr_t)adapter),V(UINT32_MAX),V((uintptr_t)query))==1,"snapshot current mode");memcpy(original_query,query,sizeof(query));
    struct {uint64_t before;unsigned char mode[220];uint64_t after;}guard={UINT64_C(0x1122334455667788),{0},UINT64_C(0x8877665544332211)};
    const uint64_t names[]={0,(uintptr_t)adapter,(uintptr_t)lower};const uint32_t flags[]={0,2,4,6};const uint32_t fields[]={0x7c0000,0x7c00a0};
    for(unsigned n=0;n<3;++n)for(unsigned f=0;f<4;++f){
        change(names[n],0,0,UINT64_C(0xa5a5a5a500000000)|flags[f],0,0);
        for(unsigned s=0;s<2;++s)for(unsigned d=0;d<2;++d){
            mode_init(guard.mode,s?220:188,fields[d]);unsigned char snapshot[sizeof(guard)];memcpy(snapshot,&guard,sizeof(guard));
            win64_region_add((uintptr_t)guard.mode,s?220:188,0);change(names[n],(uintptr_t)guard.mode,0,UINT64_C(0xa5a5a5a500000000)|flags[f],0,0);win64_region_remove((uintptr_t)guard.mode);
            require(!memcmp(snapshot,&guard,sizeof(guard)),"DEVMODE read-only prefix/SDKtail/outside guards unchanged");
        }
    }
    const size_t offsets[]={168,172,176,180,184,76,80,84};const uint32_t mismatches[]={24,1920,1080,1,75,1,1,1};
    for(unsigned i=0;i<8;++i){mode_init(guard.mode,188,0x7c00a0);put32(guard.mode,offsets[i],mismatches[i]);unsigned char snapshot[sizeof(guard)];memcpy(snapshot,&guard,sizeof(guard));win64_region_add((uintptr_t)guard.mode,188,0);change((uintptr_t)adapter,(uintptr_t)guard.mode,0,4,0,UINT32_C(0xfffffffe));win64_region_remove((uintptr_t)guard.mode);require(!memcmp(snapshot,&guard,sizeof(guard)),"unsupported mode does not modify request");}
    mode_init(guard.mode,188,0x7c0000);put32(guard.mode,76,123);put32(guard.mode,80,456);put32(guard.mode,84,1);win64_region_add((uintptr_t)guard.mode,188,0);change(0,(uintptr_t)guard.mode,0,4,0,0);win64_region_remove((uintptr_t)guard.mode);
    change((uintptr_t)unknown,UINT64_MAX,0,2,0,UINT32_C(0xfffffffb));change((uintptr_t)child,UINT64_MAX,0,2,0,UINT32_C(0xfffffffb));change(0,UINT64_MAX,1,2,0,UINT32_C(0xfffffffb));
    memset(query,0xa5,sizeof(query));put16(query,68,220);put16(query,70,0);require(api("EnumDisplaySettingsW",3,V((uintptr_t)adapter),V(UINT32_MAX),V((uintptr_t)query))==1&&!memcmp(query,original_query,sizeof(query)),"same/NULL/badmode queries never mutate virtual current/default mode");
    win64_region_remove((uintptr_t)query);win64_region_remove((uintptr_t)adapter);win64_region_remove((uintptr_t)lower);win64_region_remove((uintptr_t)child);win64_region_remove((uintptr_t)unknown);win64_display_configure(0,0,0);
    printf("DISPLAY_CHANGE_PASS checks=%u old_callbacks=%u change_calls=%u same_mode_noop=1 NULL_default_noop=1 flags0_2_4_6=1 dm188_220=1 fields7c0000_7c00a0=1 input_readonly=1 current_mode_unchanged=1 badmode_minus2=1 badparam_minus5=1 fullCPU=1 EAX32=1 no_rsp_pop=1 errors_preserved=1 physical_display_application=0\n",checks,callbacks,change_calls);return 0;
}
