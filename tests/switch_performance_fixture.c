#include "switch_performance.h"
#include "cpu64.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mobile;
int frostpunk_dxvk_mobile_effects(void){return mobile;}
int frostpunk_mobile_graphics_reduced(unsigned group){(void)group;return mobile;}
uint32_t fp_port_setting_get(uint32_t id){assert(id==22);return mobile;}
void fp_port_setting_set(uint32_t id,uint32_t value){if(id==22&&value<=1)mobile=value;}
void fp_port_gesture_help(CPU *c){(void)c;}
void fp_mobile_output_config(uint64_t config,uint32_t value){(void)config;(void)value;}
static void transaction(CPU *c){
#include "mobile-transaction.inc"
L_000141A17706:return;
}
static const struct{const char *path;unsigned width,height,mips,skip;} cases[]={
#include "switch-cases.inc"
};
int main(void){
    unsigned tested=0;
    for(unsigned mode=0;mode<2;++mode){mobile=mode;
        for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){
            uint32_t h[4]={cases[i].width,cases[i].height,0x36545341u,cases[i].mips};
            assert(fp_switch_texture_skip(cases[i].path,h,0)==(mode?cases[i].skip:0));
            const uint32_t formats[]={0x36545341u,0x31545844u,0x33545844u,0x35545844u};
            for(unsigned f=0;f<4;++f){h[2]=formats[f];assert(fp_switch_texture_skip(cases[i].path,h,0)==(mode?cases[i].skip:0));}
            h[2]=0x36545341u;
            assert(fp_switch_texture_skip(cases[i].path,h,4)==4);
            char path[272];strcpy(path,cases[i].path);char *dot=strrchr(path,'.');strcpy(dot,".DDS|variant");
            for(char *p=path;*p;++p){if(*p=='/')*p='\\';else if(*p>='a'&&*p<='z')*p-=32;}
            assert(fp_switch_texture_skip(path,h,0)==(mode?cases[i].skip:0));
            h[0]++;assert(fp_switch_texture_skip(path,h,0)==0);h[0]--;
            h[3]=1;assert(fp_switch_texture_skip(path,h,0)==0);h[3]=17;assert(fp_switch_texture_skip(path,h,0)==0);
            h[3]=cases[i].mips;h[2]=21;assert(fp_switch_texture_skip(path,h,0)==0);
            ++tested;
        }
    }
    #ifdef _WIN32
    _putenv_s("FROSTPUNK_TEXTURE_QUALITY","switch");
    #else
    setenv("FROSTPUNK_TEXTURE_QUALITY","switch",1);
    #endif
    for(unsigned mode=0;mode<2;++mode){mobile=mode;
        for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){
            unsigned skip=cases[i].skip;
            uint32_t h[4]={cases[i].width>>skip,cases[i].height>>skip,0x36545341u,cases[i].mips-skip};
            for(unsigned original=0;original<4;++original)
                assert(fp_switch_texture_skip(cases[i].path,h,original)==(original>skip?original-skip:0));
        }
    }
    #ifdef _WIN32
    _putenv_s("FROSTPUNK_TEXTURE_QUALITY","");
    #else
    unsetenv("FROSTPUNK_TEXTURE_QUALITY");
    #endif
    uint32_t shared_ui[4]={1024,1024,0x36545341u,11};
    assert(fp_switch_texture_skip("effects/quadtextures/smears/bigsplotch2forfreezingtobbar.dds",shared_ui,0)==0);
    uint32_t h[4]={4096,2048,0x36545341u,13};char long_path[261];memset(long_path,'a',260);long_path[260]=0;
    assert(fp_switch_texture_skip(long_path,h,0)==0&&fp_switch_texture_skip(NULL,h,0)==0&&fp_switch_texture_skip("ui/toppanel/top_panel_elements.dds",h,0)==0&&fp_switch_texture_skip(cases[0].path,NULL,0)==0);
    _Alignas(16) unsigned char original[176],config[176],expected[176];memset(original,0xa5,sizeof(original));
    const unsigned floats[]={104,108,120,124,144};const float maxima[]={.125f,0,.125f,.125f,0};
    for(unsigned i=0;i<5;++i)wrss((uintptr_t)original+floats[i],1);
    memcpy(expected,original,sizeof(expected));
    const unsigned disabled[]={76,77,78,79,80,81,82,92,100,101,113,128,129,152,160,161,163};for(unsigned i=0;i<sizeof(disabled)/sizeof(*disabled);++i)expected[disabled[i]]=0;
    expected[148]=1;wr32((uintptr_t)expected+84,1);wr32((uintptr_t)expected+116,1);
    for(unsigned i=0;i<5;++i)wrss((uintptr_t)expected+floats[i],maxima[i]);
    for(unsigned i=0;i<1024;++i){mobile=i&1;memcpy(config,original,sizeof(config));errno=EILSEQ;fp_switch_gfx_config((uintptr_t)config);assert(errno==EILSEQ);assert(!memcmp(config,mobile?expected:original,sizeof(config)));}
    mobile=1;const float lower[]={0,.1f,-1,NAN,INFINITY};
    for(unsigned i=0;i<sizeof(lower)/sizeof(*lower);++i){memcpy(config,original,sizeof(config));
        for(unsigned j=0;j<5;++j)wrss((uintptr_t)config+floats[j],lower[i]);
        fp_switch_gfx_config((uintptr_t)config);
        for(unsigned j=0;j<5;++j){float want=isfinite(lower[i])&&lower[i]>maxima[j]?maxima[j]:lower[i];assert(!memcmp(config+floats[j],&want,4));}}
    for(unsigned i=0;i<2;++i){memcpy(config,original,sizeof(config));wr32((uintptr_t)config+84,i);wr32((uintptr_t)config+116,i);
        fp_switch_gfx_config((uintptr_t)config);assert(rd32((uintptr_t)config+84)==i&&rd32((uintptr_t)config+116)==i);}
    unsigned transactions=0;unsigned char stack[128]={0};
    for(unsigned id=22;id<=30;++id)for(unsigned old=0;old<2;++old)for(unsigned value=0;value<3;++value){
        memset(config,0,sizeof(config));unsigned char dirty=0;mobile=old;
        CPU cpu={0};cpu.rbx=id-1;cpu.rdi=(uintptr_t)config;cpu.rsi=(uintptr_t)&dirty;cpu.rsp=(uintptr_t)stack;wr32(cpu.rsp+0x50,value);
        transaction(&cpu);unsigned changed=id==22&&value<=1&&old!=value;
        assert(config[0x2c]==changed&&dirty==changed);assert(mobile==(id==22&&value<=1?value:old));++transactions;
    }
    printf("{\"success\":true,\"texture_mode_cases\":%u,\"gfx_on_off_cycles\":1024,\"mobile_transactions\":%u,\"ui_unchanged\":true,\"original_quality_restored_by_transaction\":true}\n",tested,transactions);
}
