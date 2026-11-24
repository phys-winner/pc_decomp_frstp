#include "switch_performance.h"
#include "cpu64.h"
#include "port_settings.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
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

static int path_word(const char *path,const char *word){
    size_t n=strlen(word);
    for(const char *p=path;*p;++p){
        if(p!=path&&p[-1]!='/'&&p[-1]!='\\')continue;
        size_t i=0;for(;i<n;++i){unsigned char c=(unsigned char)p[i];if(c>='A'&&c<='Z')c=(unsigned char)(c+32);if(c!=(unsigned char)word[i])break;}
        if(i==n&&(p[n]=='/'||p[n]=='\\'||p[n]==0))return 1;
    }
    return 0;
}

/* UI and fonts stay sharp. Everything else is world or effect art. */
static int keeps_full_mips(const char *path){
    return path_word(path,"ui")||path_word(path,"font")||path_word(path,"fonts");
}

static uint32_t texture_edge_limit(void){
    const char *text=getenv("PORT_OPTION_TEXTURE_LIMIT");
    if(!text||!text[0])return 0;
    char *end=0;unsigned long value=strtoul(text,&end,10);
    return end&&!*end&&value>=64ul&&value<=4096ul?(uint32_t)value:0;
}

static uint32_t raise_skip(uint32_t skip,uint32_t big,unsigned mips,uint32_t limit){
    while(skip+1u<mips&&(big>>skip)>limit)++skip;
    return skip;
}

/* Textures absent from the Switch table, and table rows whose file header is already
 * smaller than the authored size. raise_skip stops once the current largest mip fits,
 * so a second call with the same header does not discard another level. */
static uint32_t tight_unknown_skip(const char *path,const uint32_t header[4],uint32_t original){
    uint32_t limit=texture_edge_limit();unsigned mips=header[3]&255u;
    uint32_t big=header[0]>header[1]?header[0]:header[1];
    if(!limit||!frostpunk_mobile_graphics_reduced(FP_GFX_TEXTURES)||keeps_full_mips(path))return original;
    if(big<=limit||mips<2u||mips>16u)return original;
    uint32_t skip=raise_skip(original,big,mips,limit);
    return skip>original&&skip<mips?skip:original;
}

static void texture_budget_note(const uint32_t header[4],uint32_t original,uint32_t result,int format_ok){
    if(!header||!texture_edge_limit())return;
    int saved=errno;
    static uint64_t calls,capped,badfmt,top,kept;
    ++calls;
    if(!format_ok)++badfmt;
    else{
        top+=(uint64_t)header[0]*header[1];
        uint32_t w=header[0]>>result;if(!w)w=1;
        uint32_t h=header[1]>>result;if(!h)h=1;
        kept+=(uint64_t)w*h;
        if(result>original)++capped;
    }
    if((calls&63u)==0u){
        fprintf(stderr,"FP_TEXTURE_BUDGET calls=%llu capped=%llu badfmt=%llu top_mpix=%.1f kept_mpix=%.1f\n",
            (unsigned long long)calls,(unsigned long long)capped,(unsigned long long)badfmt,
            (double)top/1000000.0,(double)kept/1000000.0);
        fflush(stderr);
    }
    errno=saved;
}

