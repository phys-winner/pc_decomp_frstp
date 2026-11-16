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
            const uint32_t formats[]={0x36545341u,0x34545341u,0x31545844u,0x33545844u,0x35545844u};
            for(unsigned f=0;f<5;++f){h[2]=formats[f];assert(fp_switch_texture_skip(cases[i].path,h,0)==(mode?cases[i].skip:0));}
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
    mobile=1;
#ifdef _WIN32
    _putenv_s("PORT_OPTION_TEXTURE_LIMIT","256");
#else
    setenv("PORT_OPTION_TEXTURE_LIMIT","256",1);
#endif
    {
        uint32_t world[4]={4096,4096,0x35545844u,13};
        assert(fp_switch_texture_skip("world/buildings/generator_house.dds",world,0)==4);
        assert(fp_switch_texture_skip("ui/world/generator_house.dds",world,0)==0);
        assert(fp_switch_texture_skip("fonts/title.dds",world,0)==0);
        assert(fp_switch_texture_skip("effects/quadtextures/smears/bigsplotch2forfreezingtobbar.dds",shared_ui,0)==2);
        uint32_t mid[4]={512,512,0x35545844u,10};
        assert(fp_switch_texture_skip("world/buildings/not_in_the_table.dds",mid,0)==1);
        uint32_t reduced[4]={1024,1024,0x35545844u,11};
        assert(fp_switch_texture_skip("world/buildings/generator_house.dds",reduced,0)==2);
        world[3]=2;assert(fp_switch_texture_skip("world/buildings/generator_house.dds",world,0)==1);
        int known=0;
        for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i){
            if(cases[i].width<4096||cases[i].height<2048)continue;
            uint32_t hknown[4]={cases[i].width,cases[i].height,0x33545844u,cases[i].mips};
            uint32_t skip=cases[i].skip,big=cases[i].width>cases[i].height?cases[i].width:cases[i].height;
            while(skip+1u<cases[i].mips&&(big>>skip)>256u)++skip;
            uint32_t want=cases[i].mips>skip&&skip>0?skip:0;
            assert(fp_switch_texture_skip(cases[i].path,hknown,0)==want);
            known=1;break;
        }
        assert(known);
        mobile=0;world[3]=13;assert(fp_switch_texture_skip("world/buildings/generator_house.dds",world,0)==0);
        assert(fp_switch_texture_skip("effects/quadtextures/smears/bigsplotch2forfreezingtobbar.dds",shared_ui,0)==0);
        mobile=1;
#ifdef _WIN32
        _putenv_s("PORT_OPTION_TEXTURE_LIMIT","512");
#else
        setenv("PORT_OPTION_TEXTURE_LIMIT","512",1);
#endif
        assert(fp_switch_texture_skip("world/buildings/generator_house.dds",world,0)==3);
#ifdef _WIN32
        _putenv_s("PORT_OPTION_TEXTURE_LIMIT","");
#else
        unsetenv("PORT_OPTION_TEXTURE_LIMIT");
#endif
        assert(fp_switch_texture_skip("world/buildings/generator_house.dds",world,0)==0);
    }
    printf("{\"success\":true,\"texture_mode_cases\":%u,\"gfx_on_off_cycles\":1024,\"mobile_transactions\":%u,\"ui_unchanged\":true,\"original_quality_restored_by_transaction\":true}\n",tested,transactions);
}
