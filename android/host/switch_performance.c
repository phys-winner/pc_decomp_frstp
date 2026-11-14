#include "switch_performance.h"
#include "cpu64.h"
#include "port_settings.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#endif

static const struct {uint32_t key;uint16_t width,height;uint8_t skip;} caps[]={
#include "switch_texture_caps.inc"
};

static uint32_t texture_key(const char *path){
    char name[272];size_t n=0;
    if(!path)return 0;
    while(n<260&&path[n]&&path[n]!='|'){unsigned char c=path[n];name[n]=c=='\\'?'/':c>='A'&&c<='Z'?c+32:c;++n;}
    if(n==260)return 0;
    name[n]=0;char *dot=strrchr(name,'.');if(!dot)return 0;
    if(strcmp(dot,".dds")&&strcmp(dot,".texture"))return 0;
    n=(size_t)(dot-name);memcpy(name+n,".texture",9);n+=8;
    uint32_t h=(uint32_t)n;size_t at=0;
    while(n-at>=4){uint32_t k;memcpy(&k,name+at,4);k*=0x5bd1e995u;k^=k>>24;k*=0x5bd1e995u;h=(h*0x5bd1e995u)^k;at+=4;}
    if(n>at){uint32_t tail=0;for(size_t i=0;i<n-at;++i)tail|=(uint32_t)(unsigned char)name[at+i]<<(8*i);h=(h^tail)*0x5bd1e995u;}
    h^=h>>13;h*=0x5bd1e995u;return h^(h>>15);
}

uint32_t fp_switch_texture_skip(const char *path,const uint32_t header[4],uint32_t original){
    // shortcut: Resident mip chains change on texture reload; use the engine refresh transaction for instant switching.
    if(!header)return original;
    if(header[2]!=0x36545341u&&header[2]!=0x31545844u&&header[2]!=0x33545844u&&header[2]!=0x35545844u)return original;
    uint32_t key=texture_key(path);size_t lo=0,hi=sizeof(caps)/sizeof(*caps);
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(caps[mid].key<key)lo=mid+1;else hi=mid;}
    if(lo==sizeof(caps)/sizeof(*caps)||caps[lo].key!=key)return original;
    unsigned mips=header[3]&255u,skip=caps[lo].skip;
    const char *quality=getenv("FROSTPUNK_TEXTURE_QUALITY");
    // The cached chain already omits these levels; engine Low must not discard them twice.
    if(header[2]==0x36545341u&&quality&&!strcmp(quality,"switch")&&mips&&mips<=16&&
       header[0]==(caps[lo].width>>skip)&&header[1]==(caps[lo].height>>skip))return original>skip?original-skip:0;
    if(!frostpunk_mobile_graphics_reduced(FP_GFX_TEXTURES)||caps[lo].width!=header[0]||caps[lo].height!=header[1])return original;
    if(mips<=skip||mips>16||skip<=original)return original;
    return skip;
}

static void cap_float(uint64_t config,unsigned offset,float maximum){
    float value=rdss(config+offset);if(isfinite(value)&&value>maximum)wrss(config+offset,maximum);
}

void fp_switch_gfx_config(uint64_t config){
    int mobile=frostpunk_dxvk_mobile_effects();
    /* Extend the Switch profile to minimum rendering; authored PC presets stay intact.  The launcher
     * keeps any group at PC quality (android/port.json "Graphics" options). */
    if(frostpunk_mobile_graphics_reduced(FP_GFX_EFFECTS)){
        /* interiors, detail, refraction, softness; local lights, GI, particles, FXAA/SSAO/SSR/DOF */
        wr8(config+76,0);wr8(config+77,0);wr8(config+78,0);wr8(config+79,0);
        if(rd32(config+84)>1)wr32(config+84,1);
        cap_float(config,108,0);wr8(config+113,0);
        cap_float(config,144,0);wr8(config+148,1);
        wr8(config+152,0);wr8(config+160,0);wr8(config+161,0);wr8(config+163,0);
    }
    if(frostpunk_mobile_graphics_reduced(FP_GFX_TEXTURES)){
        /* trilinear, anisotropic filtering, forced highest mip */
        wr8(config+80,0);wr8(config+81,0);wr8(config+82,0);
    }
    if(frostpunk_mobile_graphics_reduced(FP_GFX_SNOW)){
        /* tessellation, triplanar and splatting; 128 / 1024 GPU snow uses GPU resampling,
         * preserving logical snow/save dimensions. */
        wr8(config+92,0);wr8(config+100,0);wr8(config+101,0);
        cap_float(config,104,.125f);
    }
    if(frostpunk_mobile_graphics_reduced(FP_GFX_SHADOWS)){
        /* cascades, shadow-map scales, point-light shadows and filtering */
        if(rd32(config+116)>1)wr32(config+116,1);
        cap_float(config,120,.125f);cap_float(config,124,.125f);
        wr8(config+128,0);wr8(config+129,0);
    }
#ifdef __ANDROID__
    static _Atomic int previous=-1;
    if(atomic_exchange_explicit(&previous,mobile,memory_order_relaxed)!=mobile){
        int saved=errno;
        fprintf(stderr,"FP_SWITCH_GFX mobile=%d snow_scale=%.3f tessellation=%u lights=%.3f particle_spawn=%.3f gi=%u dof=%u texture_caps=%zu\n",mobile,rdss(config+104),rd8(config+92),rdss(config+108),rdss(config+144),rd8(config+113),rd8(config+163),sizeof(caps)/sizeof(*caps));
        errno=saved;
    }
#endif
}

#ifdef __ANDROID__
/* A profile's first start (no config.bin yet) begins at the Graphics menu's "Low" preset instead
 * of the constructor default High (3 in all eight quality levels: on the Pixel 10a a new profile
 * took 3.3 GB of DXVK memory and was killed while loading the city).  Runs on WinMain entry, when
 * the static initializer has built FrostpunkEngineConfig at 0x14296E8F0 and the engine has not
 * looked for config.bin yet; without the file the engine keeps and saves these values (the PC
 * renderer never auto-detects: RenderingDeviceDirectX11PC +48 returns 0).  They are what the menu
 * writes for "Low": sub_140E66520(level 1) for row 9, and the SettingsConfig sub-rows of level 1 -
 * shadows/textures/snow Low (rows 10-12), particles Quarter = special effects 0 (row 13), FSAA,
 * AO, GI and DoF off (rows 14-17, bytes 233-236).  Existing profiles are never touched. */
void fp_graphics_first_run_defaults(void){
    static const uint16_t levels[8]={28,116,120,124,128,132,136,140};
    const uint64_t config=UINT64_C(0x14296E8F0),gfx=config+176;   /* engine config, renderer container */
    const char *profile=getenv("FROSTPUNK_SERVICES_PROFILE");char path[4096];
    if(!profile||snprintf(path,sizeof(path),"%s/AppData/11bitstudios/Frostpunk/config.bin",profile)>=(int)sizeof(path)||
       !access(path,F_OK))return;
    int pristine=rd64(config)==UINT64_C(0x141D4F3B8);
    for(unsigned i=0;i<8;++i)pristine&=rd32(gfx+levels[i])==3;
    if(!pristine){fprintf(stderr,"FP_GRAPHICS_DEFAULTS skipped=unexpected_config\n");return;}
    for(unsigned i=0;i<8;++i)wr32(gfx+levels[i],1);
    wr32(gfx+136,0);
    for(unsigned i=233;i<=236;++i)wr8(gfx+i,0);
    fprintf(stderr,"FP_GRAPHICS_DEFAULTS preset=low new_profile=1\n");
}
#endif