uint32_t fp_switch_texture_skip(const char *path,const uint32_t header[4],uint32_t original){
    // shortcut: Resident mip chains change on texture reload; use the engine refresh transaction for instant switching.
    uint32_t result;
    if(!header)return original;
    /* DXT1/3/5 originals and the converted ASTC ("AST4", "AST6") or ETC2 ("ETC2", "ETCA") archive. */
    int converted=header[2]==0x36545341u||header[2]==0x34545341u||header[2]==0x32435445u||header[2]==0x41435445u;
    if(!converted&&header[2]!=0x31545844u&&header[2]!=0x33545844u&&header[2]!=0x35545844u){
        texture_budget_note(header,original,original,0);
        return original;
    }
    if(texture_edge_limit()&&keeps_full_mips(path)){
        texture_budget_note(header,original,original,1);
        return original;
    }
    uint32_t key=texture_key(path);size_t lo=0,hi=sizeof(caps)/sizeof(*caps);
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(caps[mid].key<key)lo=mid+1;else hi=mid;}
    if(lo==sizeof(caps)/sizeof(*caps)||caps[lo].key!=key){
        result=tight_unknown_skip(path,header,original);
        texture_budget_note(header,original,result,1);
        return result;
    }
    unsigned mips=header[3]&255u,skip=caps[lo].skip;
    const char *quality=getenv("FROSTPUNK_TEXTURE_QUALITY");
    int astc=converted;
    // The cached chain already omits these levels; engine Low must not discard them twice.
    if(astc&&quality&&!strcmp(quality,"switch")&&mips&&mips<=16&&
       header[0]==(caps[lo].width>>skip)&&header[1]==(caps[lo].height>>skip)){
        result=original>skip?original-skip:0;
        if(texture_edge_limit())result=tight_unknown_skip(path,header,result);
        texture_budget_note(header,original,result,1);
        return result;
    }
    if(!frostpunk_mobile_graphics_reduced(FP_GFX_TEXTURES)||caps[lo].width!=header[0]||caps[lo].height!=header[1]){
        result=texture_edge_limit()?tight_unknown_skip(path,header,original):original;
        texture_budget_note(header,original,result,1);
        return result;
    }
    if(mips>16){texture_budget_note(header,original,original,1);return original;}
#if defined(__ANDROID__)
    {
        const char *memory=getenv("PORT_OPTION_MEMORY");
        /* One extra mip on the minimal budget, never the last remaining level. */
        if(memory&&!strcmp(memory,"minimal")&&skip+1u<mips)skip+=1u;
    }
#endif
    {
        uint32_t limit=texture_edge_limit(),big=header[0]>header[1]?header[0]:header[1];
        if(limit)skip=raise_skip(skip,big,mips,limit);
    }
    result=mips<=skip||skip<=original?original:skip;
    texture_budget_note(header,original,result,1);
    return result;
}

static void cap_float(uint64_t config,unsigned offset,float maximum){
    float value=rdss(config+offset);if(isfinite(value)&&value>maximum)wrss(config+offset,maximum);
}

#ifdef __ANDROID__
/* Live descriptor probe. fp_switch_gfx_config only reads. Writes happen in fp_fx_apply,
 * which the texture fixture never calls. 0x140E71680 passes a stack descriptor; a later
 * call keeps the previous pointer while its first 16 bytes still match. */
#define FX_BYTES 0xACu
static uint64_t fx_descriptor;
static unsigned char fx_baseline[FX_BYTES];
static unsigned char fx_head[16];
static int fx_have;
static int fx_override_active;
static int fx_index = -1;
static unsigned fx_want_n;
static unsigned char fx_want[4];
static char fx_seen[PROP_VALUE_MAX];
static int fx_seen_valid;
static int fx_rewrite_logged;
static int fx_stale_logged;
static char menu_seen[PROP_VALUE_MAX];
static int menu_seen_valid;
static int menu_index = -1;
static uint32_t menu_prev;

static int fx_head_match(uint64_t config) {
    unsigned char now[16];
    if (!config) return 0;
    memcpy(now, (const void *)(uintptr_t)config, 16);
    return !memcmp(now, fx_head, 16);
}

static int fx_mirror_match(uint64_t config) {
    unsigned char a[16], b[16];
    if (!config) return 0;
    memcpy(a, (const void *)(uintptr_t)config, 16);
    memcpy(b, (const void *)(uintptr_t)(config + FX_BYTES), 16);
    return !memcmp(a, b, 16);
}

static void fx_remember(uint64_t config) {
    int saved = errno;
    if (!config) { errno = saved; return; }
    if (fx_have && fx_head_match(fx_descriptor)) {
        /* Same address and no live poke: keep the snapshot current so "off" restores the
         * latest clamp, not the first one. The fixture hits the same address every cycle. */
        if (config == fx_descriptor && !fx_override_active &&
            memcmp((const void *)(uintptr_t)config, fx_baseline, FX_BYTES)) {
            memcpy(fx_baseline, (const void *)(uintptr_t)config, FX_BYTES);
            uint64_t b = (uintptr_t)fx_baseline;
            fprintf(stderr, "FP_FX baseline=refresh ptr=%llx gi=%u dof=%u lights=%.3f particles=%.3f cascades=%u snow=%.3f tessellation=%u interiors=%u post152=%u post160=%u post161=%u\n",
                (unsigned long long)config, rd8(b + 113), rd8(b + 163), rdss(b + 108), rdss(b + 144), rd32(b + 116), rdss(b + 104), rd8(b + 92), rd8(b + 76), rd8(b + 152), rd8(b + 160), rd8(b + 161));
        }
        /* A different address whose +0xAC mirror matches, while the saved one does not,
         * is the renderer copy replacing a stack snapshot. */
        if (config == fx_descriptor || fx_override_active || !fx_mirror_match(config) || fx_mirror_match(fx_descriptor)) {
            errno = saved; return;
        }
    } else if (fx_have && fx_override_active) {
        errno = saved; return;
    }
    memcpy(fx_baseline, (const void *)(uintptr_t)config, FX_BYTES);
    memcpy(fx_head, fx_baseline, 16);
    fx_descriptor = config;
    int refresh = fx_have;
    fx_have = 1;
    uint64_t b = (uintptr_t)fx_baseline;
    fprintf(stderr, "FP_FX baseline=%s ptr=%llx gi=%u dof=%u lights=%.3f particles=%.3f cascades=%u snow=%.3f tessellation=%u interiors=%u post152=%u post160=%u post161=%u\n",
        refresh ? "refresh" : "1", (unsigned long long)config, rd8(b + 113), rd8(b + 163), rdss(b + 108), rdss(b + 144), rd32(b + 116), rdss(b + 104), rd8(b + 92), rd8(b + 76), rd8(b + 152), rd8(b + 160), rd8(b + 161));
    errno = saved;
}
#endif

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
    fx_remember(config);
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

static const struct { const char *name; unsigned offset; char kind; } fx_fields[] = {
    {"interiors", 76, 'b'}, {"obj_detail", 77, 'b'}, {"refraction", 78, 'b'}, {"softness", 79, 'b'},
    {"trilinear", 80, 'b'}, {"aniso", 81, 'b'}, {"mip", 82, 'b'}, {"tessellation", 92, 'b'},
    {"triplanar", 100, 'b'}, {"splatting", 101, 'b'}, {"gi", 113, 'b'}, {"point_shadows", 128, 'b'},
    {"shadow_filter", 129, 'b'}, {"byte148", 148, 'b'}, {"post152", 152, 'b'}, {"post160", 160, 'b'},
    {"post161", 161, 'b'}, {"dof", 163, 'b'}, {"cap84", 84, 'u'}, {"cascades", 116, 'u'},
    {"snow", 104, 'f'}, {"lights", 108, 'f'}, {"shadow0", 120, 'f'}, {"shadow1", 124, 'f'}, {"particles", 144, 'f'},
};

static const struct { const char *name; unsigned offset; int byte; } menu_fields[] = {
    {"quality", 28, 0}, {"particles", 136, 0}, {"d116", 116, 0}, {"d120", 120, 0},
    {"d124", 124, 0}, {"d128", 128, 0}, {"d132", 132, 0}, {"d140", 140, 0},
    {"fsaa", 233, 1}, {"ao", 234, 1}, {"gi", 235, 1}, {"dof", 236, 1},
};

static void fx_log_live(const char *name, const char *value, int applied) {
    uint64_t c = fx_descriptor;
    fprintf(stderr, "FP_FX name=%s value=%s applied=%d mirror=%d gi=%u dof=%u lights=%.3f particles=%.3f cascades=%u snow=%.3f tessellation=%u interiors=%u post152=%u post160=%u post161=%u\n",
        name, value, applied > 0, applied == 2, rd8(c + 113), rd8(c + 163), rdss(c + 108), rdss(c + 144), rd32(c + 116), rdss(c + 104), rd8(c + 92), rd8(c + 76), rd8(c + 152), rd8(c + 160), rd8(c + 161));
}

static int fx_commit(void) {
    if (!fx_have || !fx_head_match(fx_descriptor)) return 0;
    uint64_t c = fx_descriptor;
    unsigned char ahead[16], mhead[16];
    memcpy(ahead, (const void *)(uintptr_t)c, 16);
    memcpy(mhead, (const void *)(uintptr_t)(c + FX_BYTES), 16);
    int mirror = !memcmp(ahead, mhead, 16);
    memcpy((void *)(uintptr_t)c, fx_baseline, FX_BYTES);
    if (mirror) memcpy((void *)(uintptr_t)(c + FX_BYTES), fx_baseline, FX_BYTES);
    if (fx_index >= 0) {
        uint64_t at = c + fx_fields[fx_index].offset;
        memcpy((void *)(uintptr_t)at, fx_want, fx_want_n);
        if (mirror) memcpy((void *)(uintptr_t)(at + FX_BYTES), fx_want, fx_want_n);
    }
    return mirror ? 2 : 1;
}

static int fx_stuck(void) {
    if (fx_index < 0 || !fx_have) return 1;
    return !memcmp((const void *)(uintptr_t)(fx_descriptor + fx_fields[fx_index].offset), fx_want, fx_want_n);
}

static int fx_find(const char *text, const char **value) {
    const char *eq = strchr(text, '=');
    if (!eq || eq == text || !eq[1]) return -1;
    size_t n = (size_t)(eq - text);
    for (unsigned i = 0; i < sizeof(fx_fields) / sizeof(*fx_fields); ++i) {
        if (strlen(fx_fields[i].name) == n && !memcmp(fx_fields[i].name, text, n)) { *value = eq + 1; return (int)i; }
    }
    return -1;
}

static void fx_encode(int index, const char *text) {
    char kind = fx_fields[index].kind;
    if (kind == 'f') {
        float v = strtof(text, NULL);
        memcpy(fx_want, &v, 4);
        fx_want_n = 4;
    } else if (kind == 'u') {
        uint32_t v = (uint32_t)strtoul(text, NULL, 0);
        memcpy(fx_want, &v, 4);
        fx_want_n = 4;
    } else {
        fx_want[0] = (unsigned char)strtoul(text, NULL, 0);
        fx_want_n = 1;
    }
}

static void fx_note(const char *value) {
    snprintf(fx_seen, sizeof(fx_seen), "%s", value);
    fx_seen_valid = 1;
}

static void fx_property(void) {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.fp.fx", value);
    if (fx_seen_valid && !strcmp(fx_seen, value)) {
        if (!fx_override_active) return;
        if (!fx_have || !fx_head_match(fx_descriptor)) {
            if (!fx_stale_logged) { fprintf(stderr, "FP_FX skip=stale\n"); fx_stale_logged = 1; }
            return;
        }
        if (!fx_stuck()) {
            int applied = fx_commit();
            if (applied && !fx_rewrite_logged) {
                fprintf(stderr, "FP_FX rewrite=1 name=%s\n", fx_fields[fx_index].name);
                fx_rewrite_logged = 1;
            }
        }
        return;
    }
    int request = value[0] && strcmp(value, "off");
    if (request && (!fx_have || !fx_head_match(fx_descriptor))) {
        if (fx_have && !fx_stale_logged) { fprintf(stderr, "FP_FX skip=stale\n"); fx_stale_logged = 1; }
        return;
    }
    fx_stale_logged = 0;
    fx_rewrite_logged = 0;
    if (!request) {
        fx_index = -1;
        fx_override_active = 0;
        int applied = fx_have && fx_head_match(fx_descriptor) ? fx_commit() : 0;
        fx_note(value);
        if (applied) fx_log_live("off", value[0] ? value : "empty", applied);
        return;
    }
    const char *text = NULL;
    int index = fx_find(value, &text);
    if (index < 0) {
        fprintf(stderr, "FP_FX skip=name value=%s\n", value);
        fx_index = -1;
        fx_override_active = 0;
        fx_note(value);
        return;
    }
    fx_encode(index, text);
    fx_index = index;
    int applied = fx_commit();
    if (!applied) { fx_index = -1; return; }
    fx_override_active = 1;
    fx_note(value);
    fx_log_live(fx_fields[index].name, text, applied);
}

static int fx_guest_static(uint64_t p) {
    return p >= UINT64_C(0x140000000) && p < UINT64_C(0x144000000);
}

static uint64_t fx_settings(void) {
    uint64_t slot = rd64(UINT64_C(0x142985AB0));
    if (fx_guest_static(slot) && rd64(slot) == UINT64_C(0x141D4F3B8)) return slot;
    if (rd64(UINT64_C(0x14296E8F0)) == UINT64_C(0x141D4F3B8)) return UINT64_C(0x14296E8F0);
    return 0;
}

static void fx_log_levels(uint64_t settings, const char *why) {
    uint64_t g = settings + 176;
    fprintf(stderr, "FP_FX_LEVELS why=%s q28=%u q116=%u q120=%u q124=%u q128=%u q132=%u q136=%u q140=%u b233=%u b234=%u b235=%u b236=%u gate=%u ptr=%llx fixed=%d\n",
        why, rd32(g + 28), rd32(g + 116), rd32(g + 120), rd32(g + 124), rd32(g + 128), rd32(g + 132), rd32(g + 136), rd32(g + 140),
        rd8(g + 233), rd8(g + 234), rd8(g + 235), rd8(g + 236), rd8(UINT64_C(0x142985AAC)),
        (unsigned long long)settings, settings == UINT64_C(0x14296E8F0));
}

static void menu_restore(uint64_t settings) {
    if (menu_index < 0) return;
    uint64_t g = settings + 176;
    if (menu_fields[menu_index].byte) wr8(g + menu_fields[menu_index].offset, (uint8_t)menu_prev);
    else wr32(g + menu_fields[menu_index].offset, menu_prev);
    menu_index = -1;
}

static void menu_dirty(uint64_t settings) {
    wr8(settings + 0x2c, 1);
    wr8(settings + 176 + 0x2c, 1);
}

static void fx_menu(void) {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("debug.fp.gfx", value);
    if (menu_seen_valid && !strcmp(menu_seen, value)) return;
    uint64_t settings = fx_settings();
    int request = value[0] && strcmp(value, "off");
    if (!settings) {
        static char waited[PROP_VALUE_MAX];
        if (request && strcmp(waited, value)) {
            fprintf(stderr, "FP_FX_MENU skip=settings value=%s\n", value);
            snprintf(waited, sizeof(waited), "%s", value);
        }
        return;
    }
    if (!request) {
        int held = menu_index >= 0;
        menu_restore(settings);
        if (held) menu_dirty(settings);
        snprintf(menu_seen, sizeof(menu_seen), "%s", value);
        menu_seen_valid = 1;
        fx_log_levels(settings, "off");
        return;
    }
    if (!strcmp(value, "read")) {
        snprintf(menu_seen, sizeof(menu_seen), "%s", value);
        menu_seen_valid = 1;
        fx_log_levels(settings, "read");
        return;
    }
    const char *text = NULL;
    int index = -1;
    const char *eq = strchr(value, '=');
    if (eq && eq != value && eq[1]) {
        size_t n = (size_t)(eq - value);
        for (unsigned i = 0; i < sizeof(menu_fields) / sizeof(*menu_fields); ++i) {
            if (strlen(menu_fields[i].name) == n && !memcmp(menu_fields[i].name, value, n)) { text = eq + 1; index = (int)i; break; }
        }
    }
    if (index < 0) {
        fprintf(stderr, "FP_FX_MENU skip=name value=%s\n", value);
        snprintf(menu_seen, sizeof(menu_seen), "%s", value);
        menu_seen_valid = 1;
        return;
    }
    menu_restore(settings);
    uint64_t g = settings + 176;
    if (menu_fields[index].byte) menu_prev = rd8(g + menu_fields[index].offset);
    else menu_prev = rd32(g + menu_fields[index].offset);
    unsigned long parsed = strtoul(text, NULL, 0);
    if (menu_fields[index].byte) wr8(g + menu_fields[index].offset, (uint8_t)parsed);
    else wr32(g + menu_fields[index].offset, (uint32_t)parsed);
    menu_index = index;
    menu_dirty(settings);
    snprintf(menu_seen, sizeof(menu_seen), "%s", value);
    menu_seen_valid = 1;
    fprintf(stderr, "FP_FX_MENU name=%s value=%lu prev=%u gate=%u ptr=%llx\n",
        menu_fields[index].name, parsed, menu_prev, rd8(UINT64_C(0x142985AAC)), (unsigned long long)settings);
    fx_log_levels(settings, menu_fields[index].name);
}

static int gate_quality(const char *value) { return value[0] ? (int)strtoul(value, NULL, 0) & 3 : -1; }
static int gate_fill(const char *value) { return value[0] ? !!(strtoul(value, NULL, 0) & 4) : -1; }
static int gate_far(const char *value) {
    if (!value[0]) return -1;
    unsigned long parsed = strtoul(value, NULL, 0);
    return parsed == 10 || parsed == 20 || parsed == 30 || parsed == 40 ? (int)parsed : 0;
}
static int gate_lod(const char *value) { return value[0] >= '0' && value[0] <= '3' && !value[1] ? value[0] - '0' : -1; }

static void fx_gates(void) {
    char quality[PROP_VALUE_MAX] = {0}, fill[PROP_VALUE_MAX] = {0}, far[PROP_VALUE_MAX] = {0}, lod[PROP_VALUE_MAX] = {0};
    static char seen_q[PROP_VALUE_MAX], seen_f[PROP_VALUE_MAX], seen_d[PROP_VALUE_MAX], seen_l[PROP_VALUE_MAX];
    static int ready;
    __system_property_get("debug.fp.quality", quality);
    __system_property_get("debug.fp.extra_fx", fill);
    __system_property_get("debug.fp.detail", far);
    __system_property_get("debug.fp.lod", lod);
    if (ready && !strcmp(seen_q, quality) && !strcmp(seen_f, fill) && !strcmp(seen_d, far) && !strcmp(seen_l, lod)) return;
    fprintf(stderr, "FP_FX_GATE quality=%d fill=%d far=%d lod=%d\n", gate_quality(quality), gate_fill(fill), gate_far(far), gate_lod(lod));
    snprintf(seen_q, sizeof(seen_q), "%s", quality);
    snprintf(seen_f, sizeof(seen_f), "%s", fill);
    snprintf(seen_d, sizeof(seen_d), "%s", far);
    snprintf(seen_l, sizeof(seen_l), "%s", lod);
    ready = 1;
}

/* Quality levels live in FrostpunkEngineConfig (0x14296E8F0, vtable 0x141D4F3B8), renderer at +176.
 * A saved profile loads High and a developed city then fills the DXVK heap. Reduced and minimal
 * budgets write the menu's Ultra low preset into that object after the file has been read, and
 * put the saved levels back only while the same object is serialized. The dirty bytes at +0x2c
 * stay untouched, so an unchanged session does not rewrite config.bin. */
static const uint16_t budget_levels[8] = {28, 116, 120, 124, 128, 132, 136, 140};
static uint32_t budget_saved[8];
static uint8_t budget_fx[4];
static int budget_have;
static _Atomic int budget_suspended;

static int budget_tight(void) {
    const char *mode = getenv("PORT_OPTION_MEMORY");
    return mode && (!strcmp(mode, "reduced") || !strcmp(mode, "minimal"));
}

static int budget_is_config(uint64_t self) {
    return self && rd64(self) == UINT64_C(0x141D4F3B8);
}

static void budget_read(uint64_t gfx, uint32_t levels[8], uint8_t fx[4]) {
    for (unsigned i = 0; i < 8; ++i) levels[i] = rd32(gfx + budget_levels[i]);
    for (unsigned i = 0; i < 4; ++i) fx[i] = rd8(gfx + 233u + i);
}

static void budget_write(uint64_t gfx, const uint32_t levels[8], const uint8_t fx[4]) {
    for (unsigned i = 0; i < 8; ++i) wr32(gfx + budget_levels[i], levels[i]);
    for (unsigned i = 0; i < 4; ++i) wr8(gfx + 233u + i, fx[i]);
}

static void budget_apply(uint64_t self) {
    static const uint32_t ultra[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t off[4] = {0, 0, 0, 0};
    if (budget_is_config(self)) budget_write(self + 176, ultra, off);
    if (self != UINT64_C(0x14296E8F0) && budget_is_config(UINT64_C(0x14296E8F0)))
        budget_write(UINT64_C(0x14296E8F0) + 176, ultra, off);
}

static void budget_log(const char *why) {
    fprintf(stderr, "FP_GRAPHICS_BUDGET why=%s saved=%u,%u,%u,%u,%u,%u,%u,%u fx=%u,%u,%u,%u\n",
        why, budget_saved[0], budget_saved[1], budget_saved[2], budget_saved[3], budget_saved[4],
        budget_saved[5], budget_saved[6], budget_saved[7], budget_fx[0], budget_fx[1], budget_fx[2], budget_fx[3]);
}

void fp_graphics_budget_note(uint64_t self, uint64_t target, int after, uint64_t result) {
    int saved = errno;
    if (!budget_tight()) { errno = saved; return; }
    if (!budget_is_config(self)) {
        static int foreign;
        if (!foreign && self) {
            foreign = 1;
            fprintf(stderr, "FP_GRAPHICS_BUDGET skip self=%llx vt=%llx after=%d\n",
                (unsigned long long)self, (unsigned long long)rd64(self), after);
        }
        errno = saved;
        return;
    }
    int saving = target == UINT64_C(0x140E65A50);
    if (!after) {
        if (saving && budget_have) {
            atomic_store_explicit(&budget_suspended, 1, memory_order_relaxed);
            budget_write(self + 176, budget_saved, budget_fx);
            fprintf(stderr, "FP_GRAPHICS_BUDGET save=suspend\n");
        }
        errno = saved;
        return;
    }
    if (saving) {
        if (budget_have) budget_apply(self);
        atomic_store_explicit(&budget_suspended, 0, memory_order_relaxed);
        errno = saved;
        return;
    }
    if ((result & 0xffu) == 0) { errno = saved; return; }
    budget_read(self + 176, budget_saved, budget_fx);
    budget_have = 1;
    budget_log("load");
    budget_apply(self);
    errno = saved;
}

void fp_graphics_budget_tick(void) {
    int saved = errno;
    uint64_t self = UINT64_C(0x14296E8F0);
    if (!budget_tight() || atomic_load_explicit(&budget_suspended, memory_order_relaxed) || !budget_is_config(self)) {
        errno = saved;
        return;
    }
    if (!budget_have) { errno = saved; return; }
    uint64_t gfx = self + 176;
    uint32_t live[8];
    uint8_t fx[4];
    budget_read(gfx, live, fx);
    static int live_logged;
    if (!live_logged) {
        live_logged = 1;
        fprintf(stderr, "FP_GRAPHICS_BUDGET live=%u,%u,%u,%u,%u,%u,%u,%u fx=%u,%u,%u,%u\n",
            live[0], live[1], live[2], live[3], live[4], live[5], live[6], live[7], fx[0], fx[1], fx[2], fx[3]);
    }
    int drift = 0;
    for (unsigned i = 0; i < 8; ++i) if (live[i]) drift = 1;
    for (unsigned i = 0; i < 4; ++i) if (fx[i]) drift = 1;
    if (drift) {
        budget_apply(self);
        static int logged;
        if (!logged) { fprintf(stderr, "FP_GRAPHICS_BUDGET reapply=1\n"); logged = 1; }
    }
    errno = saved;
}

void fp_fx_apply(void) {
    extern void fp_mobile_quality_refresh(void) __attribute__((weak));
    extern void fp_mobile_detail_refresh(void) __attribute__((weak));
    extern void fp_mobile_lod_refresh(void) __attribute__((weak));
    int saved = errno;
    fp_graphics_budget_tick();
    if (fp_mobile_quality_refresh) fp_mobile_quality_refresh();
    if (fp_mobile_detail_refresh) fp_mobile_detail_refresh();
    if (fp_mobile_lod_refresh) fp_mobile_lod_refresh();
    fx_gates();
    fx_property();
    fx_menu();
    errno = saved;
}
#else
void fp_fx_apply(void) {}
#endif
