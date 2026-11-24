#include "mobile_controls.h"
#include "port_settings.h"
#include "text_input.h"
#include "win64.h"
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

/* The UI walks compare node names with dozens of literals per node (thousands of nodes per pass):
 * the first byte is compared inline (a constant for a literal) and strcmp runs only when it matches.
 * Same result as strcmp: the sign of the first differing byte as unsigned char, 0 when equal. */
static inline int mobile_strcmp(const char *a, const char *b) {
    unsigned char x = (unsigned char)a[0], y = (unsigned char)b[0];
    if (x != y) return (int)x - (int)y;
    return x ? strcmp(a + 1, b + 1) : 0;
}
#define strcmp(a, b) mobile_strcmp((a), (b))

enum { CAMERA=1, UI=2, BUILDING=4, TIME=8, ROAD=16 };
typedef struct {float left,top,right,bottom;unsigned flags;} touch_rect;
static pthread_mutex_t input_mutex=PTHREAD_MUTEX_INITIALIZER;
static float pending[4];
static float pending_building_turn;
static unsigned reset_pending, camera_available, building_available;
/* TouchControls "cancel" while a construction tool is active: leave the paint input mode the way the
 * game does.  FrostpunkGameplayInputSystem (instance 143FE8C40) vt[34] 14150CC00 switches its state
 * +0xA8 from 2 (paint) to the default; the paint mode's Exit (141A03910) clears the painter.  Calling
 * FrostpunkPainterManager::ClearPainter (141A05920) alone left the system in paint mode without a
 * painter, and no building could be selected afterwards. */
static unsigned painter_clear_pending;
static unsigned street_tool_pending;
static _Atomic unsigned touch_construction;
/* Construction aim past the camera limits (2026-11-22).  The touch tools aim at the screen centre, and
 * the city camera's target stops at FrostpunkCameraBoundsBase (camera_bounds_clamp), so the strip
 * between those limits and the edge of the map, which a PC cursor reaches at the side of the screen,
 * could not be aimed at.  The part of a construction pan that the limits cut off moves the aim instead:
 * aim_offset is the aim's ground offset (X, Z) from the point under the screen centre, and a pan back
 * brings it home first.  fp_mobile_camera_after keeps the aim on the visible screen; aim_point is the aim
 * in the screen convention of 14181BDB0 (x to the right, y up), read by the building painter's aim.
 * Game thread only. */
static float aim_offset[2];
static float aim_point[4]={.5f,.5f,0,1};
static _Atomic unsigned construction_projected;
static float construction_plane[8];
static uint16_t street_detail[256];
static unsigned street_detail_painter,street_detail_units;
/* Left notifications collapse to their icons and back on a tap on an icon (tap target 2 consumes
 * it).  Each NOTIFICATION / TUTORIAL_NOTIFICATION is BUTTON -> clip -> move(text, close); the clip
 * folds to scale 0 and the BUTTON keeps the icon's width, so the freed strip passes taps to the city. */
static unsigned notifications_collapsed;
/* Continuous pinch zoom (TouchControls action 7; action 2 at the end of every gesture settles it).
 * Between two city zoom levels of FrostpunkGameplayCameraSystem the RTS controller follows the
 * fingers; the gesture ends on the nearest level through the game's own SetZoomLevel. */
static float pending_zoom;
static unsigned zoom_settle_pending,zoom_city_ok,zoom_limit_out;
static touch_rect notify_icons[24];static unsigned notify_icon_count;
static unsigned road_available;
static uint64_t painters,panels;
static float building_gesture;
static _Atomic unsigned debug_requested;
static unsigned pause_menu_visible;
static uint64_t debug_ready_since;
static touch_rect rectangles[512];
static unsigned rectangle_count;
static struct {uint32_t id;touch_rect bounds,hit[2];float x[2],y[2];unsigned enabled,swipe_enabled;} setting_targets[128];
static struct {uint32_t id;touch_rect bounds;float x,y;unsigned enabled;} slider_targets[64];
static unsigned slider_target_count;
static uint64_t law_photo_tap_target(float x,float y);
static unsigned setting_target_count;
static _Atomic unsigned switch_menu;
static _Atomic unsigned switch_available;
static _Atomic unsigned switch_modal_visible;
static _Atomic unsigned switch_mode;
static uint64_t switch_targets[5];
static int switch_button_index(const char *name);
/* Construction controls: the close button; the open menu; the active painter kind
 * (1 building, 2 street, 3 pipe street, 4 demolition, 5 street demolition). */
static uint64_t construction_close_target;
static unsigned construction_open,painter_kind;
static float construction_menu_aim=.5f;
/* Loading-screen tips: FrostpunkUIHintControl (vtable 141EE06E0) is a persistent root of the loading
 * UIScreen; each loading instantiates HINT/TEXT_DESCRIPTION under its HINTS_PARENT (1413B8B00).  The
 * loading screen never runs the UI pass, so the root is scaled beforehand from the menu/city pass. */
static uint64_t loading_hint;static uint32_t loading_hint_id;
/* Video subtitles: VideoSubtitlesSupport (1410048A0) adds its own UIScreen from
 * #LiquidEngine/UI/SubtitlesPanel while a video plays; the text is FRAME/Subtitles. */
static uint64_t subtitle_text;static uint32_t subtitle_text_id;
static int construction_button_index(const char *name);
static touch_rect time_rectangle;
static unsigned time_rectangle_valid,time_state_valid,current_speed;
static int pending_speed=-1;
static _Atomic uint64_t saved_controller;
static float initial_position[4],initial_target[4],initial_rotation[4],initial_distance;
static _Atomic unsigned applied_scale;
static struct { uint64_t root; uint32_t id; } scaled_roots[512];
static unsigned scaled_count;
static struct { uint64_t node; uint32_t id; unsigned axes; } partial_canvases[512];
static unsigned partial_count;
static struct { uint64_t node; uint32_t id; float y; unsigned translated,scale,seen; } mobile_docked[4096];
static unsigned mobile_docked_count;
static unsigned ui_pass;
static struct { uint64_t node; uint32_t id; float x,y; } fitted_panels[1024];
static unsigned fitted_count;
static struct {uint64_t node;uint32_t id;unsigned axes;} backgrounds[512];
static unsigned background_count;
static unsigned placing_decoration;
typedef struct {uint64_t node;uint32_t id;float x,y,sx,sy,w,h;unsigned dock;float px,py;unsigned input;} mobile_layout;
static mobile_layout mobile_layouts[4096];
static unsigned mobile_layout_count;
static void snapshot_ui(void);
/* Touch activity (JNI queries of the touch layer), for the UI snapshot rate. */
static _Atomic uint64_t touch_activity_ns;
static uint64_t monotonic_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;}
static void touch_activity(void){atomic_store_explicit(&touch_activity_ns,monotonic_ns(),memory_order_relaxed);}
static void parent_transform(uint64_t node,float *x,float *y,float *sx,float *sy);
static unsigned map_gate_next_opens(void);
static void selection_tab_fit(CPU *cpu,uint64_t node);

/* (node, id) -> index of the append-only UI tracking arrays above. Their entries never leave,
 * and the UI pass used to scan them linearly for every node of every frame, so its cost grew
 * with the session. Entries are unique per (node, id), so the index returns the same entry the
 * scan found. Node 0 marks an empty slot; tables stay at most half full. */
typedef struct { uint64_t node; uint32_t id; int32_t index; } ui_index_slot;
static ui_index_slot docked_index[8192],partial_index[1024],scaled_index[1024],fitted_index[2048],background_index[1024];
static ui_index_slot mobile_layout_index[8192];
static unsigned ui_index_hash(uint64_t node,uint32_t id){
    return (unsigned)(((node^((uint64_t)id<<32)^id)*UINT64_C(0x9E3779B97F4A7C15))>>40);
}
static int ui_index_find(const ui_index_slot *table,unsigned mask,uint64_t node,uint32_t id){
    if(!node)return -1;
    for(unsigned h=ui_index_hash(node,id)&mask;table[h].node;h=(h+1)&mask)
        if(table[h].node==node&&table[h].id==id)return table[h].index;
    return -1;
}
static void ui_index_add(ui_index_slot *table,unsigned mask,uint64_t node,uint32_t id,int index){
    if(!node)return;
    unsigned h=ui_index_hash(node,id)&mask;
    while(table[h].node){if(table[h].node==node&&table[h].id==id)return;h=(h+1)&mask;}
    table[h].id=id;table[h].index=index;table[h].node=node;
}

static int original_dialog(uint64_t node){
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    /* Every name tested below starts with B, I, E or F; skip the strcmp chain for the rest. */
    if(!name||(name[0]!='B'&&name[0]!='I'&&name[0]!='E'&&name[0]!='F'))return 0;
    // The menu's animated background is one composition; scaling its layers separately exposes their edges.
    if((!strcmp(name,"BACKGROUND")||!strcmp(name,"INGAME_MENU_OVERLAY"))&&rd64(node)==GVA(0x141D78A60)
       &&fabsf(rdss(node+0x210)-1280)<.01f&&fabsf(rdss(node+0x214)-720)<.01f)return 1;
    if(!strncmp(name,"EXT_",4)&&fabsf(rdss(node+0x210)-1280)<.01f&&fabsf(rdss(node+0x214)-720)<.01f
       &&strcmp(name,"EXT_SOCIOTECH_PANEL")&&strcmp(name,"EXT_DILEMMA_SMALL_PANEL"))return 1;
    return !strncmp(name,"EXT_MESSAGE_",12)||!strcmp(name,"EXT_POPULATION_PANEL")||!strcmp(name,"EXT_SELECTION_PANEL")
        ||!strcmp(name,"EXT_RESOURCES_PANEL")||!strcmp(name,"EXT_SOCIETY_STATE_PANEL")||!strcmp(name,"EXT_SEQUENCE_SOCIETY_STATE_PANEL")
        ||!strcmp(name,"FEEDBACK_PANEL")||!strcmp(name,"EXT_CHEAT_PANEL")||!strncmp(name,"EXT_DEBUG_",10)
        ||!strcmp(name,"EXT_TECHNOLOGY_PANEL")||!strcmp(name,"EXT_SEARCH_CITIZENS_PANEL")
        ||!strcmp(name,"EXT_EXPEDITION_CONFIRMATION_PANEL");
}
static int inside_original_dialog(uint64_t node){
    for(unsigned depth=0;node&&depth<40;++depth,node=rd64(node+0x230))
        if(original_dialog(node))return 1;
    return 0;
}

static unsigned ui_visible_chain(uint64_t node){
    for(unsigned depth=0;node&&depth<40;++depth,node=rd64(node+0x230))
        if(rd32(node+0x1e0)&2)return 0;
    return node==0;
}

/* Drawn, not only laid out: a closing panel keeps its hidden flag clear while its show/hide state
 * (+0x30, +0x34) already says hidden (the construction menu after its close animation). */
static unsigned ui_shown_chain(uint64_t node){
    for(unsigned depth=0;node&&depth<40;++depth,node=rd64(node+0x230))
        if((rd32(node+0x30)|rd32(node+0x1e0)|rd8(node+0x34))&2)return 0;
    return node==0;
}

static uint64_t ui_owner_screen(uint64_t node){
    // Text editors can leave +248 empty; their parent chain still owns the
    // native hit rectangles. A real owner remains authoritative when present.
    for(unsigned depth=0;node&&depth<40;++depth,node=rd64(node+0x230)){
        if(rd64(node)==GVA(0x141D7B0B0))return node;
        uint64_t owner=rd64(node+0x248);
        if(owner&&rd64(owner)==GVA(0x141D7B0B0))return owner;
    }
    return 0;
}

void fp_mobile_painters_created(CPU *cpu,void (*original)(CPU*)) {
    uint64_t object=cpu->rcx;original(cpu);painters=object;
}
void fp_mobile_building_aim(CPU *cpu,void (*original)(CPU*)) {
    if(!atomic_load(&touch_construction)){original(cpu);return;}
    /* Preview, rotation and placement read the same aim, independently of the HUD pointer: the screen
     * centre, or where a pan past the camera limits moved it (aim_point). */
    _Alignas(16) float aim[4];memcpy(aim,aim_point,sizeof(aim));
    uint64_t input=cpu->rdx;
    cpu->rdx=(uintptr_t)aim;
    original(cpu);
    if(cpu->rdx==(uintptr_t)aim)cpu->rdx=input;
}
void fp_mobile_panels_created(CPU *cpu,void (*original)(CPU*)) {
    uint64_t object=cpu->rcx;original(cpu);panels=object;
    fprintf(stderr,"FP_PANEL_MANAGER object=%llx vtable=%llx enabled=%u\n",(unsigned long long)object,(unsigned long long)(rd64(object)-g_image_delta),rd8(object+0x3f1));
}
void fp_mobile_debug_menu_request(void){atomic_store(&debug_requested,1);}

__attribute__((visibility("default"))) int frostpunk_dxvk_scene_active(void){return atomic_load(&saved_controller)!=0;}

__attribute__((visibility("default"))) int frostpunk_mobile_submit(unsigned action,float dx,float dy,float zoom,float angle) {
    touch_activity();
    if(action>9 || !isfinite(dx)||!isfinite(dy)||!isfinite(zoom)||!isfinite(angle)
       ||fabsf(dx)>4||fabsf(dy)>4||fabsf(zoom)>4||fabsf(angle)>3.141593f)return 0;
    /* Triggers and the farthest city level use the original, bounded wheel command; pinches send a
     * continuous zoom (action 7). */
    if(zoom!=0&&action!=7)return 0;
    if(action==4&&(dx<0||dx>3||floorf(dx)!=dx))return 0;
    if(action==5&&(dx<0||dx>1||floorf(dx)!=dx||dy||angle))return 0;
    if(action==8){
        if((dx!=0&&dx!=1)||dy||angle)return 0;
        atomic_store(&touch_construction,(unsigned)dx);return 1;
    }
    if(action==9&&((dx!=2&&dx!=4)||dy||angle))return 0;
    pthread_mutex_lock(&input_mutex);
    if(action==5){
        if(!frostpunk_switch_ui()||!atomic_load(&switch_available)){pthread_mutex_unlock(&input_mutex);return 0;}
        atomic_store(&switch_menu,(unsigned)dx);memset(pending,0,sizeof(pending));
    }
    else if(action==2){memset(pending,0,sizeof(pending));reset_pending=0;pending_building_turn=0;pending_speed=-1;atomic_store(&switch_menu,0);
        pending_zoom=0;zoom_settle_pending=1;}
    else if(action==7){
        if(!zoom_city_ok||dx||dy||angle){pthread_mutex_unlock(&input_mutex);return 0;}
        pending_zoom=fmaxf(-4,fminf(4,pending_zoom+zoom));
    }
    else if(action==1){reset_pending=1;memset(pending,0,sizeof(pending));}
    else if(action==3)pending_building_turn=fmaxf(-3.141593f,fminf(3.141593f,pending_building_turn+angle));
    else if(action==4)pending_speed=(int)dx;
    else if(action==6){
        if(!painter_kind){pthread_mutex_unlock(&input_mutex);return 0;}
        painter_clear_pending=1;
    }
    else if(action==9)street_tool_pending=(unsigned)dx;
    else for(unsigned i=0;i<4;++i){float value=(float[]){dx,dy,zoom,angle}[i];pending[i]=fmaxf(-4,fminf(4,pending[i]+value));}
    pthread_mutex_unlock(&input_mutex);return 1;
}

__attribute__((visibility("default"))) unsigned frostpunk_mobile_context(float x,float y) {
    touch_activity();
    if(!isfinite(x)||!isfinite(y)||x<0||x>1||y<0||y>1)return 0;
    // IsLoadingScreenActive: world UI exists before the loading screen has finished.
    if(!applied_scale||__atomic_load_n((uint8_t*)(uintptr_t)GVA(0x142982F8D),__ATOMIC_ACQUIRE))return 2048;
    pthread_mutex_lock(&input_mutex);
    unsigned result=(camera_available?CAMERA:0)|(building_available?BUILDING:0)|(road_available?ROAD:0);
    if(time_state_valid)result|=256|(current_speed<<9);
    for(unsigned i=0;i<rectangle_count;++i){touch_rect r=rectangles[i];
        if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)result|=UI|r.flags;}
    if(camera_available&&time_rectangle_valid&&x>=time_rectangle.left&&x<=time_rectangle.right
       &&y>=time_rectangle.top&&y<=time_rectangle.bottom)result|=TIME|UI;
    for(unsigned i=0;i<setting_target_count;++i){touch_rect r=setting_targets[i].bounds;
        if(setting_targets[i].enabled&&x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)result|=8192|UI;
    }
    for(unsigned i=0;i<slider_target_count;++i){touch_rect r=slider_targets[i].bounds;
        if(slider_targets[i].enabled&&x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)result|=64|UI;
    }
    if(frostpunk_switch_ui())result|=1u<<24;
    if(atomic_load(&switch_menu))result|=(1u<<25)|UI;
    if(atomic_load(&switch_available))result|=1u<<26;
    if(camera_available&&construction_open)result|=1u<<28;
    if(camera_available)result|=(painter_kind&7u)<<29;
    /* The next zoom-out notch enters the world map.  Not tied to camera_available: that is false while a
     * level transition animates, and the following notch arrived exactly then (2->3->4 in one pinch);
     * SetZoomLevel (1412537F0) stores the new index at once, so the check is valid mid-transition. */
    if(map_gate_next_opens())result|=1u<<27;
    if(zoom_limit_out)result|=128;   /* a pinch pushes out past the farthest city level */
    pthread_mutex_unlock(&input_mutex);
    return result|(fp_port_setting_get(FP_SETTING_TOUCH_OFFSET)<<16);
}

static uint64_t touch_point(float x,float y){
    uint32_t a,b;memcpy(&a,&x,4);memcpy(&b,&y,4);return ((uint64_t)a<<32)|b;
}

__attribute__((visibility("default"))) uint64_t frostpunk_mobile_swipe_target(float x,float y,int direction) {
    touch_activity();
    if(!isfinite(x)||!isfinite(y)||x<0||x>1||y<0||y>1||direction<-1||direction>11)return 0;
    uint64_t result=0;float nearest=INFINITY;
    pthread_mutex_lock(&input_mutex);
    if(direction>=8){
        result=direction==11&&camera_available?construction_close_target:0;
        pthread_mutex_unlock(&input_mutex);return result;
    }
    if(direction>=3){
        result=frostpunk_switch_ui()&&atomic_load(&switch_available)&&atomic_load(&switch_menu)?switch_targets[direction-3]:0;
        pthread_mutex_unlock(&input_mutex);return result;
    }
    if(direction==0){
        for(unsigned i=0;i<slider_target_count;++i){touch_rect r=slider_targets[i].bounds;
            if(slider_targets[i].enabled&&x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom){
                result=touch_point(slider_targets[i].x,slider_targets[i].y);break;
            }
        }
    } else for(unsigned i=0;i<setting_target_count;++i){
        for(unsigned side=0;side<2;++side){
            if(direction!=2&&side!=(unsigned)(direction>0))continue;
            touch_rect r=direction==2?setting_targets[i].hit[side]:setting_targets[i].bounds;
            if(x<r.left||x>r.right||y<r.top||y>r.bottom||!(setting_targets[i].enabled&(1u<<side)))continue;
            float distance=hypotf((x-setting_targets[i].x[side])*1280,(y-setting_targets[i].y[side])*720);
            if(distance<nearest){nearest=distance;result=direction==2||(setting_targets[i].swipe_enabled&(1u<<side))
                ?touch_point(setting_targets[i].x[side],setting_targets[i].y[side]):0;}
        }
    }
    if(direction==2&&!result)
        for(unsigned i=0;i<notify_icon_count;++i){touch_rect r=notify_icons[i];
            if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom){
                notifications_collapsed^=1u;result=UINT64_MAX;
                fprintf(stderr,"FP_NOTIFICATIONS collapsed=%u\n",notifications_collapsed);break;
            }
        }
    if(direction==2&&!result)result=law_photo_tap_target(x,y);
    pthread_mutex_unlock(&input_mutex);return result;
}

static void intro_videos(uint64_t screen);
void fp_mobile_ui_scale(void) {
    intro_videos(0);
    unsigned requested=fp_port_setting_get(FP_SETTING_UI_SCALE);
    // Menus retain one reference canvas for layout, particle masks and their camera; only the HUD is enlarged.
    unsigned scale=!atomic_load(&saved_controller)||pause_menu_visible?100:requested;
    if(scale==applied_scale)return;
    float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
    if(width<=0||height<=0)return;
    float previous=applied_scale?applied_scale:100;
    wrss(GVA(0x142980E60),width*previous/scale);
    wrss(GVA(0x142980E64),height*previous/scale);
    applied_scale=scale;
    fprintf(stderr,"FP_UI_SCALE value=%u requested=%u canvas=%.2fx%.2f\n",scale,requested,rdss(GVA(0x142980E60)),rdss(GVA(0x142980E64)));
}

static void call(CPU *cpu,uint64_t function,uint64_t object,const float *vector,float a,float b,float d) {
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);
    win64_pointer(c.rsp-8,256,1);
    c.rcx=object;c.xmm[1].f32[0]=a;c.xmm[2].f32[0]=b;c.xmm[3].f32[0]=d;
    if(vector){c.rdx=c.rsp+0x80;memcpy((void*)(uintptr_t)c.rdx,vector,16);}
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,function);
}

static void call_value(CPU *cpu,uint64_t function,uint64_t object,unsigned value) {
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=object;c.rdx=value;c.r8=0;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,function);
}

static mobile_layout *remember_layout(uint64_t node){
    uint32_t id=rd32(node+0x1e4);int i=ui_index_find(mobile_layout_index,8191u,node,id);
    if(i<0){
        if(mobile_layout_count==4096){static unsigned once;if(!once++)fprintf(stderr,"FP_UI_REGISTRY_FULL layouts=4096\n");return NULL;}
        i=(int)mobile_layout_count++;
        mobile_layouts[i]=(mobile_layout){node,id,rdss(node+0xc0),rdss(node+0xc4),rdss(node+0xf0),rdss(node+0xf4),rdss(node+0x210),rdss(node+0x214),rd32(node+0x138),rdss(node+0xd0),rdss(node+0xd4),rd32(node+0x1e0)&0x10};
        ui_index_add(mobile_layout_index,8191u,node,id,i);
    }
    return &mobile_layouts[i];
}

static void layout_position(CPU *cpu,uint64_t node,float x,float y){
    if(fabsf(rdss(node+0xc0)-x)>.01f||fabsf(rdss(node+0xc4)-y)>.01f){
        float position[4]={x,y,0,1};call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
    }
}

static void layout_scale(CPU *cpu,uint64_t node,float sx,float sy){
    if(fabsf(rdss(node+0xf0)-sx)>.0001f||fabsf(rdss(node+0xf4)-sy)>.0001f){
        float transform[4]={sx,sy,0,1};call(cpu,UINT64_C(0x140EC46E0),node,transform,0,0,0);
    }
}

static float dialog_ime_y(uint64_t ink,float baseline,float height){
    float fraction=fp_mobile_ime_visible_fraction();
    if(fraction>=1)return baseline;
    uint64_t editor=fp_mobile_text_focused_editor(),ancestor=editor;
    for(unsigned depth=0;ancestor&&ancestor!=ink&&depth<40;++depth)ancestor=rd64(ancestor+0x230);
    if(ancestor!=ink)return baseline;
    float x,y,sx,sy,ix,iy,isx,isy;
    parent_transform(editor,&x,&y,&sx,&sy);parent_transform(ink,&ix,&iy,&isx,&isy);
    float bottom=y+sy*(rdss(editor+0xc4)+(rdss(editor+0xd4)+rdss(editor+0x104))*rdss(editor+0xf4));
    bottom+=(baseline-rdss(ink+0xc4))*isy;
    if(!isfinite(bottom)||!isfinite(isy)||isy<=0)return baseline;
    return baseline-fmaxf(0,bottom-height*fraction+8)/isy;
}

static unsigned hud_bottom_panel(uint64_t node){
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    uint64_t parent=rd64(node+0x230);
    const char *parent_name=parent?(const char*)(uintptr_t)rd64(parent+0x1d8):NULL;
    return name&&!strcmp(name,"Bottom_Panel")&&parent_name
        &&(!strcmp(parent_name,"BOTTOM")||!strcmp(parent_name,"BOTTOM_static"))
        &&fabsf(rdss(node+0x214)-208)<.01f;
}

__attribute__((weak)) float frostpunk_display_aspect(void);
/* Canvas x of the screen's right edge for centre-docked HUD elements. */
static float screen_canvas_right(void){
    float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
    float aspect=frostpunk_display_aspect?frostpunk_display_aspect():16.f/9.f;
    float full=height*aspect;return full>width?width+(full-width)*.5f:width;
}
/* Technology/newsflash notices: the PC column (x 622, 450 wide) leaves their right third off a phone
 * at large scales; keep the whole notice 8 units inside the screen edge. */
static float newsflash_x(void){
    float x=622+(fp_port_setting_get(FP_SETTING_UI_SCALE)>100?(rdss(GVA(0x142980E60))-1280)*.5f:0);
    return applied_scale>100?fminf(x,screen_canvas_right()-458):x;
}
static unsigned newsflash_panel(uint64_t node){
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    uint64_t parent=rd64(node+0x230);
    const char *parent_name=parent?(const char*)(uintptr_t)rd64(parent+0x1d8):NULL;
    return name&&!strcmp(name,"EXT_NEWSFLASH_NOTIFICATION_PANEL")&&parent_name&&!strcmp(parent_name,"TOP")
        &&rd64(node)==GVA(0x141D78A60)&&fabsf(rdss(node+0x210)-450)<.01f&&fabsf(rdss(node+0x214)-100)<.01f;
}

/* Height, in a node's own units, of what its visible subtree draws.  Layout-only nodes (containers,
 * flows, tab controllers and tab pages) report their children, so a stretched empty box adds nothing;
 * text, tables, scroll views and clipping boxes their own box; pictures and buttons both. */
static float visual_bottom(uint64_t node,unsigned depth,unsigned *budget);
static float visual_extent(uint64_t node,unsigned depth,unsigned *budget){
    uint64_t type=rd64(node);unsigned flags=rd32(node+0x1e0);float own=rdss(node+0x104);
    if(!isfinite(own)||own<0||own>2000)own=0;
    unsigned leaf=type==GVA(0x141DED7D8)||type==GVA(0x141D796F8)||type==GVA(0x141DE15E0)||type==GVA(0x1420C25C8)||(flags&0x4000000u);
    if(leaf||!rd64(node+0x220)||depth>=8)return own;
    unsigned layout=type==GVA(0x141D78A60)||type==GVA(0x141DE0A48)||type==GVA(0x141EDFC48)||type==GVA(0x141EDF7C8);
    return fmaxf(layout?0:own,visual_bottom(node,depth+1,budget));
}
static float visual_bottom(uint64_t node,unsigned depth,unsigned *budget){
    float bottom=0;unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<256&&*budget;child=rd64(child+0x240)){
        --*budget;
        if(rd32(child+0x1e0)&2)continue;
        float b=rdss(child+0xf4);if(!isfinite(b)||b<=0||b>8)continue;
        // Desktop-viewport picture templates (SFX_INTERIOR 1281x718) report the PC canvas, not a picture.
        if(rd64(child)==GVA(0x141D796F8)&&rdss(child+0x104)>=700)continue;
        float edge=rdss(child+0xc4)+rdss(child+0xd4)*b+visual_extent(child,depth,budget)*b;
        if(isfinite(edge)&&edge<2000)bottom=fmaxf(bottom,edge);
    }
    return bottom;
}
/* Two-column selection panel at large UI scales: one panel scale for every building and tab.  A frame
 * taller than the standard one hangs from the top bar over the bottom HUD; only pages that do not fit
 * the screen even then are scaled down (selection_tab_fit). */
static float selection_limit=INFINITY,selection_limit_standard=INFINITY,selection_pad_bottom;
/* The HUD draws its bottom bar and population box after RIGHT, i.e. over the panel: while a tall
 * frame reaches into that strip they are folded to scale 0 (as phone_hud_visibility does). */
static unsigned selection_cover_pass;
static float selection_standard_scale(float width,float height){
    return fminf(1,fminf((width-24)/684.f,(height-132)/241.f));
}
static float selection_frame_top(float width,float height,float frame){
    float standard=241*selection_standard_scale(width,height);
    return height-76-fminf(frame,standard);
}

#include "mobile_selection_columns.inc"

static unsigned construction_widget(uint64_t node) {
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);if(!name)return 0;
    unsigned kind=!strcmp(name,"BUILDING_SCROLL_PANE")?1:!strcmp(name,"left_button_clip")?2:!strcmp(name,"right_button_clip")?3:0;
    if(!kind)return 0;
    for(uint64_t parent=rd64(node+0x230),depth=0;parent&&depth<7;++depth,parent=rd64(parent+0x230)){
        const char *ancestor=(const char*)(uintptr_t)rd64(parent+0x1d8);
        if(ancestor&&!strcmp(ancestor,"EXT_CONSTRUCTION_PANEL"))return kind;
    }
    return 0;
}

static unsigned small_event_mask(uint64_t node) {
    if(rd64(node)!=GVA(0x141D796F8)||fabsf(rdss(node+0x210))>.01f)return 0;
    uint64_t parent=rd64(node+0x230);if(!parent)return 0;
    const char *name=(const char*)(uintptr_t)rd64(parent+0x1d8);
    return name&&!strcmp(name,"EXT_DILEMMA_SMALL_PANEL");
}

static unsigned law_widget(uint64_t node) {
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);if(!name)return 0;
    unsigned kind=!strcmp(name,"CATEGORIES_LAYOUT")?1:!strcmp(name,"INFO_PANEL")?2:
        (!strcmp(name,"TreeClip")||!strcmp(name,"SOCIOTECHTREE"))?3:!strcmp(name,"CUSTOM_CLOSE_BUTTON")?4:
        !strcmp(name,"top")?5:0;
    if(!kind)return 0;
    for(uint64_t parent=rd64(node+0x230),depth=0;parent&&depth<8;++depth,parent=rd64(parent+0x230)){
        const char *ancestor=(const char*)(uintptr_t)rd64(parent+0x1d8);
        if(ancestor&&!strcmp(ancestor,"EXT_SOCIOTECH_PANEL"))return kind;
    }
    return 0;
}

static void parent_transform(uint64_t node,float *x,float *y,float *sx,float *sy) {
    uint64_t chain[40];unsigned count=0;
    for(uint64_t parent=rd64(node+0x230);parent&&count<40;parent=rd64(parent+0x230))chain[count++]=parent;
    *x=*y=0;*sx=*sy=1;
    while(count){uint64_t parent=chain[--count];float a=rdss(parent+0xf0),b=rdss(parent+0xf4);
        if(!isfinite(a)||!isfinite(b)||a<=0||b<=0)continue;
        *x+=*sx*(rdss(parent+0xc0)+rdss(parent+0xd0)*a);
        *y+=*sy*(rdss(parent+0xc4)+rdss(parent+0xd4)*b);
        *sx*=a;*sy*=b;
    }
}

static void technology_bounds(uint64_t node,unsigned depth,float x,float y,float sx,float sy,float bounds[4]) {
    if(depth>3||(rd32(node+0x1e0)&2))return;
    float a=rdss(node+0xf0),b=rdss(node+0xf4);
    if(!isfinite(a)||!isfinite(b)||a<=0||b<=0)return;
    x+=sx*(rdss(node+0xc0)+rdss(node+0xd0)*a);y+=sy*(rdss(node+0xc4)+rdss(node+0xd4)*b);
    sx*=a;sy*=b;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    if(name&&(!strcmp(name,"Background")||!strcmp(name,"TECHTREE_PARENT")||!strcmp(name,"CITY_RESOURCES_LAYOUT")
       ||!strcmp(name,"CLOSE_BUTTON")||!strcmp(name,"TITLE"))){
        bounds[0]=fminf(bounds[0],x);bounds[1]=fminf(bounds[1],y);
        bounds[2]=fmaxf(bounds[2],x+rdss(node+0x100)*sx);bounds[3]=fmaxf(bounds[3],y+rdss(node+0x104)*sy);
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240))
        technology_bounds(child,depth+1,x,y,sx,sy,bounds);
}

static void mobile_message_wrap(CPU *cpu,uint64_t text,float width,float height,unsigned limit);
static float mobile_message_height(uint64_t text);
#include "mobile_law_layout.inc"
#include "mobile_overlay_text.inc"

void fp_mobile_show_feedback(CPU *cpu,void (*original)(CPU*)){
    uint64_t message=cpu->rdx,detail=0;
    unsigned kind=atomic_load(&touch_construction)&&message?street_mode_message(rd64(message),&detail):0;
    if(!kind){
        original(cpu);return;
    }
    unsigned units=0;
    while(units<sizeof(street_detail)/sizeof(*street_detail)&&rd16(detail+2*units))++units;
    pthread_mutex_lock(&input_mutex);
    memcpy(street_detail,(const void*)(uintptr_t)detail,units*sizeof(*street_detail));
    street_detail_units=units;street_detail_painter=kind;
    pthread_mutex_unlock(&input_mutex);
    /* The feedback method consumes its string even when the message stays on the touch buttons. */
    call(cpu,UINT64_C(0x140E20F10),message,NULL,0,0,0);
    wr64(message,0);
    cpu->rip=pop64(cpu);
}
#include "mobile_selection_background.inc"
#include "mobile_phone_panels.inc"

typedef struct {uint64_t node;uint32_t id,flags,mode;} technology_saved;
static technology_saved technology_states[64];
static unsigned technology_state_count;

static technology_saved *technology_remember(uint64_t node){
    uint32_t id=rd32(node+0x1e4);
    for(unsigned i=0;i<technology_state_count;++i)
        if(technology_states[i].node==node&&technology_states[i].id==id)return &technology_states[i];
    if(technology_state_count==64)return NULL;
    technology_saved *saved=&technology_states[technology_state_count++];
    *saved=(technology_saved){.node=node,.id=id,.flags=rd32(node+0x1e0)&0x04000010,.mode=rd32(node+0x370)};
    return saved;
}

static void technology_restore(CPU *cpu,uint64_t node){
    uint32_t id=rd32(node+0x1e4);
    for(unsigned i=0;i<technology_state_count;++i){technology_saved *saved=&technology_states[i];
        if(saved->node!=node||saved->id!=id)continue;
        uint32_t current=rd32(node+0x1e0)&0x04000010;
        if(saved->flags&~current)call_value(cpu,UINT64_C(0x140EC0F60),node,saved->flags&~current);
        if(current&~saved->flags)call_value(cpu,UINT64_C(0x140EBE470),node,current&~saved->flags);
        wr32(node+0x370,saved->mode);
        break;
    }
}

static unsigned technology_tab_index(uint64_t tab){
    unsigned index=0;uint64_t parent=rd64(tab+0x230);
    for(uint64_t child=parent?rd64(parent+0x220):0;child&&child!=tab&&index<128;child=rd64(child+0x240))
        if(law_photo_name(child)&&!strcmp(law_photo_name(child),"TAB_BUTTON"))++index;
    return index;
}

typedef struct {uint64_t controller,pane,key[2];uint32_t id;unsigned seen,mobile;} technology_focus_saved;
static technology_focus_saved technology_focus_states[16];
static unsigned technology_focus_count,technology_focus_pass;
static void technology_focus(CPU *cpu,uint64_t button,float width,float height){
    if(technology_focus_pass==ui_pass)return;
    technology_focus_pass=ui_pass;
    uint64_t controller=mobile_button_controller(button,UINT64_C(0x141A68920));
    if(!controller||rd64(controller)!=GVA(0x14212EEE0))return;
    uint64_t reference=rd64(controller+0x188),pane=reference?rd64(reference):0;
    if(!pane||rd64(pane)!=GVA(0x141DA74C8))return;
    uint32_t id=rd32(pane+0x1e4);unsigned i=0;
    for(;i<technology_focus_count;++i)if(technology_focus_states[i].controller==controller&&technology_focus_states[i].pane==pane&&technology_focus_states[i].id==id)break;
    if(i==technology_focus_count){if(i==16)return;technology_focus_states[technology_focus_count++]=(technology_focus_saved){.controller=controller,.pane=pane,.id=id};}
    technology_focus_saved *saved=&technology_focus_states[i];unsigned mobile=applied_scale>100;
    unsigned changed=!saved->seen||saved->mobile!=mobile||memcmp(saved->key,(void*)(uintptr_t)(controller+0xe0),16);
    memcpy(saved->key,(void*)(uintptr_t)(controller+0xe0),16);saved->seen=1;saved->mobile=mobile;
    uint64_t entries=rd64(controller+0x1a8);unsigned count=rd32(controller+0x1b0);
    if(!entries||count>4096)return;
    uint64_t goals=law_photo_child(rd64(pane+0x230),"GOAL_ENTRIES_SCROLL_PANE");
    uint64_t content=law_photo_child(pane,"CANVAS_CONTENT_PANEL");
    if(mobile&&goals&&content){
        float offset=rdss(content+0xc4);
        // Category rebuilds create unscrolled tier buttons while their pane retains the previous offset.
        for(unsigned n=0;n<count;++n){uint64_t entry=entries+128*n,ref=rd64(entry+16),node=ref?rd64(ref):0;
            if(node&&rd64(node+0x230)==goals)layout_position(cpu,node,rdss(node+0xc0),rdss(entry+52)+offset);
        }
        uint64_t workshop=law_photo_child(goals,"WORKSHOP_ICON");
        if(workshop)layout_position(cpu,workshop,rdss(workshop+0xc0),-rdss(workshop+0xd4)+offset);
    }
    if(!mobile||!changed)return;
    for(unsigned n=0;n<count;++n){uint64_t entry=entries+128*n;
        if(memcmp(saved->key,(void*)(uintptr_t)entry,16))continue;
        float w=(width-92-fminf(210,width*.33f))/.55f,h=(height-136)/.55f;
        // The map record has a 16-byte key before the value returned by 141A5BA30.
        float position[4]={fmaxf(0,rdss(entry+48)-w*.5f),fmaxf(0,rdss(entry+52)-h*.5f),0,0};
        // Replace the PC controller's pending animation; an instant ScrollTo leaves that animation running.
        call(cpu,UINT64_C(0x141095700),pane,position,0,.18f,0);
        break;
    }
}

/* Visibility of purely decorative nodes that the phone layout drops (restored at 100%). */
typedef struct {uint64_t node;uint32_t id;} mobile_hidden_saved;
static mobile_hidden_saved mobile_hidden[128];
static unsigned mobile_hidden_count;
static void mobile_decoration_visible(CPU *cpu,uint64_t node,unsigned visible){
    uint32_t id=rd32(node+0x1e4);unsigned i=0;
    for(;i<mobile_hidden_count;++i)if(mobile_hidden[i].node==node&&mobile_hidden[i].id==id)break;
    if(!visible){
        if(i<mobile_hidden_count||(rd32(node+0x1e0)&2)||mobile_hidden_count==128)return;
        mobile_hidden[mobile_hidden_count++]=(mobile_hidden_saved){node,id};
        call_value(cpu,UINT64_C(0x140EC0F60),node,2);
    } else if(i<mobile_hidden_count){
        if(rd32(node+0x1e0)&2)call_value(cpu,UINT64_C(0x140EBE470),node,2);
        mobile_hidden[i]=mobile_hidden[--mobile_hidden_count];
    }
}

/* Measured ink width of a UIText (its glyph block), falling back to the configured box. */
static float mobile_text_ink(uint64_t text){
    float ink=rdss(text+0x4d0);
    if(!isfinite(ink)||ink<=0)ink=rdss(text+0x210);
    return isfinite(ink)&&ink>0?ink:0;
}

static void technology_contents(CPU *cpu,uint64_t node,unsigned depth,unsigned *budget,float width,float height){
    if(!node||depth>32||!*budget)return;
    --*budget;
    const char *name=law_photo_name(node);
    uint64_t parent=rd64(node+0x230);const char *parent_name=parent?law_photo_name(parent):NULL;
    if(name){
        if(!strcmp(name,"TECH_ENTRY"))technology_focus(cpu,node,width,height);
        if(!strcmp(name,"TECH_NAME"))phone_text_size(cpu,node,12);
        if(!strcmp(name,"ornaments")&&parent_name&&!strcmp(parent_name,"top")&&law_photo_ancestor(node,"EXT_TECHNOLOGY_PANEL"))
            mobile_decoration_visible(cpu,node,applied_scale<=100);  /* desktop swirls run through the left-aligned title */
        if(applied_scale<=100){law_photo_restore(cpu,node);technology_restore(cpu,node);}
        else if(!strcmp(name,"EXT_TECHNOLOGY_PANEL")||(!strcmp(name,"FRAME")&&parent_name&&!strcmp(parent_name,"EXT_TECHNOLOGY_PANEL"))||!strcmp(name,"ANI")){
            if(law_photo_remember(node)){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,width,height);law_photo_place(cpu,node,0,0,1,1);}
        } else if(!strcmp(name,"Background")&&parent_name&&!strcmp(parent_name,"ANI"))law_photo_place(cpu,node,4,44,(width-8)/1250,(height-52)/583);
        else if(!strcmp(name,"top")&&parent_name&&!strcmp(parent_name,"ANI"))law_photo_place(cpu,node,0,8,1,1);
        else if(!strcmp(name,"TITLE")&&parent_name&&!strcmp(parent_name,"top"))law_photo_place(cpu,node,18,10,.82f,.82f);
        else if(!strcmp(name,"CLOSE_BUTTON")&&parent_name&&!strcmp(parent_name,"top"))law_photo_place(cpu,node,width-144,8,.8f,.8f);
        else if(!strcmp(name,"CITY_RESOURCES_LAYOUT"))law_photo_place(cpu,node,20,height-49,.6f,.6f);
        else if(!strcmp(name,"Table_stroke")&&parent_name&&!strcmp(parent_name,"middle")&&law_photo_ancestor(node,"TIER_PANEL")){
            /* The tier separators are one-texel lines: at a reduced output scale a scrolled line can fall
             * between two pixels and fade.  The pane draws the stroke twice; offset the second copy by
             * about one output pixel so that one of them always covers a whole pixel row. */
            uint64_t first=law_photo_child(parent,"Table_stroke");
            law_photo_saved *base=law_photo_remember(node);
            if(base&&first&&first!=node)layout_position(cpu,node,base->base.x,base->base.y+.5f);
        }
        else if(!strcmp(name,"TECHTREE_PARENT")){
            if(law_photo_remember(node)){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,width-16,height-108);law_photo_place(cpu,node,8,54,1,1);}
        } else if(!strcmp(name,"TABS_LAYOUT")&&parent_name&&!strcmp(parent_name,"TECHTREE_PARENT")){
            law_photo_remember(node);law_photo_size(cpu,node,width-16,40);law_photo_place(cpu,node,8,48,1,1);
        } else if(!strcmp(name,"TAB_BUTTON")&&parent_name&&!strcmp(parent_name,"TABS_LAYOUT")){
            float cell=(width-16)/4;law_photo_remember(node);law_photo_size(cpu,node,cell-4,40);
            law_photo_place(cpu,node,8+technology_tab_index(node)*cell,48,1,1);
        } else if(!strcmp(name,"clip")&&parent_name&&!strcmp(parent_name,"TAB_BUTTON")){
            float cell=(width-16)/4;law_photo_remember(node);law_photo_size(cpu,node,cell-4,42);
            law_photo_place(cpu,node,8+technology_tab_index(parent)*cell,48,1,1);
        } else if(!strcmp(name,"NAME_TEXT")&&law_photo_ancestor(node,"TAB_BUTTON")){
            uint64_t tab=law_photo_ancestor(node,"TAB_BUTTON");float cell=(width-16)/4;
            if(law_photo_remember(node))mobile_message_wrap(cpu,node,(cell-14)/.95f,0,0);
            /* One line under the tab's diamond; long category names shrink to the tab instead of overflowing it. */
            float ink=mobile_text_ink(node),scale=.95f;
            if(ink>0&&ink*scale>cell-16)scale=fmaxf(.62f,(cell-16)/ink);
            float line=fmaxf(12,fminf(24,rdss(node+0x104)));
            // The glyphs are centred inside the wrap width set above, not the 127-unit node box:
            // centre that wrap box on the tab (centring the ink pushed short names 48 units right).
            float wrap=(cell-14)/.95f;
            law_photo_place(cpu,node,8+technology_tab_index(tab)*cell+(cell-4)*.5f-wrap*scale*.5f,84-line*scale,scale,scale);
        } else if((!strcmp(name,"TECHTREE")||!strcmp(name,"GOAL_ENTRIES_SCROLL_PANE"))&&rd64(node)==GVA(0x141DA74C8)){
            if(law_photo_remember(node)&&technology_remember(node)){
                uint64_t card=law_photo_child(parent,"DETAILS_WINDOW");
                float details=card&&ui_visible_chain(card)?fminf(210,width*.33f):0;
                // Both native panes share scroll offsets. Scale the panes, not their moving children.
                law_photo_unanchor(cpu,node);law_photo_size(cpu,node,(!strcmp(name,"TECHTREE")?width-92-details:66)/.55f,(height-136)/.55f);
                law_photo_place(cpu,node,!strcmp(name,"TECHTREE")?78:8,94,.55f,.55f);
                // The PC controller disables dragging and the tree has no mouse-focus flag; enable the native handler and clip both viewports.
                uint32_t missing=0x04000010&~rd32(node+0x1e0);
                if(missing)call_value(cpu,UINT64_C(0x140EC0F60),node,missing);
                wr32(node+0x370,1);
            }
        } else if((!strcmp(name,"GOAL_ENTRY")||!strcmp(name,"WORKSHOP_ICON"))&&parent_name&&!strcmp(parent_name,"GOAL_ENTRIES_SCROLL_PANE")){
            law_photo_saved *base=law_photo_remember(node);
            if(base)layout_position(cpu,node,base->base.x+30,rdss(node+0xc4));
        } else if(!strcmp(name,"DETAILS_WINDOW")){
            uint64_t main=law_photo_child(node,"MAIN_LAYOUT"),info=main?law_photo_child(main,"INFO_PANE_LAYOUT"):0;
            float content=info?rdss(info+0x104):520;
            if(!isfinite(content)||content<=0)content=520;
            float factor=fminf(fminf(210,width*.33f)/320,(height-140)/(content+12));
            law_photo_place(cpu,node,width-12-320*factor,94,factor,factor);
            // Native INFO_PANE is vertically centred inside a 520px placeholder; fit the actual dynamic card instead.
            if(info&&law_photo_remember(info))layout_position(cpu,info,rdss(info+0xc0),0);
        } else if(!strcmp(name,"TOTAL_PROGRESS")){
            /* Right-aligned on the resource row, inside the frame's bottom border. */
            // The label centres its glyphs in its 200-unit box: end the glyphs, not the box, at the border.
            float ink=mobile_text_ink(node),box=rdss(node+0x100),scale=.72f;
            if(!(box>0))box=200;
            if(!(ink>0)||ink>box)ink=box;
            law_photo_place(cpu,node,width-26-(box+ink)*.5f*scale,height-48,scale,scale);
        }
        else if(!strcmp(name,"ALL_EMPLOYEES_TEXT"))law_photo_place(cpu,node,width-230,height-66,.5f,.5f);
        else if(!strcmp(name,"HORIZONTAL_SCROLL_BAR")||!strcmp(name,"VERTICAL_SCROLL_BAR")){
            if(law_photo_remember(node))layout_scale(cpu,node,0,0);
        } else if(!strcmp(name,"bottom")&&parent_name&&!strcmp(parent_name,"ANI"))law_photo_place(cpu,node,0,height-42,1,1);
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))technology_contents(cpu,child,depth+1,budget,width,height);
}

typedef struct {uint64_t root;uint32_t id,ignore[2];float wrap[2][2],actual[9][2];unsigned char limit[2];unsigned active;} mobile_scouts_saved;
static mobile_scouts_saved mobile_scouts[16];
static unsigned mobile_scouts_count;

static void mobile_message_wrap(CPU *cpu,uint64_t text,float width,float height,unsigned limit){
    if(rd8(text+0x40e)||!isfinite(width)||width<0||!isfinite(height)||height<0)return;
    if(fabsf(rdss(text+0x37c)-width)<.01f&&fabsf(rdss(text+0x380)-height)<.01f&&rd8(text+0x384)==limit)return;
    // UIText's own Begin/EndUpdate rebuilds glyphs and measured height with the new width, preserving localization and markup.
    call_value(cpu,UINT64_C(0x140FB3C50),text,0);
    wrss(text+0x37c,width);wrss(text+0x380,height);wr8(text+0x384,(unsigned char)limit);
    call_value(cpu,UINT64_C(0x140FB4090),text,0);
}

static float mobile_message_height(uint64_t text){
    uint64_t string=rd64(text+0x3e8);
    if(!string||!rd16(string)||!ui_visible_chain(text))return 0;
    float measured=rdss(text+0x4d4);
    return isfinite(measured)&&measured>0?measured:fmaxf(0,rdss(text+0x104));
}

static void mobile_message_size(CPU *cpu,uint64_t node,float width,float height){
    law_photo_size(cpu,node,width,height);
    // The scouts backplate retains its preset's actual size after queued SetSize updates the requested size.
    if(fabsf(rdss(node+0x100)-width)>.01f||fabsf(rdss(node+0x104)-height)>.01f){
        float size[4]={width,height,0,0};call(cpu,UINT64_C(0x140F99FB0),node,size,0,0,0);
    }
}

static int mobile_scouts_layout(CPU *cpu,uint64_t root,float width,float height){
    const char *name=law_photo_name(root);
    if(!name||strcmp(name,"EXT_MESSAGE_SCOUTS_PANEL"))return 0;
    uint64_t animation=law_photo_child(root,"ANI");if(!animation)return 0;
    uint64_t background=law_photo_child(animation,"Background"),body=law_photo_child(animation,"content_layout");if(!body)return 0;
    uint64_t content=law_photo_child(body,"CONTENT");
    uint64_t text=law_photo_child(body,"MESSAGE_TEXT"),title=law_photo_child(animation,"TITLE_TEXT");
    uint64_t close=law_photo_child(animation,"CLOSE_BUTTON"),icon=law_photo_child(animation,"info_logo");
    if(!animation||!background||!body||!content||!text||!title||!close||!icon
       ||rd64(text)!=GVA(0x141DED7D8)||rd64(title)!=GVA(0x141DED7D8))return 0;
    uint64_t nodes[]={root,animation,background,body,content,text,title,close,icon};
    uint32_t id=rd32(root+0x1e4);unsigned index=0;
    for(;index<mobile_scouts_count;++index)if(mobile_scouts[index].root==root&&mobile_scouts[index].id==id)break;
    if(index==mobile_scouts_count){
        if(applied_scale<=100||index==16)return 0;
        mobile_scouts_saved *saved=&mobile_scouts[mobile_scouts_count++];saved->root=root;saved->id=id;
        uint64_t labels[]={text,title},flow[]={text,content};
        for(unsigned i=0;i<2;++i){saved->wrap[i][0]=rdss(labels[i]+0x37c);saved->wrap[i][1]=rdss(labels[i]+0x380);
            saved->limit[i]=rd8(labels[i]+0x384);saved->ignore[i]=rd32(flow[i]+0x1e0)&4;}
        for(unsigned i=0;i<9;++i){saved->actual[i][0]=rdss(nodes[i]+0x100);saved->actual[i][1]=rdss(nodes[i]+0x104);}
    }
    mobile_scouts_saved *saved=&mobile_scouts[index];
    if(applied_scale<=100){
        if(saved->active){
            uint64_t labels[]={text,title},flow[]={text,content};
            for(unsigned i=0;i<2;++i){mobile_message_wrap(cpu,labels[i],saved->wrap[i][0],saved->wrap[i][1],saved->limit[i]);
                if((rd32(flow[i]+0x1e0)&4)!=saved->ignore[i])call_value(cpu,saved->ignore[i]?UINT64_C(0x140EC0F60):UINT64_C(0x140EBE470),flow[i],4);}
            for(unsigned i=0;i<9;++i){
                law_photo_restore(cpu,nodes[i]);
                float size[4]={saved->actual[i][0],saved->actual[i][1],0,0};
                call(cpu,UINT64_C(0x140F99FB0),nodes[i],size,0,0,0);
            }
            call_value(cpu,UINT64_C(0x141096AE0),body,0);saved->active=0;
            uint64_t frame=law_photo_child(content,"resources_frame");
            if(frame){uint64_t lines[]={law_photo_child(frame,"bottom_left_line"),law_photo_child(frame,"bottom_right_line")};
                for(unsigned i=0;i<2;++i)if(lines[i])law_photo_restore(cpu,lines[i]);}
        }
        return 1;
    }
    for(unsigned i=0;i<sizeof(nodes)/sizeof(nodes[0]);++i)if(!law_photo_remember(nodes[i]))return 0;
    /* Text keeps clear of the frame's 63-unit corner art on both sides. */
    float card_width=fminf(480,width-32),text_width=card_width-96;
    if(text_width<=0)return 0;
    float title_scale=.65f,title_width=(card_width-124)/title_scale;
    mobile_message_wrap(cpu,text,text_width,0,1);mobile_message_wrap(cpu,title,title_width,0,1);
    float header=fmaxf(80,mobile_message_height(title)*title_scale+40),text_height=mobile_message_height(text);
    float content_scale=fminf(1,text_width/fmaxf(1,rdss(content+0x100)));
    float content_height=ui_visible_chain(content)?rdss(content+0x104)*content_scale:0;
    float body_height=content_height+(content_height?8:0)+text_height;
    float card_height=header+12+body_height+78,factor=fminf(1,(height-32)/card_height);
    float left=(width-card_width*factor)*.5f,top=(height-card_height*factor)*.5f;
    uint64_t flow[]={text,content};
    for(unsigned i=0;i<2;++i)if(!(rd32(flow[i]+0x1e0)&4))call_value(cpu,UINT64_C(0x140EC0F60),flow[i],4);
    mobile_message_size(cpu,root,card_width,card_height);law_photo_place(cpu,root,left,top,factor,factor);
    mobile_message_size(cpu,animation,card_width,card_height);law_photo_place(cpu,animation,left,top,factor,factor);
    mobile_message_size(cpu,background,card_width-21,card_height-66);law_photo_place(cpu,background,left,top+69*factor,factor,factor);
    mobile_message_size(cpu,body,text_width,body_height);law_photo_place(cpu,body,left+48*factor,top+(header+12)*factor,factor,factor);
    law_photo_place(cpu,content,left+48*factor,top+(header+12)*factor,content_scale*factor,content_scale*factor);
    mobile_message_size(cpu,text,text_width,text_height);law_photo_place(cpu,text,left+48*factor,top+(header+12+content_height+(content_height?8:0))*factor,factor,factor);
    /* The cargo frame's top border breaks for its centred caption; open the gap to the caption's width. */
    uint64_t frame=law_photo_child(content,"resources_frame");
    uint64_t caption=frame?law_photo_child(frame,"what_journey_has_geven_txt"):0;
    uint64_t line_left=frame?law_photo_child(frame,"bottom_left_line"):0,line_right=frame?law_photo_child(frame,"bottom_right_line"):0;
    if(caption&&line_left&&line_right&&law_photo_remember(line_left)&&law_photo_remember(line_right)){
        float ink=mobile_text_ink(caption),box=rdss(caption+0x210),frame_width=rdss(frame+0x100);
        float centre=rdss(caption+0xc0)+box*.5f,gap_left=centre-ink*.5f-12,gap_right=centre+ink*.5f+12;
        float left_x=rdss(line_left+0xc0),right_end=rdss(line_right+0xc0)+rdss(line_right+0x100);
        if(ink>0&&frame_width>0&&isfinite(gap_left)&&isfinite(gap_right)){
            law_photo_size(cpu,line_left,fmaxf(0,gap_left-left_x),rdss(line_left+0x104));
            law_photo_size(cpu,line_right,fmaxf(0,right_end-gap_right),rdss(line_right+0x104));
            layout_position(cpu,line_right,gap_right,rdss(line_right+0xc4));
        }
    }
    mobile_message_size(cpu,title,title_width,fmaxf(1,mobile_message_height(title)));law_photo_place(cpu,title,left+100*factor,top+20*factor,title_scale*factor,title_scale*factor);
    law_photo_place(cpu,icon,left+42*factor,top+39*factor,.3f*factor,.3f*factor);
    law_photo_place(cpu,close,left+(card_width-195)*factor*.5f,top+(card_height-58)*factor,factor,factor);
    saved->active=1;return 1;
}

static void viewport_decoration(CPU *cpu,uint64_t node,float width,float height) {
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);if(!name)return;
    unsigned logo=!strcmp(name,"LOGO")&&rd64(node)==GVA(0x141DA2AA8);
    unsigned header=!strcmp(name,"HEADER_LAYOUT")&&rd64(node)==GVA(0x141DE0A48);
    unsigned background=0;
    unsigned photo_fade=(!strcmp(name,"darken_lefttop")||!strcmp(name,"darken_rightbottom"));
    if((strstr(name,"BACKGROUND")||strstr(name,"DARKEN")||strstr(name,"darken")||strstr(name,"BLACK"))&&rd64(node)!=GVA(0x141D796F8)){
        uint32_t background_id=rd32(node+0x1e4);int found=ui_index_find(background_index,1023u,node,background_id);
        unsigned i=found>=0?(unsigned)found:background_count;
        if(i==background_count&&i<512&&(rdss(node+0x210)>=1200||photo_fade)){
            backgrounds[i].node=node;backgrounds[i].id=background_id;backgrounds[i].axes=rdss(node+0x214)>=700?3:1;
            ui_index_add(background_index,1023u,node,background_id,(int)i);++background_count;
        }
        if(i<background_count)background=backgrounds[i].axes;
    }
    unsigned sequel=strstr(name,"Sequel_splash_illustration")!=NULL;
    if(!logo&&!header&&!background&&!sequel)return;
    float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
    if(sx<=0||sy<=0)return;
    float transform[4];memcpy(transform,(void*)(uintptr_t)(node+0xf0),16);
    float position[4];memcpy(position,(void*)(uintptr_t)(node+0xc0),16);
    if(background){
        if(photo_fade&&rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
        if(photo_fade&&rd32(node+0x13c))call_value(cpu,UINT64_C(0x140EC30A0),node,0);
        float size[4]={width/sx/transform[0],rdss(node+0x214),0,0};
        position[0]=-x/sx-rdss(node+0xd0)*transform[0];
        if(background&2){size[1]=height/sy/transform[1];position[1]=-y/sy-rdss(node+0xd4)*transform[1];}
        if(fabsf(rdss(node+0x210)-size[0])>.01f||fabsf(rdss(node+0x214)-size[1])>.01f
           ||(photo_fade&&(fabsf(rdss(node+0x100)-size[0])>.01f||fabsf(rdss(node+0x104)-size[1])>.01f)))
            call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
    } else {
        float factor=100.f/applied_scale;
        if(logo)factor*=.95f;
        if(sequel)factor=fminf(factor,fminf((width-32)/rdss(node+0x100),(height-80)/rdss(node+0x104)));
        transform[0]=factor/sx;transform[1]=factor/sy;
        position[0]=(width*.5f-x)/sx-(rdss(node+0xd0)+rdss(node+0x100)*.5f)*transform[0];
        float top=header?24*100.f/applied_scale:(height-rdss(node+0x104)*factor)*.5f;
        position[1]=(top-y)/sy-rdss(node+0xd4)*transform[1];
        if(fabsf(rdss(node+0xf0)-transform[0])>.0001f||fabsf(rdss(node+0xf4)-transform[1])>.0001f)
            call(cpu,UINT64_C(0x140EC46E0),node,transform,0,0,0);
    }
    placing_decoration=1;
    if(fabsf(rdss(node+0xc0)-position[0])>.01f||fabsf(rdss(node+0xc4)-position[1])>.01f)
        call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
    placing_decoration=0;
}

static void mobile_input(CPU *cpu,uint64_t node,unsigned enabled){
    mobile_layout *base=remember_layout(node);
    if(base){unsigned input=(applied_scale<=100||enabled)?base->input:0;
        if((rd32(node+0x1e0)&0x10)!=input)call_value(cpu,input?UINT64_C(0x140EC0F60):UINT64_C(0x140EBE470),node,0x10);
    }
}
static void population_inputs(CPU *cpu,uint64_t node,unsigned open,unsigned depth,unsigned *budget){
    if(!node||depth>32||!*budget)return;
    --*budget;
    if(rd64(node)==GVA(0x141D79F78))mobile_input(cpu,node,open);
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))population_inputs(cpu,child,open,depth+1,budget);
}

static float issue_ordinate(uint64_t node,uint64_t ancestor,float y){
    for(;node&&node!=ancestor;node=rd64(node+0x230))
        y=rdss(node+0xc4)+(rdss(node+0xd4)+y)*rdss(node+0xf4);
    return y;
}

static float issue_circle_center(uint64_t button,uint64_t group){
    uint64_t icon=law_photo_child(button,"Ikona");
    uint64_t graphic=icon?law_photo_child(icon,"graphic"):0;
    uint64_t circle=graphic?law_photo_child(graphic,"circle_circle"):0;
    if(!circle)return NAN;
    return issue_ordinate(circle,rd64(group+0x230),rdss(circle+0x104)*.5f)-rdss(group+0xc4);
}

static void mobile_issue_row(CPU *cpu,uint64_t node){
    uint64_t header=law_photo_child(node,"HEADER");if(!header)return;
    uint64_t groups[]={law_photo_child(header,"IMPORTANT_DANGER_BUTTONS"),law_photo_child(header,"DANGER_BUTTONS")};
    mobile_layout *base[2]={0};float left[2]={INFINITY,INFINITY},right[2]={-INFINITY,-INFINITY},span[2]={0},center[2]={NAN,NAN};
    for(unsigned i=0;i<2;++i){
        if(!groups[i])continue;base[i]=remember_layout(groups[i]);
        unsigned count=0;
        for(uint64_t child=rd64(groups[i]+0x220);child&&count++<128;child=rd64(child+0x240)){
            if(rd64(child)!=GVA(0x141D79F78)||!ui_visible_chain(child))continue;
            if(applied_scale>100&&!isfinite(center[i]))center[i]=issue_circle_center(child,groups[i]);
            float x=rdss(child+0xc0)+rdss(child+0xd0)*rdss(child+0xf0),r=x+rdss(child+0x100)*rdss(child+0xf0);
            if(isfinite(x)&&isfinite(r)&&r>x){left[i]=fminf(left[i],x);right[i]=fmaxf(right[i],r);}
        }
        if(right[i]>left[i])span[i]=right[i]-left[i];
    }
    float cursor=200-(span[0]+span[1]+(span[0]&&span[1]?8:0))*.5f;
    // The two authored button templates place their circles at different offsets.
    for(unsigned i=0;i<2;++i)if(base[i]){
        if(applied_scale<=100||!span[i])layout_position(cpu,groups[i],base[i]->x,base[i]->y);
        else{
            float y=base[0]?base[0]->y:1;
            if(i&&isfinite(center[0])&&isfinite(center[1]))y+=center[0]-center[1];
            layout_position(cpu,groups[i],cursor-left[i],y);cursor+=span[i]+8;
        }
    }
}

static void mobile_issue_count(CPU *cpu,uint64_t label){
    mobile_layout *base=remember_layout(label);if(!base)return;
    float y=5.5f-rdss(label+0xd4)*rdss(label+0xf4);
    if(applied_scale>100){
        uint64_t count=law_photo_child(label,"COUNT"),icon=rd64(label+0x230);
        uint64_t button=icon?rd64(icon+0x230):0,group=button?rd64(button+0x230):0;
        uint64_t header=group?rd64(group+0x230):0;
        uint64_t major=header?law_photo_child(header,"IMPORTANT_DANGER_BUTTONS"):0;
        unsigned visited=0;
        for(uint64_t other=major?rd64(major+0x220):0;count&&other&&visited++<128;other=rd64(other+0x240)){
            if(rd64(other)!=GVA(0x141D79F78)||!ui_visible_chain(other))continue;
            uint64_t art=law_photo_child(other,"Ikona"),texts=art?law_photo_child(art,"TEXTS"):0;
            uint64_t reference=texts?law_photo_child(texts,"COUNT"):0;
            if(!reference||!ui_visible_chain(reference))continue;
            float sy=rdss(icon+0xf4)*rdss(button+0xf4)*rdss(group+0xf4);
            float target=issue_ordinate(reference,header,rdss(reference+0x104));
            float current=issue_ordinate(count,header,rdss(count+0x104));
            // The counters share a font size, with different text-box heights.
            if(sy>0&&isfinite(target)&&isfinite(current))y=rdss(label+0xc4)+(target-current)/sy;
            break;
        }
    }
    layout_position(cpu,label,base->x,y);
}

static void dialog_contents(CPU *cpu,uint64_t node,unsigned depth,unsigned *budget,float width,float height){
    if(!node||depth>32||!*budget)return;
    --*budget;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    uint64_t parent=rd64(node+0x230);
    // This transparent resource-bar blocker covers the expedition buttons after fitting the bar.
    if(name&&!strcmp(name,"lock_click_btn")&&law_photo_ancestor(node,"EXT_RESOURCES_PANEL"))mobile_input(cpu,node,0);
    const char *parent_name=parent?(const char*)(uintptr_t)rd64(parent+0x1d8):NULL;
    if(name&&law_photo_ancestor(node,"EXT_ECONOMY_PANEL")){
        if(applied_scale<=100)law_photo_restore(cpu,node);
        else if(!strcmp(name,"CLOSE_BUTTON"))law_photo_place(cpu,node,width-144,8,.8f,.8f);
        else if(!switch_mode&&!strcmp(name,"tabs_clip")){
            if(law_photo_remember(node)){law_photo_size(cpu,node,110,(height-64)/.65f);law_photo_place(cpu,node,8,56,.65f,.65f);}
        } else if(!switch_mode&&!strcmp(name,"SWITCH_SUBPANEL_BUTTONS_LAYOUT"))law_photo_place(cpu,node,20,65,.65f,.65f);
    }
    if(name&&!strcmp(name,"NEW_DLC_SHARP_GLOW")&&rd64(node)==GVA(0x141D78A60)
       &&parent_name&&!strcmp(parent_name,"MENU_BUTTON_LAYOUT")&&law_photo_ancestor(node,"EXT_MAIN_MENU_PANEL")){
        // The rotating white sweep escapes its desktop clip when the menu grows.
        // Keep the mobile menu's gold DLC label; restore the authored decoration at 100%.
        mobile_layout *base=remember_layout(node);
        if(base){unsigned mobile=fp_port_setting_get(FP_SETTING_UI_SCALE)>100;layout_scale(cpu,node,mobile?0:base->sx,mobile?0:base->sy);}
    }
    if(rd32(node+0x1e0)&2)return;
    // Screen docking uses the HUD canvas even inside an independently fitted PC dialog.
    if(rd64(node)==GVA(0x141D78A60)&&fabsf(rdss(node+0x210)-1280)<.01f&&fabsf(rdss(node+0x214)-720)<.01f){
        if(rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
        if(fabsf(rdss(node+0x100)-1280)>.01f||fabsf(rdss(node+0x104)-720)>.01f){
            float size[4]={1280,720,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
        }
    }
    if(name&&!strcmp(name,"INK_PANEL")&&parent_name
       &&(!strcmp(parent_name,"EXT_MAIN_MENU_PANEL")||!strcmp(parent_name,"EXT_INGAME_MENU_MAIN_PANEL")||!strcmp(parent_name,"EXT_SETTINGS_PANEL")
          ||!strcmp(parent_name,"EXT_INGAME_MENU_SETTINGS_PANEL")||!strcmp(parent_name,"EXT_LOADGAME_PANEL")
          ||!strcmp(parent_name,"EXT_INGAME_MENU_LOADGAME_PANEL")||!strcmp(parent_name,"EXT_INGAME_MENU_SAVEGAME_PANEL")
          ||!strcmp(parent_name,"EXT_SAVE_GAME_PANEL")||!strcmp(parent_name,"EXT_CONFIRMATION_PANEL")
          ||!strcmp(parent_name,"EXT_SCENARIOS_PANEL")||!strcmp(parent_name,"EXT_ENDLESS_CONFIGURATION_PANEL")
          ||!strcmp(parent_name,"EXT_CUSTOM_SCENARIO_PANEL"))){
        mobile_layout *base=remember_layout(node);
        if(base){
            unsigned requested=fp_port_setting_get(FP_SETTING_UI_SCALE);
            if(requested==100){
                if(rdss(node+0x210)!=base->w||rdss(node+0x214)!=base->h){float size[4]={base->w,base->h,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);}
                layout_scale(cpu,node,base->sx,base->sy);layout_position(cpu,node,base->x,dialog_ime_y(node,base->y,height));
                if(rd32(node+0x138)!=base->dock)call_value(cpu,UINT64_C(0x140EC39C0),node,base->dock);
            }
            else{
                unsigned main=!strcmp(parent_name,"EXT_MAIN_MENU_PANEL");
                unsigned pause=!strcmp(parent_name,"EXT_INGAME_MENU_MAIN_PANEL");
                unsigned compact_popup=!strcmp(parent_name,"EXT_SAVE_GAME_PANEL")||!strcmp(parent_name,"EXT_CONFIRMATION_PANEL");
                /* Scenario list and its customisation sliders, Endless configuration: one column right
                 * of the picture (header 710..1222, rows down to the footer); it grows in place.
                 * Custom scenario: two 420-unit columns around the centre. */
                unsigned scenarios=!strcmp(parent_name,"EXT_SCENARIOS_PANEL"),endless=!strcmp(parent_name,"EXT_ENDLESS_CONFIGURATION_PANEL");
                unsigned custom=!strcmp(parent_name,"EXT_CUSTOM_SCENARIO_PANEL");
                float aspect=frostpunk_display_aspect?frostpunk_display_aspect():16.f/9.f,visible=fmaxf(1280.f,720.f*aspect);
                float target_x=640,center_x=310.05f,center_y=336.5f;
                if(main){center_x=638;center_y=369;}
                else if(pause){center_x=639;center_y=362;}
                else if(!strcmp(parent_name,"EXT_INGAME_MENU_SETTINGS_PANEL")){center_x=637.55f;center_y=326.5f;}
                else if(!strcmp(parent_name,"EXT_LOADGAME_PANEL")){center_x=304.55f;center_y=327;}
                else if(!strcmp(parent_name,"EXT_INGAME_MENU_LOADGAME_PANEL")){center_x=637.55f;center_y=327;}
                else if(!strcmp(parent_name,"EXT_INGAME_MENU_SAVEGAME_PANEL")){center_x=643.55f;center_y=332;}
                else if(compact_popup){center_x=204;center_y=178;}
                else if(scenarios||endless){center_x=966;center_y=scenarios?354:360;}
                else if(custom){center_x=646;center_y=346;}
                float factor=fminf(requested/100.f,main?656.f/(370*.95f):pause?656.f/350:compact_popup?1.5f
                    :scenarios?700.f/500:endless?700.f/561:custom?fminf(700.f/539,(visible-24)/854):672.f/449);
                if(scenarios||endless){
                    /* The column's widest rows are the customisation sliders: labels from x 773 to the
                     * slider ends at 1205.  Keep the labels right of the scenario picture (its frost
                     * reaches 745, the Endless map's 765) and the sliders inside the screen. */
                    float left=775,right=640+visible*.5f-12;
                    factor=fminf(factor,(right-left)/(1205.f-773.f));
                    target_x=left+(center_x-773)*factor;
                }
                // Scale the complete ink composition so its text, alpha masks and camera keep one transform.
                if(rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
                if(!compact_popup&&(rdss(node+0x210)!=1280||rdss(node+0x214)!=720)){float size[4]={1280,720,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);}
                layout_scale(cpu,node,factor,factor);
                float y=360-(center_y+rdss(node+0xd4))*factor;
                layout_position(cpu,node,target_x-(center_x+rdss(node+0xd0))*factor,dialog_ime_y(node,y,height));
            }
        }
    }
    if(name&&parent_name&&((!strcmp(name,"TEXTSEQUENCE")&&!strcmp(parent_name,"CONTENT"))
        ||(!strcmp(name,"TextLayout")&&!strcmp(parent_name,"SLIDE_DEFAULT"))
        ||(!strcmp(name,"TextBoxLayout")&&!strcmp(parent_name,"SLIDE_TEXTBOX")))
       &&law_photo_ancestor(node,"EXT_CINEMATICS_PANEL")){
        /* Cinematic captions: TEXTSEQUENCE (centred lines up to 1000 wide), the default slide's
         * TextLayout (900-wide text) and the text-box slide's TextBoxLayout grow with the interface
         * scale around their centres, inside the screen's width. */
        mobile_layout *base=remember_layout(node);
        if(base){
            unsigned requested=fp_port_setting_get(FP_SETTING_UI_SCALE);
            float aspect=frostpunk_display_aspect?frostpunk_display_aspect():16.f/9.f,visible=fmaxf(1280.f,720.f*aspect);
            float content=name[0]=='T'&&name[1]=='E'?1000.f:name[4]=='B'?440.f:900.f;
            float s=requested>100?fminf(fminf(requested/100.f,1.5f),(visible-24)/content):1;
            if(!(s>1))s=1;
            float sx=base->sx*s,sy=base->sy*s;
            /* The text-box slide sits at the left edge: it grows to the right from there. */
            float anchor=name[4]=='B'?0.f:.5f;
            float x=base->x+(base->px+base->w*anchor)*(base->sx-sx),y=base->y+(base->py+base->h*.5f)*(base->sy-sy);
            layout_scale(cpu,node,sx,sy);layout_position(cpu,node,x,y);
        }
    }
    if(name&&parent_name&&!strcmp(parent_name,"MODES_VIEW")&&(!strcmp(name,"BACK_BUTTON")||!strcmp(name,"DOTS_UNDERLINE"))
       &&law_photo_ancestor(node,"EXT_ENDLESS_MODE_SELECTION_PANEL")){
        /* Under the enlarged cards: Back and its divider move down by 30 (Back ends at 713 of 720). */
        mobile_layout *base=remember_layout(node);
        if(base)layout_position(cpu,node,base->x,base->y+(fp_port_setting_get(FP_SETTING_UI_SCALE)>100?30.f:0.f));
    }
    if(name&&!strcmp(name,"MODES_LAYOUT")&&law_photo_ancestor(node,"EXT_ENDLESS_MODE_SELECTION_PANEL")){
        /* The three Endless cards keep their titles and descriptions in a 1047x130 strip (canvas
         * 118..1165, 495..625) under the city art: scale the cards around it, the art stays. */
        mobile_layout *base=remember_layout(node);
        if(base){
            unsigned requested=fp_port_setting_get(FP_SETTING_UI_SCALE);
            float aspect=frostpunk_display_aspect?frostpunk_display_aspect():16.f/9.f,visible=fmaxf(1280.f,720.f*aspect);
            float factor=requested>100?fminf(fminf(requested/100.f,1.35f),(visible-24)/1047):1;
            if(factor<1)factor=1;
            /* Local point of the strip's centre: canvas (642,560) minus the layout's own origin. */
            float lx=642-(rdss(rd64(node+0x230)+0xc0)+base->x),ly=560-base->y;
            layout_scale(cpu,node,base->sx*factor,base->sy*factor);
            layout_position(cpu,node,base->x+lx*(1-factor),base->y+ly*(1-factor));
        }
    }
    if(name&&!strcmp(name,"TOOLTIP_DETAILS")&&law_photo_ancestor(node,"EXT_POPULATION_PANEL")){
        uint64_t population=law_photo_ancestor(node,"EXT_POPULATION_PANEL"),animation=law_photo_child(population,"ANIM");
        uint64_t button=animation?law_photo_child(animation,"SHOW_TOOLTIP_DETAILS_BUTTON"):0;
        uint64_t controller=button?mobile_button_controller(button,UINT64_C(0x141A4D700)):0;
        uint64_t reference=controller?rd64(controller+0x100):0;
        // The folded native panel leaves invisible buttons over the top-right HUD at large scales.
        if(reference&&rd64(reference)==node){unsigned remaining=512;population_inputs(cpu,node,rd8(controller+0x108),0,&remaining);}
        uint64_t content=rd64(node+0x220);mobile_layout *base=remember_layout(node);
        if(base&&content){
            float top=applied_scale>100?474-55-rdss(content+0x104)-rdss(content+0xc4):base->y;
            layout_position(cpu,node,base->x,top);
        }
    }
    if(name&&(!strcmp(name,"darken_lefttop")||!strcmp(name,"darken_rightbottom")||strstr(name,"Sequel_splash_illustration")))
        viewport_decoration(cpu,node,width,height);
    /* The Beacon's page (scout deploy slot and the scrolling list of scout units, 391 units) sits
     * directly in its subpanel, not under EXT_TAB_: its scout list ran below the frame. */
    if(name&&name[0]=='E'&&(!strncmp(name,"EXT_TAB_",8)||!strcmp(name,"EXT_EXPEDITION_BUILDING_PANEL")))selection_tab_fit(cpu,node);
    if(rd64(node)==GVA(0x141DED7D8)){
        // Tutorial pages stay a fitted PC dialog (scale 0.5); their paragraphs live in a scroll pane, so a
        // larger font only adds lines.  Section tabs and headers keep their authored size.
        // Only the paragraph: the clock label belongs to the illustration and the footer has a one-line frame.
        if(name&&!strcmp(name,"DESCRIPTION_TEXT")&&law_photo_ancestor(node,"EXT_TUTORIAL_SUMMARY_PANEL")){
            phone_text_fit(cpu,node,23);
            // The paragraph's box keeps its authored 60-unit height; give the flow the measured height so
            // the pictures below move down instead of being overwritten.
            law_photo_saved *saved=!strcmp(name,"DESCRIPTION_TEXT")?law_photo_remember(node):NULL;
            if(saved){
                float measured=mobile_message_height(node);
                if(applied_scale<=100)law_photo_restore(cpu,node);
                else if(measured>0&&fabsf(rdss(node+0x214)-measured)>.5f){
                    law_photo_size(cpu,node,rdss(node+0x210),measured);
                    if(parent&&rd64(parent)==GVA(0x141DE0A48))call_value(cpu,UINT64_C(0x141096AE0),parent,0);
                }
            }
        }
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))
        dialog_contents(cpu,child,depth+1,budget,width,height);
}

void fp_mobile_ui_position(CPU *cpu,void (*original)(CPU*)) {
    uint64_t node=cpu->rcx;unsigned adapted=0;
    const char *node_name=(const char*)(uintptr_t)rd64(node+0x1d8);
    if(law_photo_position(cpu,original))return;
    if(placing_decoration){unsigned dock=rd32(node+0x138);wr32(node+0x138,0);original(cpu);wr32(node+0x138,dock);return;}
    if(node_name&&(!strcmp(node_name,"EXT_SOCIETY_ISSUES_PANEL")||!strcmp(node_name,"SEARCH_CITIZENS_BUTTON"))){
        float position[4];uint64_t input=cpu->rdx;memcpy(position,(void*)(uintptr_t)input,16);
        if(!strcmp(node_name,"EXT_SOCIETY_ISSUES_PANEL"))position[0]=439.5f+(rdss(GVA(0x142980E60))-1280)*.5f;
        else if(applied_scale>100){position[0]=-139.5f;position[1]=-40;}  /* keep in step with prepare_canvas */
        unsigned dock=rd32(node+0x138);wr32(node+0x138,0);cpu->rdx=(uintptr_t)position;
        original(cpu);wr32(node+0x138,dock);if(cpu->rdx==(uintptr_t)position)cpu->rdx=input;return;
    }
    if(newsflash_panel(node)){
        remember_layout(node);
        float position[4];uint64_t input=cpu->rdx;memcpy(position,(void*)(uintptr_t)input,16);
        // Both original presets author X=622; native callbacks can feed back an already adapted position.
        position[0]=newsflash_x();
        cpu->rdx=(uintptr_t)position;original(cpu);
        if(cpu->rdx==(uintptr_t)position)cpu->rdx=input;
        return;
    }
    if(applied_scale&&node_name&&!strcmp(node_name,"EXT_SELECTION_PANEL")){
        float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
        float a=rdss(node+0xf0),b=rdss(node+0xf4);
        if(sx>0&&sy>0&&a>0&&b>0){
            // The original world-anchor animation runs after layout; keep every selected-object panel on screen.
            float bottom=applied_scale>100&&rdss(node+0x210)>342?76:68;
            float position[4]={(rdss(GVA(0x142980E60))-12-x)/sx-(rdss(node+0x100)+rdss(node+0xd0))*a,
                (rdss(GVA(0x142980E64))-bottom-y)/sy-(rdss(node+0x104)+rdss(node+0xd4))*b,0,1};
            if(bottom==76){
                float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
                position[0]=((width-rdss(node+0x100)*a*sx)*.5f-x)/sx-rdss(node+0xd0)*a;
                position[1]=(selection_frame_top(width,height,rdss(node+0x104)*b*sy)-y)/sy-rdss(node+0xd4)*b;
            }
            uint64_t saved=cpu->rdx;unsigned dock=rd32(node+0x138);wr32(node+0x138,0);cpu->rdx=(uintptr_t)position;
            original(cpu);wr32(node+0x138,dock);if(cpu->rdx==(uintptr_t)position)cpu->rdx=saved;return;
        }
    }
    if(applied_scale>100&&hud_bottom_panel(node)){
        float position[4];uint64_t input=cpu->rdx;memcpy(position,(void*)(uintptr_t)input,16);
        if(fabsf(position[1]+208)<.01f)position[1]+=14;
        unsigned dock=rd32(node+0x138);wr32(node+0x138,0);
        cpu->rdx=(uintptr_t)position;original(cpu);wr32(node+0x138,dock);
        if(cpu->rdx==(uintptr_t)position)cpu->rdx=input;return;
    }
    if(inside_original_dialog(node)){original(cpu);return;}
    pthread_mutex_lock(&input_mutex);
    {
        int i=ui_index_find(docked_index,8191u,node,rd32(node+0x1e4));
        if(i>=0){adapted=1;mobile_docked[i].y=rdss(cpu->rdx+4);}
    }
    pthread_mutex_unlock(&input_mutex);
    unsigned dock=rd32(node+0x138),scale=applied_scale;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    unsigned main_buttons=name&&!strcmp(name,"MAIN_BUTTONS");
    unsigned bottom_origin=name&&(!strcmp(name,"BOTTOM")||!strcmp(name,"BOTTOM_static"))&&fabsf(rdss(node+0x214))<.01f;
    unsigned widget=construction_widget(node);
    unsigned event_mask=small_event_mask(node);
    if((!adapted||dock!=5||!scale)&&!main_buttons&&!widget&&!event_mask&&!bottom_origin){original(cpu);return;}
    _Alignas(16) float position[4];uint64_t input=cpu->rdx;
    memcpy(position,(void*)(uintptr_t)input,16);
    if(bottom_origin){position[0]=0;position[1]=rdss(GVA(0x142980E64));}
    else if(event_mask){
        position[0]+=(rdss(GVA(0x142980E60))-1280)*.5f;
        position[1]+=(rdss(GVA(0x142980E64))-720)*.5f;
    } else if(widget){
        float base=widget==1?84:widget==2?11:1214;
        if(fabsf(position[0]-base)<.01f)position[0]+=(1280-rdss(GVA(0x142980E60)))*.5f*(widget==3?-1:1);
    } else if(main_buttons&&fabsf(position[0]-18)<.01f){
        uint64_t parent=rd64(node+0x168);if(!parent)parent=rd64(node+0x230);
        if(parent&&rdss(parent+0x100)>0)position[0]+=(rdss(parent+0x100)-1280)*.5f;
    } else if(adapted&&dock==5&&scale){position[0]*=100.f/scale;position[1]*=100.f/scale;}
    // Preserve the original slide animation while filling the mobile viewport.
    wr32(node+0x138,0);cpu->rdx=(uintptr_t)position;original(cpu);wr32(node+0x138,dock);
    if(cpu->rdx==(uintptr_t)position)cpu->rdx=input;
}

#include "mobile_dialog_mask.inc"
#include "switch_ui.inc"

/* Diagnostics for the picture templates (debug.fp.header_sfx, re-read every two seconds): bits 1, 2, 4
 * omit SFX_MASK_BLUR, SFX_MASK_BLUR_CORNERS, SFX_INTERIOR; bit 8 keeps the desktop viewport. */
static float header_sfx_override;   /* debug.fp.sfx_factor: viewport multiplier instead of the interface scale */
static unsigned header_sfx_skipped(void){
    static unsigned mask;static uint64_t checked;
#ifdef __ANDROID__
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    uint64_t ms=(uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
    if(ms-checked>2000){checked=ms;char value[PROP_VALUE_MAX]={0};
        mask=__system_property_get("debug.fp.header_sfx",value)>0&&value[0]?(unsigned)strtoul(value,NULL,0):0u;
        char factor[PROP_VALUE_MAX]={0};
        header_sfx_override=__system_property_get("debug.fp.sfx_factor",factor)>0&&factor[0]?strtof(factor,NULL):0;}
#endif
    return mask;
}

void fp_mobile_ui_template(CPU *cpu,void (*original)(CPU*)) {
    if(dialog_mask_render(cpu,original))return;
    if(law_photo_template(cpu,original))return;
    uint64_t node=cpu->rcx;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    /* Building and world-map site pictures: SFX_INTERIOR is a perspective 3D template (layered art and
     * snow) with its blur/corner masks, authored as a desktop-canvas viewport whose top-left corner is the
     * picture.  140F9D240 sizes that viewport from the unscaled +100/+104, so inside a scaled panel on
     * the halved phone canvas it showed a magnified, blurred crop.  Give the draw the viewport the
     * picture actually covers: the node's accumulated scale. */
    /* The same desktop-viewport pictures appear across the HUD panels (world-map site SFX_HEADER, the
     * citizen portrait, the artifact depot); full-screen dialogs fitted as a whole have factor 1. */
    /* Menus keep the 100% HUD canvas but their ink panels grow with the requested scale (the restart
     * notice by 1.5): its 2560x720 ink mask then covered only the right half of the message. */
    unsigned confirmation=name&&fp_port_setting_get(FP_SETTING_UI_SCALE)>100&&law_photo_ancestor(node,"EXT_CONFIRMATION_PANEL");
    if((applied_scale>100||confirmation)&&name&&rd64(node)==GVA(0x141D796F8)&&(!strncmp(name,"SFX_",4)||strstr(name,"_SFX"))
       &&(confirmation||law_photo_ancestor(node,"EXT_BUILDING_HEADER")||law_photo_ancestor(node,"EXT_WORLD_MAP_SELECTION_PANEL")
          ||law_photo_ancestor(node,"EXT_ARTIFACT_DEPOT_PANEL"))){
        unsigned bit=!strcmp(name,"SFX_MASK_BLUR")?1u:!strcmp(name,"SFX_MASK_BLUR_CORNERS")?2u:!strcmp(name,"SFX_INTERIOR")?4u:0u,mode=header_sfx_skipped();
        if(bit&mode){cpu->rsp+=8;return;} // Complete the guest void call even when its decorative draw is omitted.
        if(!(mode&8)&&!rd8(node+0x380)&&!rd8(node+0x383)){
            float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
            /* 140F9D240 converts size and origin with the same canvas factor and centres an off-axis
             * projection on the viewport, so the viewport must be the node's on-screen box: accumulated
             * scale only.  The interface scale on top doubled the scene (only the generator's spire
             * and a crop of Teslagrad showed); 1 was checked against the full PC composition. */
            float multiplier=header_sfx_override>0?header_sfx_override:1;
            float factor=sx*rdss(node+0xf0)*multiplier,factor_y=sy*rdss(node+0xf4)*multiplier;
            float width=rdss(node+0x100),height=rdss(node+0x104);
            if(isfinite(factor)&&factor>0&&fabsf(factor-factor_y)<.001f&&width>0&&height>0){
                unsigned perspective=rd8(node+0x385);float value=rdss(node+0x37c);
                wrss(node+0x100,width*factor);wrss(node+0x104,height*factor);
                if(!perspective)wrss(node+0x37c,value*factor);
                original(cpu);
                if(!perspective)wrss(node+0x37c,value);
                wrss(node+0x100,width);wrss(node+0x104,height);
                return;
            }
        }
    }
    if(!name||!strstr(name,"MASK")){original(cpu);return;}
    float factor=dialog_mask_projection_factor(node);
    if(fabsf(factor-1)<.0001f){original(cpu);return;}
    // Paint masks have their own 3D camera; its projection must match the enlarged text.
    unsigned offset=rd8(node+0x385)?0x378:0x37c;
    float value=rdss(node+offset);
    if(!isfinite(value)||value<=0||(offset==0x378&&value>=179)){original(cpu);return;}
    float adjusted=offset==0x378?atanf(tanf(value*(3.141592654f/360))/factor)*(360/3.141592654f):value*factor;
    wrss(node+offset,adjusted);original(cpu);wrss(node+offset,value);
}

/* UICanvas::Render (1410CCA60, the line charts of the Economy panel): with a virtual size (+0x2A0/
 * +0x2A4) the canvas scale is (transform scale x size) / (virtual size x own scale) and the transform
 * is applied once more on top, so a scaled ancestor shrinks the plot by its scale a second time (the
 * fitted Economy panel at 0.52 drew 11 days into half of the axis).  The PC never scales an ancestor
 * of a chart; divide that factor out by shrinking the virtual size for the draw. */
void fp_mobile_canvas_render(CPU *cpu,void (*original)(CPU*)){
    uint64_t node=cpu->rcx;
    float w=rdss(node+0x2A0),h=rdss(node+0x2A4),x,y,sx,sy;
    if(!(w>1.2e-7f)&&!(h>1.2e-7f)){original(cpu);return;}
    parent_transform(node,&x,&y,&sx,&sy);
    {static unsigned logged;if(!logged++)fprintf(stderr,"FP_CANVAS virtual=%.2fx%.2f ancestors=%.3fx%.3f\n",w,h,sx,sy);}
    if(!(sx>0)||!(sy>0)||(fabsf(sx-1)<.001f&&fabsf(sy-1)<.001f)){original(cpu);return;}
    if(w>1.2e-7f)wrss(node+0x2A0,w*sx);
    if(h>1.2e-7f)wrss(node+0x2A4,h*sy);
    original(cpu);
    wrss(node+0x2A0,w);wrss(node+0x2A4,h);
}

/* The pause-menu button in the top-left corner is twice its desktop size on a phone.  The HUD
 * animates its position, so the scale keeps the box's top-left corner: the pivot offset is added
 * once to every position the engine writes (tracked by the last value written here). */
static uint64_t menu_node;static uint32_t menu_id;static float menu_written[2],menu_offset[2];
static void menu_button_scale(CPU *cpu,uint64_t node){
    mobile_layout *base=remember_layout(node);if(!base||base->sx<=0||base->sy<=0)return;
    unsigned mobile=applied_scale>100;float factor=mobile?2:1;
    float sx=base->sx*factor,sy=base->sy*factor;
    float offset[2]={rdss(node+0xd0)*(base->sx-sx),rdss(node+0xd4)*(base->sy-sy)};
    float x=rdss(node+0xc0),y=rdss(node+0xc4);
    unsigned ours=menu_node==node&&menu_id==rd32(node+0x1e4)&&fabsf(x-menu_written[0])<.01f&&fabsf(y-menu_written[1])<.01f;
    if(ours){x-=menu_offset[0];y-=menu_offset[1];}   /* back to the engine's own position */
    layout_scale(cpu,node,sx,sy);
    if(!mobile){if(ours)layout_position(cpu,node,x,y);menu_node=0;return;}
    if(ours&&fabsf(offset[0]-menu_offset[0])<.01f&&fabsf(offset[1]-menu_offset[1])<.01f)return;
    layout_position(cpu,node,x+offset[0],y+offset[1]);
    menu_node=node;menu_id=rd32(node+0x1e4);menu_written[0]=x+offset[0];menu_written[1]=y+offset[1];
    memcpy(menu_offset,offset,sizeof(offset));
}

/* Vertical extent of the visible texts and buttons of a panel (its art and input blockers reach far
 * beyond what the player needs to see). */
static void content_extent(uint64_t node,unsigned depth,unsigned *budget,float x,float y,float sx,float sy,float extent[2]){
    if(!node||depth>14||!*budget||(rd32(node+0x1e0)&2))return;
    --*budget;
    float a=rdss(node+0xf0),b=rdss(node+0xf4);
    if(!isfinite(a)||!isfinite(b)||a<=0||b<=0)return;
    x+=sx*(rdss(node+0xc0)+rdss(node+0xd0)*a);y+=sy*(rdss(node+0xc4)+rdss(node+0xd4)*b);sx*=a;sy*=b;
    uint64_t type=rd64(node);
    if(type==GVA(0x141DED7D8)||type==GVA(0x141D79F78)){
        float h=rdss(node+0x104)*sy;
        if(isfinite(h)&&h>0&&h<400){extent[0]=fminf(extent[0],y);extent[1]=fmaxf(extent[1],y+h);}
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<256;child=rd64(child+0x240))content_extent(child,depth+1,budget,x,y,sx,sy,extent);
}
/* Bottom edge, in a node's own units, of its visible children and grandchildren (pictures included). */
static float local_bottom(uint64_t node,unsigned depth){
    float bottom=0;unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<256;child=rd64(child+0x240)){
        if(rd32(child+0x1e0)&2)continue;
        float b=rdss(child+0xf4);if(!isfinite(b)||b<=0||b>8)continue;
        float top=rdss(child+0xc4)+rdss(child+0xd4)*b;
        float edge=top+rdss(child+0x104)*b;
        if(depth<1&&rd64(child+0x220)){float inner=local_bottom(child,depth+1);if(inner>0)edge=fmaxf(edge,top+inner*b);}
        if(isfinite(edge)&&edge<2000)bottom=fmaxf(bottom,edge);
    }
    return bottom;
}
/* Selection-panel tab pages (under a tab CONTENT or directly in a subpanel's column) keep the panel
 * scale; a page that would run below the screen even in the tallest frame (the generator's power
 * column, 323 units drawn in a 248-unit box) is scaled down to the room left under its tab row.
 * The room comes from the fixed frame geometry, never from the page's own (shrinking) parent. */
static void selection_tab_fit(CPU *cpu,uint64_t node){
    uint64_t parent=rd64(node+0x230);const char *parent_name=parent?law_photo_name(parent):NULL;
    if(!parent_name||(strcmp(parent_name,"CONTENT")&&!strstr(parent_name,"SUBPANEL"))||!law_photo_ancestor(node,"EXT_SELECTION_PANEL"))return;
    mobile_layout *base=remember_layout(node);if(!base||base->sx<=0||base->sy<=0)return;
    float scale=1;
    if(applied_scale>100&&isfinite(selection_limit)){
        unsigned budget=3000;float content=visual_extent(node,0,&budget);
        float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
        float top=y+sy*(rdss(node+0xc4)+rdss(node+0xd4)*rdss(node+0xf4));
        if(content>0&&sy>0&&isfinite(top)){
            float room=(selection_limit-top)/(sy*base->sy),standard=(selection_limit_standard-top)/(sy*base->sy);
            // A page just over the standard frame (construction progress: +5 units) shrinks a little and
            // keeps the bottom HUD; only clearly taller pages fold the HUD and use the full height.
            if(standard>0&&content>standard*1.01f&&content<=standard/.9f)scale=standard/content;
            else if(room>0&&content>room*1.01f)scale=fmaxf(.5f,room/content);
        }
    }
    layout_scale(cpu,node,base->sx*scale,base->sy*scale);
}

/* World-map site/settlement panel: the desktop position leaves its lower half (Explore, transport
 * buttons) below a phone screen.  Lift it until its content fits under the top bar. */
static struct {uint64_t node;uint32_t id;float written,shift;} world_site;
static void world_site_fit(CPU *cpu,uint64_t node,float height){
    float y=rdss(node+0xc4);uint32_t id=rd32(node+0x1e4);
    unsigned ours=world_site.node==node&&world_site.id==id&&fabsf(y-world_site.written)<.01f;
    if(ours)y-=world_site.shift;                 /* the engine's own position */
    float shift=0;
    /* A dilemma site (picture, description, findings) is taller than the phone between the top bar and
     * the bottom edge: shrink the panel's ANIM content to the room, then lift it as before. */
    uint64_t anim=law_photo_child(node,"ANIM");mobile_layout *anim_base=anim?remember_layout(anim):NULL;
    if(anim_base&&applied_scale<=100)layout_scale(cpu,anim,anim_base->sx,anim_base->sy);
    if(applied_scale>100){
        float x0,y0,sx,sy;parent_transform(node,&x0,&y0,&sx,&sy);
        float extent[2]={INFINITY,-INFINITY};unsigned budget=600;
        content_extent(node,0,&budget,x0,y0,sx,sy,extent);
        if(ours)extent[0]-=world_site.shift*sy,extent[1]-=world_site.shift*sy;
        if(anim_base&&anim_base->sy>0&&isfinite(extent[0])&&isfinite(extent[1])){
            // Texts and buttons only; the findings box under the last caption needs ~60 more units.
            float current=rdss(anim+0xf4)/anim_base->sy,span=extent[1]-extent[0],room=height-6-62-60,fit=1;
            if(current>0&&span>0&&span/current>room)fit=fmaxf(.6f,room/(span/current));
            if(fabsf(fit-current)>.005f){layout_scale(cpu,anim,anim_base->sx*fit,anim_base->sy*fit);return;}  /* re-measure next pass */
        }
        if(isfinite(extent[0])&&isfinite(extent[1])&&sy>0){
            if(extent[1]>height-66)shift=(height-66-extent[1])/sy;   /* keep the findings box on screen too */
            if(extent[0]+shift*sy<62)shift=(62-extent[0])/sy;
        }
    }
    if(ours&&fabsf(shift-world_site.shift)<.01f)return;
    if(!ours&&!shift)return;
    layout_position(cpu,node,rdss(node+0xc0),y+shift);
    world_site.node=node;world_site.id=id;world_site.written=y+shift;world_site.shift=shift;
}

/* Full-screen story and dilemma screens keep their PC composition fitted to the screen (scale 0.5 at
 * 200%), which leaves an 11-unit description at ~20 px.  Grow only the text column (title block,
 * description, choices) around its own centre, as far as the screen allows (at most 1.45x); the
 * art and ink masks keep their places.  Children of a target scale with it. */
static unsigned dilemma_target(const char *name){
    static const char *const names[]={"dillema_ornaments_and_title","dillema_title_and_ornaments","title_panel","CHOICES","CHOICE_DESCRIPTION",
        "DESCRIPTION_LAYOUT","DESCRIPTION_TEXT","TITLE_TEXT","ARC_SUBTITLE_TEXT","DILEMMA_TYPE_TEXT"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i)if(!strcmp(name,names[i]))return 1;
    return 0;
}
/* Glyph bounds of the visible texts under a target, in the target's own units (container targets
 * such as title_panel are 64-unit placeholders whose titles extend far beyond them). */
static void dilemma_local_bounds(uint64_t node,unsigned depth,float x,float y,float s,float box[4]){
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<64;child=rd64(child+0x240)){
        if(rd32(child+0x1e0)&2)continue;
        float a=rdss(child+0xf0),b=rdss(child+0xf4);if(!(a>0&&b>0))continue;
        float cx=x+s*(rdss(child+0xc0)+rdss(child+0xd0)*a),cy=y+s*(rdss(child+0xc4)+rdss(child+0xd4)*b);
        if(rd64(child)==GVA(0x141DED7D8)){
            float w=rdss(child+0x100),h=rdss(child+0x104),ink=fminf(rdss(child+0x4d0),w);
            if(ink>0&&h>0&&w<4000&&h<4000){
                float left=cx+s*a*(w-ink)*.5f;
                box[0]=fminf(box[0],left);box[1]=fminf(box[1],cy);box[2]=fmaxf(box[2],left+s*a*ink);box[3]=fmaxf(box[3],cy+s*b*h);
            }
        }
        if(depth<4)dilemma_local_bounds(child,depth+1,cx,cy,s*a,box);
    }
}
static void dilemma_collect(uint64_t node,unsigned depth,uint64_t *out,unsigned *count){
    if(depth>16||*count>=16||(rd32(node+0x1e0)&2))return;
    const char *name=law_photo_name(node);
    if(depth&&name&&dilemma_target(name)){out[(*count)++]=node;return;}
    unsigned children=0;
    for(uint64_t child=rd64(node+0x220);child&&children++<256;child=rd64(child+0x240))dilemma_collect(child,depth+1,out,count);
}
static void dilemma_column(CPU *cpu,uint64_t root,float width,float height){
    uint64_t nodes[16];unsigned count=0;dilemma_collect(root,0,nodes,&count);
    if(!count)return;
    float rx,ry,rsx,rsy;parent_transform(root,&rx,&ry,&rsx,&rsy);
    unsigned grow=applied_scale>100&&rsy*rdss(root+0xf4)<.65f;
    mobile_layout *base[16];float origin[16][2],parent[16][4],rect[16][4],box[4]={INFINITY,INFINITY,-INFINITY,-INFINITY};
    float choices[2]={INFINITY,-INFINITY};
    for(unsigned i=0;i<count;++i){
        base[i]=remember_layout(nodes[i]);if(!base[i])return;
        float x,y,sx,sy;parent_transform(nodes[i],&x,&y,&sx,&sy);
        parent[i][0]=x;parent[i][1]=y;parent[i][2]=sx;parent[i][3]=sy;
        origin[i][0]=x+sx*(base[i]->x+base[i]->px*base[i]->sx);origin[i][1]=y+sy*(base[i]->y+base[i]->py*base[i]->sy);
        float w=rdss(nodes[i]+0x100)*base[i]->sx*sx,h=rdss(nodes[i]+0x104)*base[i]->sy*sy,left=origin[i][0];
        if(rd64(nodes[i])==GVA(0x141DED7D8)){
            // Titles sit in 600-800 unit boxes around short centred lines: measure the glyphs.
            float ink=rdss(nodes[i]+0x4d0)*base[i]->sx*sx;
            if(!(ink>0)){rect[i][0]=NAN;continue;}
            if(ink<w){left+=(w-ink)*.5f;w=ink;}
        }
        rect[i][0]=left;rect[i][1]=origin[i][1];rect[i][2]=left+w;rect[i][3]=origin[i][1]+h;
        if(!(isfinite(w)&&isfinite(h)&&w>0&&h>0&&w<4000&&h<4000))rect[i][0]=NAN;
        if(rd64(nodes[i])!=GVA(0x141DED7D8)){
            float inner[4]={INFINITY,INFINITY,-INFINITY,-INFINITY},s=base[i]->sx*sx;
            dilemma_local_bounds(nodes[i],0,0,0,1,inner);
            if(inner[2]>inner[0]&&inner[3]>inner[1]&&isfinite(s)&&s>0){
                float l=origin[i][0]+inner[0]*s,t=origin[i][1]+inner[1]*s,r=origin[i][0]+inner[2]*s,b=origin[i][1]+inner[3]*s;
                if(!isfinite(rect[i][0])){rect[i][0]=l;rect[i][1]=t;rect[i][2]=r;rect[i][3]=b;}
                else{rect[i][0]=fminf(rect[i][0],l);rect[i][1]=fminf(rect[i][1],t);rect[i][2]=fmaxf(rect[i][2],r);rect[i][3]=fmaxf(rect[i][3],b);}
            }
        }
        const char *name=law_photo_name(nodes[i]);
        if(name&&!strcmp(name,"CHOICES")&&isfinite(rect[i][0])){choices[0]=rect[i][0];choices[1]=rect[i][2];}
    }
    // The column is what lines up with the choices; captions elsewhere on the art stay out of it.
    float reach=isfinite(choices[0])?(choices[1]-choices[0])*.75f:INFINITY;
    unsigned column[16]={0};
    for(unsigned i=0;i<count;++i){
        float center=(rect[i][0]+rect[i][2])*.5f;
        // Empty or unmeasured texts follow the column; captions off to the side keep their place.
        column[i]=!isfinite(rect[i][0])||!isfinite(choices[0])||(center>=choices[0]-reach&&center<=choices[1]+reach);
        if(!isfinite(rect[i][0])||!column[i])continue;
        box[0]=fminf(box[0],rect[i][0]);box[1]=fminf(box[1],rect[i][1]);
        box[2]=fmaxf(box[2],rect[i][2]);box[3]=fmaxf(box[3],rect[i][3]);
    }
    float k=1,cx=(box[0]+box[2])*.5f,cy=(box[1]+box[3])*.5f;
    if(grow&&box[2]>box[0]&&box[3]>box[1]){
        // A 2.2:1 phone shows ~76 canvas units beyond each side of the 16:9 canvas.
        float right=screen_canvas_right(),left=width-right;
        k=1.45f;
        if(cx-box[0]>1)k=fminf(k,(cx-left-8)/(cx-box[0]));
        if(box[2]-cx>1)k=fminf(k,(right-8-cx)/(box[2]-cx));
        if(cy-box[1]>1)k=fminf(k,(cy-6)/(cy-box[1]));
        if(box[3]-cy>1)k=fminf(k,(height-6-cy)/(box[3]-cy));
        if(!(k>1))k=1;
    }
    {static float logged;if(fabsf(k-logged)>.01f){logged=k;fprintf(stderr,"FP_DILEMMA_COLUMN panel=%s targets=%u box=%.0f,%.0f,%.0f,%.0f k=%.2f\n",
        law_photo_name(root)?law_photo_name(root):"?",count,box[0],box[1],box[2],box[3],k);}}
    for(unsigned i=0;i<count;++i){
        float sx=base[i]->sx*k,sy=base[i]->sy*k;
        float ox=cx+(origin[i][0]-cx)*k,oy=cy+(origin[i][1]-cy)*k;
        float px=parent[i][2]>0?(ox-parent[i][0])/parent[i][2]-base[i]->px*sx:base[i]->x;
        float py=parent[i][3]>0?(oy-parent[i][1])/parent[i][3]-base[i]->py*sy:base[i]->y;
        if(k==1||!column[i]){sx=base[i]->sx;sy=base[i]->sy;px=base[i]->x;py=base[i]->y;}
        layout_scale(cpu,nodes[i],sx,sy);layout_position(cpu,nodes[i],px,py);
    }
}

static void prepare_canvas(CPU *cpu,uint64_t node,unsigned depth,unsigned parent_canvas,unsigned *budget) {
    if(!node||depth>32||!*budget)return;
    if(switch_button_layout(cpu,node))return;
    if(rd32(node+0x1e0)&2)return;
    --*budget;
    {
        int docked=ui_index_find(docked_index,8191u,node,rd32(node+0x1e4));
        if(docked>=0)mobile_docked[docked].seen=ui_pass;
    }
    unsigned dock=rd32(node+0x138),anchors=rd32(node+0x13c);
    const char *node_name=(const char*)(uintptr_t)rd64(node+0x1d8);
    unsigned rotated=switch_mode&&(fabsf(rdss(node+0xe0))>.01f||fabsf(rdss(node+0xe4))>.01f||fabsf(rdss(node+0xe8))>.01f);
    unsigned header=node_name&&!strcmp(node_name,"HEADER_LAYOUT");
    if(phone_hud_covered&&phone_side_hud(node_name))return;
    if(hud_bottom_panel(node)){
        mobile_layout *base=remember_layout(node);unsigned covered=applied_scale>100&&selection_cover_pass==ui_pass;
        if(base)layout_scale(cpu,node,covered?0:base->sx,covered?0:base->sy);
    }
    float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
    if(phone_search_panel(cpu,node,width,height,budget))return;
    if(mobile_scouts_layout(cpu,node,width,height))return;
    if(node_name&&(!strcmp(node_name,"EXT_SOCIETY_ISSUES_PANEL")||!strcmp(node_name,"SEARCH_CITIZENS_BUTTON"))){
        mobile_layout *base=remember_layout(node);
        if(base){
            if(!strcmp(node_name,"EXT_SOCIETY_ISSUES_PANEL")){
                if(applied_scale<=100)law_photo_restore(cpu,node);
                else if(law_photo_remember(node)&&rd32(node+0x13c))call_value(cpu,UINT64_C(0x140EC30A0),node,0);
                /* Switch: the bars sit lower (no empty frame under them), the issue icons a little higher. */
                layout_position(cpu,node,439.5f+(width-1280)*.5f,base->y-(switch_mode?6.f:0.f));
                mobile_issue_row(cpu,node);
            } else {
                static unsigned logged;
                if(!logged){logged=1;fprintf(stderr,"FP_SEARCH_BASE x=%.2f y=%.2f\n",base->x,base->y);}
                /* The magnifier's ring touches the left end of the population box, as on PC (the Switch
                 * preset parks it right of the box, off the screen: use the PC place there too). */
                float home_x=switch_mode?58.f:base->x,home_y=switch_mode?-54.5f:base->y;
                layout_position(cpu,node,applied_scale>100?-139.5f:home_x,applied_scale>100?-40:home_y);
                unsigned covered=applied_scale>100&&selection_cover_pass==ui_pass;   /* folded with the bottom HUD */
                layout_scale(cpu,node,covered?0:base->sx,covered?0:base->sy);
            }
        }
        unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))prepare_canvas(cpu,child,depth+1,0,budget);
        return;
    }
    if(node_name&&!strcmp(node_name,"INGAME_MENU_BUTTON")){
        uint64_t parent=rd64(node+0x230);const char *parent_name=parent?law_photo_name(parent):NULL;
        if(parent_name&&!strcmp(parent_name,"TOP_LEFT_BUTTONS"))menu_button_scale(cpu,node);
    }
    if(node_name&&!strcmp(node_name,"EXT_WORLD_MAP_SELECTION_PANEL"))world_site_fit(cpu,node,height);
    if(node_name&&node_name[0]=='E'&&!strcmp(node_name,"EXT_CONSTRUCTION_PANEL")){
        /* While a building, street or demolition tool is active the menu folds away (as on the Switch)
         * so the tool works on the city under the centre cursor; it returns when the tool ends. */
        mobile_layout *base=remember_layout(node);unsigned fold=painter_kind!=0;
        if(base)layout_scale(cpu,node,fold?0:base->sx,fold?0:base->sy);
        if(!fold&&ui_shown_chain(node)){
            float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
            float top=y+sy*(rdss(node+0xc4)+rdss(node+0xd4)*rdss(node+0xf4));
            /* With the menu open, keep the cursor in the visible part of the city. */
            construction_menu_aim=isfinite(top)&&top>66&&top<height?(66+top)*.5f/height:.5f;
        }
    }
    if(node_name&&node_name[0]=='c'&&!strcmp(node_name,"clip")){
        uint64_t holder=rd64(node+0x230);const char *holder_name=holder?law_photo_name(holder):NULL;
        if(holder_name&&!strcmp(holder_name,"hide_expeditions_container")){
            /* The Switch scout portraits reached ~12 units past the screen's right edge at 2x. */
            mobile_layout *base=remember_layout(node);
            if(base&&fabsf(base->x+465)<.5f)layout_position(cpu,node,base->x-(switch_mode&&applied_scale>100?12.f:0.f),base->y);
        }
        uint64_t button=rd64(node+0x230),owner=button?rd64(button+0x230):0;
        const char *button_name=button?law_photo_name(button):NULL,*owner_name=owner?law_photo_name(owner):NULL;
        if(button_name&&!strcmp(button_name,"BUTTON")&&owner_name&&(!strcmp(owner_name,"NOTIFICATION")||!strcmp(owner_name,"TUTORIAL_NOTIFICATION"))
           &&law_photo_ancestor(owner,"EXT_NOTIFICATION_PANEL")){
            unsigned fold=notifications_collapsed;
            mobile_layout *base=remember_layout(node),*button_base=remember_layout(button);
            if(base)layout_scale(cpu,node,fold?0:base->sx,fold?0:base->sy);
            if(button_base&&button_base->w>0){
                /* The icon: 66 of the 406 units of a notification, the "?" 56 of the tutorial's 440. */
                float width=fold?(owner_name[0]=='T'?56.f:66.f):button_base->w;
                if(fabsf(rdss(button+0x210)-width)>.01f){float size[4]={width,button_base->h,0,0};call(cpu,UINT64_C(0x140EC4C70),button,size,0,0,0);}
            }
        }
    }
    if(node_name&&node_name[0]=='l'&&switch_mode&&!strcmp(node_name,"line_bottom")){
        /* The PC frame line of the main buttons stays in the Switch HUD without its buttons: an empty
         * bracket under Discontent/Hope. */
        uint64_t parent=rd64(node+0x230);const char *parent_name=parent?law_photo_name(parent):NULL;
        mobile_layout *base=parent_name&&!strcmp(parent_name,"MAIN_BUTTONS")?remember_layout(node):NULL;
        if(base)layout_scale(cpu,node,0,0);
    }
    if(node_name&&node_name[0]=='c'&&!strcmp(node_name,"critical_texts_layout")&&law_photo_ancestor(node,"DANGER_BUTTONS")){
        mobile_issue_count(cpu,node);
    }
    if(node_name&&node_name[0]=='h'&&!strcmp(node_name,"hide_expeditions_container")){
        /* The construction buttons of TouchControls take the right edge: fold the scout portraits. */
        mobile_layout *base=remember_layout(node);unsigned fold=construction_open||painter_kind;
        if(base)layout_scale(cpu,node,fold?0:base->sx,fold?0:base->sy);
    }
    if(node_name&&node_name[0]=='V'&&!strcmp(node_name,"VISIBLE_WITH_MAP_UNLOCK")){
        /* The Switch preset draws the world-map button and scout portraits at 0.9 (PC 0.75): on the
         * 2x HUD they ran past the screen's right edge.  Use the PC size there. */
        uint64_t parent=rd64(node+0x230);const char *parent_name=parent?law_photo_name(parent):NULL;
        mobile_layout *base=parent_name&&!strcmp(parent_name,"EXT_EXPEDITIONS_PANEL")?remember_layout(node):NULL;
        if(base){
            unsigned fit=switch_mode&&applied_scale>100;float s=fit?fminf(base->sx,.75f):base->sx;
            layout_scale(cpu,node,s,s);
            /* No position here: the panel slides this node in, and a base taken during the slide
             * pinned the map and beacon buttons off screen. */
        }
    }
    if(applied_scale>100&&node_name&&!strcmp(node_name,"EXT_RESEARCH_FINISHED_PANEL")){
        /* "Research finished" sits at x 501 in TOP with a 281-unit card: on the 2x HUD it ended 66 units
         * past a 2.2:1 screen.  Keep its right edge inside the screen (its slide-in starts further right). */
        float limit=screen_canvas_right()-8-rdss(node+0x100)*rdss(node+0xf0);
        if(rdss(node+0xc0)>limit)layout_position(cpu,node,limit,rdss(node+0xc4));
    }
    if(node_name&&!strcmp(node_name,"picture_interior_blur")&&law_photo_ancestor(node,"EXT_WORLD_MAP_SELECTION_PANEL")){
        /* This picture-sized overlay blurred the whole site illustration on the phone canvas; the
         * picture's own SFX_HEADER camera is sharp once its viewport follows the panel scale. */
        mobile_layout *base=remember_layout(node);
        if(base)layout_scale(cpu,node,applied_scale>100?0:base->sx,applied_scale>100?0:base->sy);
    }
    if(newsflash_panel(node)){
        remember_layout(node);
        float x=newsflash_x();
        if(fabsf(rdss(node+0xc0)-x)>.01f){
            float position[4];memcpy(position,(void*)(uintptr_t)(node+0xc0),16);position[0]=x;
            call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
        }
        unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))prepare_canvas(cpu,child,depth+1,0,budget);
        return;
    }
    if(node_name&&!strcmp(node_name,"TOOLTIPS")&&rd64(node)==GVA(0x141D78A60)){
        uint64_t parent=rd64(node+0x230);const char *parent_name=parent?(const char*)(uintptr_t)rd64(parent+0x1d8):NULL;
        if(parent_name&&!strcmp(parent_name,"FRAME")){
            // Tooltip positions already use the live canvas; their 64px container is an origin, not a centred dialog.
            if(rd32(node+0x13c))call_value(cpu,UINT64_C(0x140EC30A0),node,0);
            if(rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
            layout_position(cpu,node,0,0);
            unsigned count=0;
            for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))prepare_canvas(cpu,child,depth+1,0,budget);
            return;
        }
    }
    if(law_photo_layout(cpu,node,width,height)){
        unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))prepare_canvas(cpu,child,depth+1,0,budget);
        return;
    }
    if(hud_bottom_panel(node)){
        mobile_layout *base=remember_layout(node);
        if(base){
            unsigned mobile=applied_scale>100;
            if(mobile&&rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
            float w=mobile?width:base->w,h=base->h;
            if(fabsf(rdss(node+0x210)-w)>.01f||fabsf(rdss(node+0x214)-h)>.01f){float size[4]={w,h,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);}
            layout_position(cpu,node,mobile?0:base->x,mobile?-194:base->y);
            if(!mobile&&rd32(node+0x138)!=base->dock)call_value(cpu,UINT64_C(0x140EC39C0),node,base->dock);
        }
    }
    if(node_name&&(!strcmp(node_name,"BOTTOM")||!strcmp(node_name,"BOTTOM_static"))&&fabsf(rdss(node+0x214))<.01f
       &&fabsf(rdss(node+0xc4)-height)>.01f){
        float position[4]={0,height,0,1};call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
    }
    if(node_name&&!strcmp(node_name,"EXT_TECHNOLOGY_PANEL")){
        technology_contents(cpu,node,0,budget,width,height);if(applied_scale>100)return;
    }
    if(original_dialog(node)&&rdss(node+0x210)>0&&rdss(node+0x214)>0){
        // Full-screen dialogs retain their complete PC layout; the HUD keeps the selected scale.
        float w=rdss(node+0x210),h=rdss(node+0x214),size[4]={w,h,0,0};
        float bounds[4]={0,0,w,h};
        unsigned selection=!strcmp(node_name,"EXT_SELECTION_PANEL"),people=!strcmp(node_name,"EXT_POPULATION_PANEL");
        if(selection){
            phone_list_contents(cpu,node,0,budget,0,154);
            mobile_layout *base=remember_layout(node);float content_height=225;
            unsigned columns=base?selection_columns(cpu,node,0,&content_height,0):0;
            if(columns==1){content_height=225;selection_columns(cpu,node,0,&content_height,1);}
            if(base){w=base->w;h=base->h;}
            // Native action buttons extend below their nominal pane; keep their outlines inside the frame.
            selection_limit=INFINITY;
            if(columns==1&&applied_scale>100){
                float standard=selection_standard_scale(width,height),top=selection_frame_top(width,height,INFINITY);
                w=684;h=fminf(content_height+16,(height-8-top)/standard);
                selection_limit=height-8-(16+fmaxf(0,selection_pad_bottom))*standard;
                selection_limit_standard=height-76-(16+fmaxf(0,selection_pad_bottom))*standard;
            }
            {static unsigned logged_columns=99;
             if(columns!=logged_columns){logged_columns=columns;fprintf(stderr,"FP_SELECTION_COLUMNS columns=%u size=%.1fx%.1f\n",columns,w,h);}}
            if(selection_background(cpu,node,w,h))return;
            size[0]=w;size[1]=h;bounds[2]=w;bounds[3]=h;
        }
        if(!strcmp(node_name,"EXT_TECHNOLOGY_PANEL")){
            // The technology canvas is 900x600; its live tree, header and close button extend beyond it.
            float content[4]={INFINITY,INFINITY,-INFINITY,-INFINITY};unsigned count=0;
            for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240))
                technology_bounds(child,1,0,0,1,1,content);
            if(content[2]>content[0]&&content[3]>content[1])memcpy(bounds,content,sizeof(bounds));
        }
        if(!strcmp(node_name,"EXT_RESOURCES_PANEL")){
            /* Its dark band starts 14 units down: at 12 the scene showed through a 3-4 px strip above it. */
            bounds[1]=14;bounds[3]=195;
            if(applied_scale>100){bounds[0]=84;bounds[2]=924;}
        }
        unsigned economy=!strcmp(node_name,"EXT_ECONOMY_PANEL");
        if(economy){
            phone_economy_text(cpu,node,0,budget);
            if(applied_scale>100){bounds[0]=switch_mode?0:70;bounds[1]=switch_mode?0:4;bounds[2]=switch_mode?1280:1200;bounds[3]=switch_mode?720:698;}
        }
        float fit_w=bounds[2]-bounds[0],fit_h=bounds[3]-bounds[1];
        unsigned compact=selection&&applied_scale>100&&w>342;
        unsigned full=fabsf(w-1280)<.01f&&fabsf(h-720)<.01f,population=!strcmp(node_name,"EXT_POPULATION_PANEL")||!strcmp(node_name,"EXT_SELECTION_PANEL");
        unsigned resources=!strcmp(node_name,"EXT_RESOURCES_PANEL"),society=strstr(node_name,"SOCIETY_STATE_PANEL")!=NULL;
        float scale=full?fminf(width/fit_w,height/fit_h):fminf(1,fminf((width-24)/fit_w,(height-(compact?132:population?80:24))/fit_h));
        if(people)scale=1;
        if(resources&&applied_scale>100)scale=fminf(1,(width-8)/840.f);
        float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
        float transform[4]={scale/sx,scale/sy,0,1};
        float left=population?width-12-fit_w*scale:(width-fit_w*scale)*.5f;
        float top=population?height-68-fit_h*scale:(height-fit_h*scale)*.5f;
        if(people){left=width-260;top=height-h-18;}
        // Keep selection actions between the top resource bar and the bottom HUD; one scale for every
        // building and tab, taller pages extend the frame downwards over the HUD.
        if(compact){scale=selection_standard_scale(width,height);transform[0]=scale/sx;transform[1]=scale/sy;
            left=(width-w*scale)*.5f;top=selection_frame_top(width,height,h*scale);
            if(h>241.5f)selection_cover_pass=ui_pass;}
        if(people&&applied_scale>100&&selection_cover_pass==ui_pass){transform[0]=0;transform[1]=0;}
        if(resources){
            top=0;
            /* The temperature dial (TEMPERATURE_OVERLAY_BUTTON, x 493 of the panel) is the centre of the
             * PC's top bar; the panel's own box is not symmetric about it, so centring the box put the
             * dial 8 units left at 2x and 43 left at 100 %. */
            uint64_t ani=law_photo_child(node,"ANI"),dial=ani?law_photo_child(ani,"TEMPERATURE_OVERLAY_BUTTON"):0;
            float centre=dial?rdss(ani+0xc0)+rdss(dial+0xc0):NAN;
            if(isfinite(centre)&&centre>bounds[0]&&centre<bounds[2])left=width*.5f-(centre-bounds[0])*scale;
        }
        if(society)top=height-(applied_scale>100?53:67)+(switch_mode?12:0);
        float position[4]={(left-x)/sx-(rdss(node+0xd0)+bounds[0])*transform[0],(top-y)/sy-(rdss(node+0xd4)+bounds[1])*transform[1],0,1};
        if(rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
        if(rd32(node+0x13c))call_value(cpu,UINT64_C(0x140EC30A0),node,0);
        if(fabsf(rdss(node+0x100)-w)>.01f||fabsf(rdss(node+0x104)-h)>.01f)call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
        if(fabsf(rdss(node+0xf0)-transform[0])>.0001f||fabsf(rdss(node+0xf4)-transform[1])>.0001f)call(cpu,UINT64_C(0x140EC46E0),node,transform,0,0,0);
        if(fabsf(rdss(node+0xc0)-position[0])>.01f||fabsf(rdss(node+0xc4)-position[1])>.01f)call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
        if(!strncmp(node_name,"EXT_DILEMMA",11))dilemma_column(cpu,node,width,height);
        unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))
            dialog_contents(cpu,child,depth+1,budget,width,height);
        return;
    }
    if(node_name&&!strcmp(node_name,"BUILDING_TABS_PANEL")&&rdss(node+0x210)>0){
        float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
        float right=0;unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<64;child=rd64(child+0x240))
            if(!(rd32(child+0x1e0)&2))right=fmaxf(right,rdss(child+0xc0)+(rdss(child+0xd0)+rdss(child+0x100))*rdss(child+0xf0));
        if(right<=0)right=rdss(node+0x210);
        float factor=fminf(1,(width-30)/(right*sx));
        float transform[4]={factor,factor,0,1};
        if(fabsf(rdss(node+0xf0)-factor)>.0001f||fabsf(rdss(node+0xf4)-factor)>.0001f)
            call(cpu,UINT64_C(0x140EC46E0),node,transform,0,0,0);
        return;
    }
    unsigned widget=construction_widget(node);
    if(small_event_mask(node)){
        float x=149.6f+(width-1280)*.5f,y=7+(height-720)*.5f;
        if(fabsf(rdss(node+0xc0)-x)>.01f||fabsf(rdss(node+0xc4)-y)>.01f){
            float position[4]={149.6f,7,0,1};call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
        }
    }
    if(widget){
        float position[4];memcpy(position,(void*)(uintptr_t)(node+0xc0),16);
        float base=widget==1?84:widget==2?11:1214;
        float expected=base+(1280-width)*.5f*(widget==3?-1:1);
        if(fabsf(position[0]-expected)>.01f){position[0]=base;call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);}
        if(widget==1&&fabsf(rdss(node+0x210)-(width+38))>.01f){
            float size[4]={width+38,135,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
        }
    }
    unsigned canvas=0;
    unsigned law=law_widget(node);
    if(law==3){
        float size[4]={width-(rd64(node)==GVA(0x141DA74C8)?0:4),height,0,0};
        if(fabsf(rdss(node+0x210)-size[0])>.01f||fabsf(rdss(node+0x214)-size[1])>.01f)
            call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
        // The law tree is scrollable content, not a dialog to shrink to the viewport.
        unsigned count=0;
        for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))prepare_canvas(cpu,child,depth+1,0,budget);
        return;
    }
    {
        uint32_t node_id=rd32(node+0x1e4);
        int partial=ui_index_find(partial_index,1023u,node,node_id);
        if(partial>=0)canvas=partial_canvases[partial].axes;
        if(ui_index_find(scaled_index,1023u,node,node_id)>=0)canvas=3;
    }
    if(fabsf(rdss(node+0x210)-1280)<.01f||fabsf(rdss(node+0x210)-width)<.01f
       ||((parent_canvas&1)&&(dock==1||dock==4||dock==5)))canvas|=1;
    if(fabsf(rdss(node+0x214)-720)<.01f||fabsf(rdss(node+0x214)-height)<.01f
       ||((parent_canvas&2)&&(dock==2||dock==3||dock==5)))canvas|=2;
    if(parent_canvas&&canvas&&canvas!=3){
        uint32_t partial_id=rd32(node+0x1e4);int partial=ui_index_find(partial_index,1023u,node,partial_id);
        unsigned i=partial>=0?(unsigned)partial:partial_count;
        if(i==partial_count&&i<512){partial_canvases[i].node=node;partial_canvases[i].id=partial_id;partial_canvases[i].axes=canvas;
            ui_index_add(partial_index,1023u,node,partial_id,(int)i);++partial_count;}
        float size[4];memcpy(size,(void*)(uintptr_t)(node+0x210),16);
        if(canvas&1)size[0]=width;
        if(canvas&2)size[1]=height;
        if(fabsf(size[0]-rdss(node+0x210))>.01f||fabsf(size[1]-rdss(node+0x214))>.01f)
            call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
    }
    if(!dock&&parent_canvas){
        float size[4];memcpy(size,(void*)(uintptr_t)(node+0x210),16);unsigned restore=0;
        uint64_t parent=rd64(node+0x230);
        const char *origin_name=(const char*)(uintptr_t)rd64(node+0x1d8);
        if(parent&&(parent_canvas&1)&&(anchors&14)==14&&fabsf(size[1])<.01f
           &&origin_name&&(!strcmp(origin_name,"BOTTOM_static")||!strcmp(origin_name,"BOTTOM"))){
            // Dock the zero-height HUD origin; stretching its requested width compounds live scale changes.
            dock=4;canvas|=1;
            call_value(cpu,UINT64_C(0x140EC30A0),node,0);
            call_value(cpu,UINT64_C(0x140EC39C0),node,dock);
        }
        // Zero-size stretch points in the PC HUD otherwise become negative on a smaller canvas.
        if(!dock&&parent&&(parent_canvas&1)&&(anchors&6)==6&&fabsf(size[0])<.01f){size[0]=rdss(parent+0x100);canvas|=1;restore=1;}
        if(parent&&(parent_canvas&2)&&(anchors&9)==9&&fabsf(size[1])<.01f){size[1]=rdss(parent+0x104);canvas|=2;restore=1;}
        if(restore)call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
    }
    if(parent_canvas){
        if(canvas==3&&dock!=5){
            pthread_mutex_lock(&input_mutex);
            uint32_t docked_id=rd32(node+0x1e4);int docked=ui_index_find(docked_index,8191u,node,docked_id);
            unsigned i=docked>=0?(unsigned)docked:mobile_docked_count;
            if(i==mobile_docked_count&&i<4096){
                mobile_docked[i].node=node;mobile_docked[i].id=docked_id;
                mobile_docked[i].y=rdss(node+0xc4);
                mobile_docked[i].translated=rd64(node)==GVA(0x141D796F8)?1:0;
                mobile_docked[i].seen=ui_pass;
                ui_index_add(docked_index,8191u,node,docked_id,(int)i);
                ++mobile_docked_count;
            }
            pthread_mutex_unlock(&input_mutex);
            call_value(cpu,UINT64_C(0x140EC39C0),node,5);
        }
        else if(!header&&!rotated&&!canvas&&!dock&&(((parent_canvas&1)&&!(anchors&6))||((parent_canvas&2)&&!(anchors&9)))){
            float x=rdss(node+0xc0)+rdss(node+0xd0)+rdss(node+0x100)*rdss(node+0xf0)*.5f;
            float y=rdss(node+0xc4)+rdss(node+0xd4)+rdss(node+0x104)*rdss(node+0xf4)*.5f;
            if(rd64(node)==GVA(0x141D78A60)&&fabsf(rdss(node+0x210)-64)<.01f&&rd64(node+0x220)){
                float left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;unsigned count=0;
                for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240)){
                    if(rd32(child+0x1e0)&2)continue;
                    float cx=rdss(child+0xc0)+rdss(child+0xd0),cy=rdss(child+0xc4)+rdss(child+0xd4);
                    left=fminf(left,cx);right=fmaxf(right,cx+rdss(child+0x100));
                    top=fminf(top,cy);bottom=fmaxf(bottom,cy+rdss(child+0x104));
                }
                if(isfinite(left)){x=rdss(node+0xc0)+(left+right)*.5f;y=rdss(node+0xc4)+(top+bottom)*.5f;}
            }
            unsigned flags=anchors|16;
            if((parent_canvas&1)&&!(anchors&6)){if(x>1024)flags|=4;else if(x>256)flags|=6;}
            if((parent_canvas&2)&&!(anchors&9)){if(y>576)flags|=8;else if(y>144)flags|=9;}
            if(flags!=anchors){
                float position[4];memcpy(position,(void*)(uintptr_t)(node+0xc0),16);
                uint64_t parent=rd64(node+0x230);float dx=0,dy=0;
                if(parent&&(flags&4)&&!(anchors&4)&&rdss(parent+0x100)>0)
                    dx=(rdss(parent+0x100)-1280)*((flags&2)?.5f:1.f);
                if(parent&&(flags&8)&&!(anchors&8)&&rdss(parent+0x104)>0)
                    dy=(rdss(parent+0x104)-720)*((flags&1)?.5f:1.f);
                call_value(cpu,UINT64_C(0x140EC30A0),node,flags);
                if(dx||dy){position[0]+=dx;position[1]+=dy;call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);}
            }
        }
    }
    if(!header&&!rotated&&parent_canvas&&!canvas&&!dock&&rd64(node+0x220)&&rdss(node+0xf0)>0&&rdss(node+0xf4)>0
       &&(!rd64(node+0x230)||rd64(rd64(node+0x230))!=GVA(0x141DA74C8))
       &&(rd64(node)==GVA(0x141D78A60)||rd64(node)==GVA(0x141DE0A48))){
        uint32_t id=rd32(node+0x1e4);int fitted=ui_index_find(fitted_index,2047u,node,id);
        unsigned i=fitted>=0?(unsigned)fitted:fitted_count;
        pthread_mutex_lock(&input_mutex);
        if(i==fitted_count&&i<1024){
            fitted_panels[i].node=node;fitted_panels[i].id=id;
            fitted_panels[i].x=rdss(node+0xf0);fitted_panels[i].y=rdss(node+0xf4);
            ui_index_add(fitted_index,2047u,node,id,(int)i);++fitted_count;
        }
        pthread_mutex_unlock(&input_mutex);
        if(i<1024){
            float sx=fitted_panels[i].x,sy=fitted_panels[i].y,w=rdss(node+0x100),h=rdss(node+0x104);
            float fit=1;
            if(w*sx>width-16)fit=fminf(fit,(width-16)/(w*sx));
            if(h*sy>height-16)fit=fminf(fit,(height-16)/(h*sy));
            float transform[4];memcpy(transform,(void*)(uintptr_t)(node+0xf0),16);
            sx*=fit;sy*=fit;
            if(fabsf(transform[0]-sx)>.0001f||fabsf(transform[1]-sy)>.0001f){
                float position[4];memcpy(position,(void*)(uintptr_t)(node+0xc0),16);
                position[0]+=(transform[0]-sx)*(rdss(node+0xd0)+w*.5f);
                position[1]+=(transform[1]-sy)*(rdss(node+0xd4)+h*.5f);
                transform[0]=sx;transform[1]=sy;
                call(cpu,UINT64_C(0x140EC46E0),node,transform,0,0,0);
                call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
            }
        }
    }
    if(law==1||law==2||law==4||law==5){
        float position[4];memcpy(position,(void*)(uintptr_t)(node+0xc0),16);
        if(law==1){
            position[0]=(width-rdss(node+0x100)*rdss(node+0xf0))*.5f-rdss(node+0xd0)*rdss(node+0xf0);
            position[1]=(height-rdss(node+0x104)*rdss(node+0xf4))*.5f-rdss(node+0xd4)*rdss(node+0xf4);
        } else if(law==2){
            position[0]=width-8-(rdss(node+0x100)+rdss(node+0xd0))*rdss(node+0xf0);
            position[1]=8-rdss(node+0xd4)*rdss(node+0xf4);
        } else if(law==4)position[0]=width-96;
        else position[0]=0;
        if(fabsf(position[0]-rdss(node+0xc0))>.01f||fabsf(position[1]-rdss(node+0xc4))>.01f)
            call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);
    }
    viewport_decoration(cpu,node,width,height);
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240)){
        /* The next sibling's lines load while this child's subtree is walked (a hint only: the
         * loop still reads the link after the call, which may change the tree). */
        uint64_t next=rd64(child+0x240);
        if(next){__builtin_prefetch((const void*)(uintptr_t)(next+0x100));__builtin_prefetch((const void*)(uintptr_t)(next+0x1c0));
            __builtin_prefetch((const void*)(uintptr_t)(next+0x200));__builtin_prefetch((const void*)(uintptr_t)(next+0x240));}
        prepare_canvas(cpu,child,depth+1,canvas,budget);
    }
}

/* Startup logos (launcher "skip intro videos"): the engine's own novideo bit (byte 0x142985AB8,
 * bit 0x10, set by -novideo at engine start) makes UIVideo::Play/Stop no-ops.  The video engine is
 * created without it, so the bit is held only from the first UI update until the main menu or a
 * city camera appears (at most 90 s); scenario cinematics and tutorial videos play normally. */
static unsigned intro_phase;
static uint64_t intro_since;
static uint64_t intro_now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static unsigned intro_menu_visible(uint64_t node,unsigned depth){
    if(!node||depth>3||(rd32(node+0x1e0)&2))return 0;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    if(name&&name[0]=='E'&&(!strcmp(name,"EXT_MAIN_MENU_PANEL")||!strcmp(name,"EXT_INGAME_MENU_MAIN_PANEL")))return 1;
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<256;child=rd64(child+0x240))if(intro_menu_visible(child,depth+1))return 1;
    return 0;
}
static void intro_videos(uint64_t screen){
    if(intro_phase==2)return;
    uint8_t *flags=(uint8_t*)(uintptr_t)GVA(0x142985AB8);
    if(!intro_phase){
        if(!win64_skip_intro){intro_phase=2;return;}
        __atomic_or_fetch(flags,(uint8_t)0x10,__ATOMIC_RELAXED);intro_phase=1;intro_since=intro_now_ms();
        fprintf(stderr,"FP_INTRO_VIDEOS hold=1\n");return;
    }
    if(camera_available||(screen&&intro_menu_visible(screen,0))||intro_now_ms()-intro_since>90000){
        __atomic_and_fetch(flags,(uint8_t)~0x10,__ATOMIC_RELAXED);intro_phase=2;
        fprintf(stderr,"FP_INTRO_VIDEOS released after_ms=%llu\n",(unsigned long long)(intro_now_ms()-intro_since));
    }
}


/* Free Monument to the Builders of the Generator.  FrostpunkUIMainMenuStatuePanel's "Follow Publisher
 * Page" handler (1419DF130) sets the profile bool CLAIM_STATUE_BUILDING_IN_STATUE_PANEL (string id
 * 142B8ECF0) through 14111C620 and saves the profile (141C2BE30); the main menu hides its statue button
 * and new games offer the monument while it is set.  Settings row 33 shows and changes that bool; the
 * first start claims it once, as if the button had been pressed (the Steam page itself never opens). */
static uint64_t guest_call(CPU *cpu,uint64_t function,uint64_t a,uint64_t b,uint64_t c3){
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=a;c.rdx=b;c.r8=c3;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,function);return c.rax;
}
static int monument_claimed(CPU *cpu,uint64_t profile){
    if(!profile||!rd8(profile+0x128))return -1;   /* GamerProfile::GetData asserts on an uninitialised profile */
    uint64_t out=((cpu->rsp-512)&~UINT64_C(15))+0x100;   /* inside the frame guest_call reserves, above its stack */
    wr8(out,0);wr8(out+1,0);
    guest_call(cpu,UINT64_C(0x14111BA90),profile,out,GVA(0x142B8ECF0));
    return rd8(out)&&rd8(out+1);
}
static void monument_set(CPU *cpu,uint64_t profile,unsigned value){
    guest_call(cpu,UINT64_C(0x14111C620),profile,GVA(0x142B8ECF0),value?1:0);
}
/* Unit 0959 getter of row 33, right after the original call to the profile accessor (rax = profile). */
uint32_t fp_port_monument_get(CPU *cpu){
    int claimed=monument_claimed(cpu,cpu->rax);
    return claimed<0?fp_port_setting_get(FP_SETTING_FREE_BUILDING):(uint32_t)claimed;
}
/* Unit 0959 setter, after fp_port_setting_set: rax = profile, r14 = "profile changed" (saved by Apply),
 * r15 = "requires restart" (the original RequiresRestart confirmation, as for texture quality). */
void fp_port_setting_effects(CPU *cpu,uint32_t id,uint32_t value){
    if(id==FP_SETTING_SWITCH_UI&&value!=(uint32_t)frostpunk_switch_ui()&&cpu->r15)wr8(cpu->r15,1);
    if(id==FP_SETTING_FREE_BUILDING){
        uint64_t profile=cpu->rax;
        if(profile&&rd8(profile+0x128)){
            monument_set(cpu,profile,value);
            if(cpu->r14)wr8(cpu->r14,1);
            fprintf(stderr,"FP_FREE_BUILDING claimed=%u source=settings\n",value?1u:0u);
        }
    }
}
static void free_building_first_start(CPU *cpu){
    if(camera_available||!fp_port_free_building_pending())return;
    uint64_t profile=guest_call(cpu,UINT64_C(0x1410F41B0),GVA(0x142B8A520),0,0);
    int claimed=monument_claimed(cpu,profile);
    if(claimed<0)return;
    if(!claimed){monument_set(cpu,profile,1);guest_call(cpu,UINT64_C(0x141C2BE30),profile,0,0);}
    fp_port_setting_set(FP_SETTING_FREE_BUILDING,1);
    fprintf(stderr,"FP_FREE_BUILDING claimed=1 source=%s\n",claimed?"profile":"first-start");
}

static void loading_hint_scale(CPU *cpu){
    uint64_t node=loading_hint;
    if(!node||rd64(node)!=GVA(0x141EE06E0)||rd32(node+0x1e4)!=loading_hint_id)return;
    mobile_layout *base=remember_layout(node);if(!base)return;
    unsigned requested=fp_port_setting_get(FP_SETTING_UI_SCALE);
    /* Drawn on the 1280x720 canvas (menus run at 100%).  The tip box is 832 units wide (two lines at
     * most) from x +51 and y -10..+54 of the root; a 2.2:1 screen shows ~170 more units each side of
     * this canvas.  At most 1.75x: a full-width tip stays inside the screen, its second line above the
     * bottom edge (16:9 screens get the smaller factor that fits). */
    float aspect=frostpunk_display_aspect?frostpunk_display_aspect():16.f/9.f;
    float margin=fmaxf(0.f,(720.f*aspect-1280.f)*.5f);
    /* The arrow starts 20 units into the root, the 832-unit text ends 883 units into it; the loading
     * spinner sits from x 1139 of the canvas on the same line, so the text ends at 1129. */
    float s=requested>100?fminf(fminf(requested/100.f,1.75f),(1129.f+margin-40.f)/863.f):1.f;
    if(s<=1.001f){layout_scale(cpu,node,base->sx,base->sy);layout_position(cpu,node,base->x,base->y);return;}
    layout_scale(cpu,node,base->sx*s,base->sy*s);
    layout_position(cpu,node,-margin+40.f-20.f*s,720.f-12.f-54.f*s);
}

/* Subtitles of videos ("A New Home" intro) are drawn at the 1280x720 menu size: grow them with the
 * interface scale around their bottom centre, inside the screen's width. */
static void subtitle_scale(CPU *cpu){
    /* The subtitle screen has no input controls, so the control snapshot never visits it: walk the UI
     * manager's screen vector (1429822D0: handles +0, count +8; 140EA8FD0 appends) for FRAME/Subtitles. */
    /* Found again on every pass: the screen is destroyed after each video. */
    subtitle_text=0;
    {
        uint64_t data=rd64(GVA(0x1429822D0));int32_t count=(int32_t)rd32(GVA(0x1429822D0)+8);
        for(int32_t i=0;data&&i<count&&i<64;++i){
            uint64_t handle=rd64(data+8ull*(uint64_t)i),screen=handle?rd64(handle):0;
            unsigned seen=0;
            for(uint64_t root=screen?rd64(screen+0x220):0;root&&seen++<8;root=rd64(root+0x240)){
                const char *root_name=law_photo_name(root);if(!root_name||strcmp(root_name,"FRAME"))continue;
                uint64_t text=law_photo_child(root,"Subtitles");
                if(text&&(rd64(text)==GVA(0x141DED7D8)||rd64(text)==GVA(0x141DAD740))){subtitle_text=text;subtitle_text_id=rd32(text+0x1e4);}
            }
        }
    }
    uint64_t node=subtitle_text;
    if(!node||rd32(node+0x1e4)!=subtitle_text_id||(rd64(node)!=GVA(0x141DED7D8)&&rd64(node)!=GVA(0x141DAD740)))return;
    mobile_layout *base=remember_layout(node);if(!base)return;
    unsigned requested=fp_port_setting_get(FP_SETTING_UI_SCALE);
    float aspect=frostpunk_display_aspect?frostpunk_display_aspect():16.f/9.f,visible=fmaxf(1280.f,720.f*aspect);
    float w=base->w>0?base->w:rdss(node+0x100),s=requested>100?fminf(requested/100.f,1.6f):1;
    if(w>0&&base->sx>0)s=fminf(s,(visible-24)/(w*base->sx));
    if(!(s>1))s=1;
    float sx=base->sx*s,sy=base->sy*s;
    float x=base->x+(base->px+base->w*.5f)*(base->sx-sx),y=base->y+(base->py+base->h)*(base->sy-sy);
    {static uint64_t logged;if(logged!=node){logged=node;
        fprintf(stderr,"FP_SUBTITLES node=%llx box=%.1fx%.1f at=%.1f,%.1f pivot=%.1f,%.1f scale=%.3f\n",
            (unsigned long long)node,base->w,base->h,base->x,base->y,base->px,base->py,s);}}
    layout_scale(cpu,node,sx,sy);layout_position(cpu,node,x,y);
}

void fp_mobile_ui_before(CPU *cpu) {
    intro_videos(cpu->rcx);
    subtitle_scale(cpu);
    free_building_first_start(cpu);
    loading_hint_scale(cpu);
    mobile_overlay_text(cpu);
    fp_mobile_ui_scale();
    if(!panels||!camera_available||pause_menu_visible||rd8(panels+0x865)||rd8(GVA(0x142982F8D)))debug_ready_since=0;
    else if(atomic_load(&debug_requested)){
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        uint64_t milliseconds=(uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
        if(!debug_ready_since)debug_ready_since=milliseconds;
        // Let the Back/Escape release and the original menu transition finish before opening Cheats.
        if(milliseconds-debug_ready_since>=200&&atomic_exchange(&debug_requested,0)){
            CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=panels;c.rdx=2;c.r8=0;
            push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,rd64(rd64(panels)+0x170)-g_image_delta);
            fprintf(stderr,"FP_DEVELOPER_MENU original=1 panel=2\n");
            debug_ready_since=0;
        }
    }
    uint64_t screen=cpu->rcx;
    if(rd8(screen+0x2d5))return;
    unsigned budget=50000;
    ++ui_pass;
    phone_hud_visibility(cpu,screen);
    switch_mode=frostpunk_switch_ui();
    atomic_store(&switch_available,switch_mode&&camera_available&&!building_available&&!road_available&&!phone_hud_covered&&!pause_menu_visible&&!atomic_load(&switch_modal_visible));
    if(!atomic_load(&switch_available))atomic_store(&switch_menu,0);
    if(switch_mode||fp_port_setting_get(FP_SETTING_UI_SCALE)>100||mobile_docked_count||fitted_count||mobile_layout_count)prepare_canvas(cpu,screen,0,0,&budget);
    unsigned count=0;
    for(uint64_t root=rd64(screen+0x220);root&&count++<512;root=rd64(root+0x240)){
        if(original_dialog(root))continue;
        uint32_t id=rd32(root+0x1e4);
        if(ui_index_find(scaled_index,1023u,root,id)<0){
            if(fabsf(rdss(root+0x210)-1280)>.01f||fabsf(rdss(root+0x214)-720)>.01f||scaled_count==512)continue;
            scaled_roots[scaled_count].root=root;scaled_roots[scaled_count].id=id;
            ui_index_add(scaled_index,1023u,root,id,(int)scaled_count);++scaled_count;
        }
        float size[4]={rdss(GVA(0x142980E60)),rdss(GVA(0x142980E64)),0,0};
        if(fabsf(rdss(root+0x210)-size[0])>.01f||fabsf(rdss(root+0x214)-size[1])>.01f)
            call(cpu,UINT64_C(0x140EC4C70),root,size,0,0,0);
    }
    for(unsigned i=0;i<mobile_docked_count;++i){
        uint64_t node=0;float y=0;
        pthread_mutex_lock(&input_mutex);
        // Historical screens may already be freed; only queue changes for this live tree.
        if(mobile_docked[i].seen==ui_pass&&mobile_docked[i].translated&&mobile_docked[i].scale!=applied_scale){
            node=mobile_docked[i].node;y=mobile_docked[i].y;mobile_docked[i].scale=applied_scale;
        }
        pthread_mutex_unlock(&input_mutex);
        if(node){float position[4]={0,y,0,1};call(cpu,UINT64_C(0x140EC3FD0),node,position,0,0,0);}
    }
    // Menus keep updating while the gameplay camera is suspended.
    pthread_mutex_lock(&input_mutex);snapshot_ui();pthread_mutex_unlock(&input_mutex);
    law_photo_actions(cpu);
}

static void dump_ui(FILE *file,uint64_t node,unsigned depth,unsigned *budget) {
    if(!node || depth>40 || !*budget)return;
    --*budget;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
                    fprintf(file,"%llx\t%llx\t%s\t%llx\t%llx\t%u\t%u\t%u\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f",
                        (unsigned long long)node,(unsigned long long)(rd64(node)-g_image_delta),name?name:"",
                        (unsigned long long)rd64(node+0x230),(unsigned long long)rd64(node+0x248),
                        rd32(node+0x1e0),rd32(node+0x138),rd32(node+0x13c),rdss(node+0xc0),rdss(node+0xc4),
                        rdss(node+0xd0),rdss(node+0xd4),rdss(node+0xf0),rdss(node+0xf4),rdss(node+0x100),rdss(node+0x104),rdss(node+0x210),rdss(node+0x214));
    if(rd64(node)==GVA(0x141D796F8)){
        fprintf(file,"\tUTV\t%u\t%u\t%u\t%.6f\t%.6f",rd8(node+0x380),rd8(node+0x383),rd8(node+0x385),rdss(node+0x378),rdss(node+0x37c));
        for(unsigned i=0;i<16;++i)fprintf(file,"\t%.6f",rdss(node+0x50+i*4));
    }
    // Font size, laid-out ink width, auto-size and wrap-limit flags for the layout audit.
    if(rd64(node)==GVA(0x141DED7D8)||rd64(node)==GVA(0x141DAD740))
        fprintf(file,"\tTEXT\t%.3f\t%.3f\t%u\t%u\t%.3f\t%.3f",rdss(node+0x3a0),rdss(node+0x4d0),rd8(node+0x40e),rd8(node+0x384),
            rdss(node+0x3b0),rdss(node+0x3b4));
    if(name&&!strcmp(name,"ACTIVATE_SOCIOTECH_BUTTON")){
        uint64_t object=mobile_button_controller(node,UINT64_C(0x141A5FBB0));
        fprintf(file,"\tLAWSTATE\t%u\t%llx\t%u\t%u\t%llx\t%.6f",rd32(node+0x270),
            (unsigned long long)object,object?rd32(object+0xd0):0,law_photo_camera_count,
            (unsigned long long)law_photo_touch_controller,law_photo_empty_left);
    }
    fprintf(file,"\tid=%u",rd32(node+0x1e4));
    fputc('\n',file);

    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<1024;child=rd64(child+0x240))dump_ui(file,child,depth+1,budget);
}

/* The snapshot walks every UI control of the game (thousands, three cache lines each) and was
 * taken twice per frame (UI pass and camera update): 14 % of the main thread on Adreno 735 in the
 * paused city.  It is taken once per UI pass, and every other pass while nobody touches the screen
 * (1.5 s after the last touch query); at most 100 ms old in any case.  Touch hit tests and the
 * overlay read the last snapshot, so idle frames see the state of one frame earlier at most. */
static unsigned snapshot_due(void){
    static unsigned last_pass=~0u,skipped;static uint64_t last_ns;
    uint64_t now=monotonic_ns();
    unsigned fresh=last_ns&&now-last_ns<100000000u;
    if(fresh&&last_pass==ui_pass)return 0;
    unsigned idle=now-atomic_load_explicit(&touch_activity_ns,memory_order_relaxed)>1500000000u;
    if(fresh&&idle&&!skipped&&ui_pass==last_pass+1){skipped=1;last_pass=ui_pass;return 0;}
    skipped=0;last_pass=ui_pass;last_ns=now;
    return 1;
}

static void snapshot_ui(void) {
    if(!snapshot_due())return;
    uint64_t list=rd64(GVA(0x142A89088));uint32_t count=rd32(GVA(0x142A89090));
    uint64_t screens[32];unsigned used=0,modal=0;
    uint32_t clock_ids[8];unsigned clock_count=0;
    uint32_t switch_ids[5]={0};unsigned switch_present=0;
    uint32_t construction_ids[4]={0};unsigned construction_present=0;
    struct {uint32_t id;float fraction;} notify_buttons[24];unsigned notify_button_count=0;
    notify_icon_count=0;
    uint32_t scroll_ids[128],scroll_flags[128];unsigned scroll_count=0;
    struct {uint32_t id;uint64_t screen;unsigned visible;} editors[128];unsigned editor_count=0;
    uint32_t arrow_ids[256],arrow_rows[256],arrow_sides[256];unsigned arrow_count=0;
    setting_target_count=slider_target_count=0;
    memset(switch_targets,0,sizeof(switch_targets));
    construction_close_target=0;construction_open=0;
    law_photo_touch_reset();
    pause_menu_visible=0;
    rectangle_count=0;time_rectangle_valid=0;time_rectangle=(touch_rect){1,1,0,0};
    if(!list||count>20000){atomic_store(&switch_modal_visible,0);return;}
    FILE *dump=NULL;
#ifdef __ANDROID__
    /* Diagnostic dump request: polled every 64th snapshot (two snapshots per frame) instead of
     * a system-property lookup on every call. */
    static char dumped[PROP_VALUE_MAX];static unsigned dump_poll;char request[PROP_VALUE_MAX]={0};
    if(!(dump_poll++&63u)&&__system_property_get("debug.frostpunk.ui_dump",request)>0&&strcmp(dumped,request)){
        snprintf(dumped,sizeof(dumped),"%s",request);
        char path[4096];const char *profile=getenv("FROSTPUNK_SERVICES_PROFILE");
        if(profile&&snprintf(path,sizeof(path),"%s/mobile-ui.tsv",profile)<(int)sizeof(path))dump=fopen(path,"we");
    }
#endif
    /* One pass over the control list (it was two): the owner screens are collected here, in the
     * order of their first control, which is the order the screen pass below used to find them in.
     * Nothing in this pass writes a field ui_owner_screen reads (+0, +0x230, +0x248).  The list
     * has thousands of controls spread over the heap: the three lines each control is read from
     * are prefetched sixteen controls ahead (most controls take only a few instructions here), so
     * their cache misses overlap. */
    for(unsigned i=0;i<count;++i){
        if(i+16<count){
            uint64_t ahead=rd64(list+(uint64_t)(i+16)*16);
            if(ahead){__builtin_prefetch((const void*)(uintptr_t)ahead);__builtin_prefetch((const void*)(uintptr_t)(ahead+0x1c0));
                __builtin_prefetch((const void*)(uintptr_t)(ahead+0x240));}
        }
        uint64_t control=rd64(list+(uint64_t)i*16);if(!control)continue;
        if(used<32){
            uint64_t screen=ui_owner_screen(control);
            if(screen&&rd64(screen)==GVA(0x141D7B0B0)){
                unsigned j=0;while(j<used&&screens[j]!=screen)++j;
                if(j==used)screens[used++]=screen;
            }
        }
        const char *control_name=(const char*)(uintptr_t)rd64(control+0x1d8);
        int radial=switch_mode&&atomic_load(&switch_menu)?switch_button_index(control_name):-1;
        if(radial>=0&&ui_visible_chain(control)&&(rd32(control+0x1e0)&0x10)){
            switch_ids[radial]=rd32(control+0x1e4);switch_present|=1u<<(unsigned)radial;
        }
        int build=construction_button_index(control_name);
        if(build>=0&&(rd32(control+0x1e0)&0x10)&&ui_shown_chain(control)&&law_photo_ancestor(control,"EXT_CONSTRUCTION_PANEL")){
            construction_ids[build]=rd32(control+0x1e4);construction_present|=1u<<(unsigned)build;
        }
        law_photo_touch_control(control);
        if(control_name&&control_name[0]=='B'&&!strcmp(control_name,"BUTTON")&&notify_button_count<24){
            uint64_t parent=rd64(control+0x230);const char *parent_name=parent?(const char*)(uintptr_t)rd64(parent+0x1d8):NULL;
            unsigned tutorial=parent_name&&!strcmp(parent_name,"TUTORIAL_NOTIFICATION");
            if(parent_name&&(tutorial||!strcmp(parent_name,"NOTIFICATION"))&&ui_visible_chain(control)
               &&law_photo_ancestor(control,"EXT_NOTIFICATION_PANEL")){
                float width=rdss(control+0x100),icon=tutorial?56.f:66.f;
                notify_buttons[notify_button_count].id=rd32(control+0x1e4);
                notify_buttons[notify_button_count++].fraction=width>icon?icon/width:1.f;
            }
        }
        if(rd64(control)==GVA(0x141DAC248)&&editor_count<128){
            editors[editor_count].id=rd32(control+0x1e4);editors[editor_count].screen=ui_owner_screen(control);
            editors[editor_count++].visible=ui_visible_chain(control);
        }
        if(control_name&&control_name[0]=='E'&&!strncmp(control_name,"EXT_INGAME_MENU_",16)){
            uint64_t ancestor=control;unsigned depth=0;
            for(;ancestor&&depth<40;++depth,ancestor=rd64(ancestor+0x230))
                if(rd32(ancestor+0x1e0)&2)break;
            if(!ancestor)pause_menu_visible=1;
        }
        if(control_name&&control_name[0]=='A'&&(!strcmp(control_name,"ARROW_LEFT")||!strcmp(control_name,"ARROW_RIGHT"))&&arrow_count<256&&ui_visible_chain(control)){
            uint64_t slider=rd64(control+0x230),row=slider?rd64(slider+0x230):0;
            const char *name=row?(const char*)(uintptr_t)rd64(row+0x1d8):NULL;
            if(name&&(!strcmp(name,"SETTINGS_SELECTOR")||!strcmp(name,"SETTINGS_SELECTOR_ADDITIONAL")
               ||!strcmp(name,"SETTINGS_LANGUAGE_SELECTOR")||!strcmp(name,"SCENARIO_SETTING_SELECTOR"))){
                unsigned r=0;for(;r<setting_target_count;++r)if(setting_targets[r].id==rd32(row+0x1e4))break;
                if(r<128){
                    if(r==setting_target_count){memset(&setting_targets[r],0,sizeof(setting_targets[r]));
                        setting_targets[r].id=rd32(row+0x1e4);setting_targets[r].bounds=(touch_rect){1,1,0,0,8192};
                        unsigned count=0,index=0;
                        if(!strcmp(name,"SCENARIO_SETTING_SELECTOR")){
                            if(rd8(row+0x2d0)){count=rd32(row+0x2a8);index=rd32(row+0x2d4);}
                        }else{uint64_t model=rd64(row+0x200);if(model){count=rd32(model+0x30);index=rd32(model+0x40);}}
                        /* Native taps wrap cyclic selectors; swipes stop at the first and last option. */
                        if(count>0&&count<=4096&&index<count)setting_targets[r].swipe_enabled=(index>0?1u:0u)|(index+1<count?2u:0u);
                        /* The value strip stays swipeable when a limit hides either arrow. */
                        float x,y,sx,sy;parent_transform(slider,&x,&y,&sx,&sy);
                        float a=rdss(slider+0xf0),b=rdss(slider+0xf4),width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
                        if(width>0&&height>0&&a>0&&b>0&&rdss(slider+0x100)>0&&rdss(slider+0x104)>0){
                            float left=x+sx*(rdss(slider+0xc0)+rdss(slider+0xd0)*a),top=y+sy*(rdss(slider+0xc4)+rdss(slider+0xd4)*b);
                            setting_targets[r].bounds=(touch_rect){left/width,top/height,(left+rdss(slider+0x100)*sx*a)/width,
                                (top+rdss(slider+0x104)*sy*b)/height,8192};
                        }
                        ++setting_target_count;}
                    arrow_ids[arrow_count]=rd32(control+0x1e4);arrow_rows[arrow_count]=r;
                    arrow_sides[arrow_count++]=!strcmp(control_name,"ARROW_RIGHT");
                }
            }
        }
        if(control_name&&!strcmp(control_name,"DRAG_ELEMENT")&&slider_target_count<64&&ui_visible_chain(control)){
            uint64_t row=control;unsigned difficulty=0;
            for(unsigned depth=0;row&&depth<6;++depth,row=rd64(row+0x230)){
                const char *name=(const char*)(uintptr_t)rd64(row+0x1d8);
                if(name&&(!strcmp(name,"DIFFICULTY_SLIDER")||!strcmp(name,"MAIN_DIFFICULTY_SLIDER"))){difficulty=1;break;}
            }
            if(difficulty){
                unsigned n=slider_target_count++;slider_targets[n].id=rd32(control+0x1e4);
                slider_targets[n].enabled=0;slider_targets[n].bounds=(touch_rect){1,1,0,0};
                float x,y,sx,sy;parent_transform(row,&x,&y,&sx,&sy);
                float a=rdss(row+0xf0),b=rdss(row+0xf4),width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
                if(width>0&&height>0&&a>0&&b>0){
                    float left=x+sx*(rdss(row+0xc0)+rdss(row+0xd0)*a),top=y+sy*(rdss(row+0xc4)+rdss(row+0xd4)*b);
                    slider_targets[n].bounds=(touch_rect){left/width,top/height,(left+rdss(row+0x100)*sx*a)/width,(top+rdss(row+0x104)*sy*b)/height,64};
                }
            }
        }
        if(rd64(control)==GVA(0x141DA74C8)&&scroll_count<128&&ui_visible_chain(control)){
            unsigned building_list=control_name&&!strcmp(control_name,"BUILDING_SCROLL_PANE")&&construction_widget(control)==1;
            if(building_list||(control_name&&(!strcmp(control_name,"SETTINGS_SCROLL_PANE")||!strcmp(control_name,"CONTROLS_SCROLL_PANE"))))wr32(control+0x370,1);
            if(applied_scale>100&&control_name&&(!strcmp(control_name,"TECHTREE")||!strcmp(control_name,"GOAL_ENTRIES_SCROLL_PANE"))
               &&law_photo_ancestor(control,"EXT_TECHNOLOGY_PANEL")&&rd32(control+0x370)==1&&rectangle_count<512){
                float x,y,sx,sy;parent_transform(control,&x,&y,&sx,&sy);
                float a=rdss(control+0xf0),b=rdss(control+0xf4),w=rdss(GVA(0x142980E60)),h=rdss(GVA(0x142980E64));
                float left=x+sx*(rdss(control+0xc0)+rdss(control+0xd0)*a),top=y+sy*(rdss(control+0xc4)+rdss(control+0xd4)*b);
                if(w>0&&h>0&&sx*a>0&&sy*b>0)
                    rectangles[rectangle_count++]=(touch_rect){left/w,top/h,(left+rdss(control+0x100)*sx*a)/w,(top+rdss(control+0x104)*sy*b)/h,4096};
            }
            scroll_ids[scroll_count]=rd32(control+0x1e4);
            scroll_flags[scroll_count++]=building_list?(16384|4096|32):control_name&&(!strcmp(control_name,"SETTINGS_SCROLL_PANE")||!strcmp(control_name,"CONTROLS_SCROLL_PANE"))
                ?(16384|4096):rd32(control+0x370)==1?4096:0;
        }
        if(clock_count==8)continue;
        const char *name=control_name;if(!name||(name[0]!='B'&&name[0]!='T'))continue;
        if(strcmp(name,"BUTTON_PAUSE")&&strcmp(name,"BUTTON_NORMAL")&&strcmp(name,"BUTTON_FAST")&&strcmp(name,"BUTTON_VERYFAST")
           &&strcmp(name,"TEXT_TIME")&&strcmp(name,"TEXT_WORKTIME"))continue;
        uint64_t parent=rd64(control+0x230);
        for(unsigned depth=0;parent&&depth<6;++depth,parent=rd64(parent+0x230)){
            const char *parent_name=(const char*)(uintptr_t)rd64(parent+0x1d8);
            if(parent_name&&!strcmp(parent_name,"EXT_TIME_PANEL")){clock_ids[clock_count++]=rd32(control+0x1e4);break;}
        }
    }
    for(unsigned s=0;s<used;++s){
        uint64_t screen=screens[s];
        if(switch_mode&&!modal){unsigned budget=1024;modal=switch_modal_scan(screen,0,&budget);}
        if(!loading_hint||rd64(loading_hint)!=GVA(0x141EE06E0)){
            unsigned seen=0;
            for(uint64_t root=rd64(screen+0x220);root&&seen++<64&&!loading_hint;root=rd64(root+0x240)){
                unsigned inner=0;
                for(uint64_t child=rd64(root+0x220);child&&inner++<256;child=rd64(child+0x240))
                    if(rd64(child)==GVA(0x141EE06E0)){loading_hint=child;loading_hint_id=rd32(child+0x1e4);break;}
            }
        }
        if(dump){unsigned budget=50000;dump_ui(dump,screen,0,&budget);}
        uint64_t rects=rd64(screen+0x2f0);uint32_t n=rd32(screen+0x2f8);
        if(!rects||n>20000)continue;
        if(dump)for(unsigned k=0;k<n;++k){uint64_t at=rects+(uint64_t)k*0x50;float l=1,t=1,r=0,b=0;
            for(unsigned v=0;v<4;++v){float x=(rdss(at+v*16)+1)*.5f,y=(1-rdss(at+v*16+4))*.5f;l=fminf(l,x);r=fmaxf(r,x);t=fminf(t,y);b=fmaxf(b,y);}
            fprintf(dump,"HIT\t%llx\t%u\t%x\t%.5f\t%.5f\t%.5f\t%.5f\n",(unsigned long long)screen,rd32(at+0x40),rd32(at+0x44),l,t,r,b);}
        for(unsigned k=0;k<n;++k){
            uint64_t at=rects+(uint64_t)k*0x50;unsigned clock=0,scroll=0;
            for(unsigned n=0;n<clock_count;++n)if(clock_ids[n]==rd32(at+0x40)){clock=1;break;}
            unsigned touch_flags=0,hidden_editor=0,visible_editor=0;
            for(unsigned n=0;n<editor_count;++n)if(editors[n].screen==screen&&editors[n].id==rd32(at+0x40)){
                if(editors[n].visible)visible_editor=1;else hidden_editor=1;
            }
            if(hidden_editor&&!visible_editor)continue;
            if(visible_editor)touch_flags|=32768;
            for(unsigned n=0;n<scroll_count;++n)if(scroll_ids[n]==rd32(at+0x40)){scroll=1;touch_flags=scroll_flags[n];break;}
            uint32_t flags=rd32(at+0x44);
            touch_rect r={1,1,0,0};
            r.flags=touch_flags;
            for(unsigned v=0;v<4;++v){float x=(rdss(at+v*16)+1)*.5f,y=(1-rdss(at+v*16+4))*.5f;
                r.left=fminf(r.left,x);r.right=fmaxf(r.right,x);r.top=fminf(r.top,y);r.bottom=fmaxf(r.bottom,y);}
            if(!(flags&2)&&(flags&0x10))for(unsigned index=0;index<5;++index)
                if((switch_present&(1u<<index))&&switch_ids[index]==rd32(at+0x40)){
                    float x=(r.left+r.right)*.5f,y=(r.top+r.bottom)*.5f;
                    if(isfinite(x)&&isfinite(y)&&x>=0&&x<=1&&y>=0&&y<=1)switch_targets[index]=touch_point(x,y);
                }
            if(!(flags&2)&&(flags&0x10)&&(construction_present&8u)&&construction_ids[3]==rd32(at+0x40)){
                float x=(r.left+r.right)*.5f,y=(r.top+r.bottom)*.5f;
                if(isfinite(x)&&isfinite(y)&&x>=0&&x<=1&&y>=0&&y<=1)construction_close_target=touch_point(x,y);
            }
            for(unsigned n=0;n<arrow_count;++n)if(arrow_ids[n]==rd32(at+0x40)&&!(flags&2)){
                unsigned row=arrow_rows[n],side=arrow_sides[n];touch_rect *bounds=&setting_targets[row].bounds;
                bounds->left=fminf(bounds->left,r.left);bounds->right=fmaxf(bounds->right,r.right);
                bounds->top=fminf(bounds->top,r.top);bounds->bottom=fmaxf(bounds->bottom,r.bottom);
                setting_targets[row].x[side]=(r.left+r.right)*.5f;setting_targets[row].y[side]=(r.top+r.bottom)*.5f;
                float px=fmaxf((r.right-r.left)*.75f,12.f/1280),py=fmaxf((r.bottom-r.top)*.75f,8.f/720);
                setting_targets[row].hit[side]=(touch_rect){r.left-px,r.top-py,r.right+px,r.bottom+py,8192};
                bounds->left=fminf(bounds->left,r.left-px);bounds->right=fmaxf(bounds->right,r.right+px);
                bounds->top=fminf(bounds->top,r.top-py);bounds->bottom=fmaxf(bounds->bottom,r.bottom+py);
                if(flags&0x10)setting_targets[row].enabled|=1u<<side;
                break;
            }
            for(unsigned n=0;n<slider_target_count;++n)if(slider_targets[n].id==rd32(at+0x40)&&!(flags&2)&&(flags&0x10)){
                slider_targets[n].x=(r.left+r.right)*.5f;slider_targets[n].y=(r.top+r.bottom)*.5f;slider_targets[n].enabled=1;
                slider_targets[n].bounds.left=fminf(slider_targets[n].bounds.left,r.left);
                slider_targets[n].bounds.right=fmaxf(slider_targets[n].bounds.right,r.right);
                slider_targets[n].bounds.top=fminf(slider_targets[n].bounds.top,r.top);
                slider_targets[n].bounds.bottom=fmaxf(slider_targets[n].bounds.bottom,r.bottom);break;
            }
            if((flags&2)||(!(flags&0x10)&&!clock&&!scroll))continue;
            for(unsigned n=0;n<notify_button_count&&notify_icon_count<24;++n)if(notify_buttons[n].id==rd32(at+0x40)){
                touch_rect icon=r;icon.right=r.left+(r.right-r.left)*notify_buttons[n].fraction;
                notify_icons[notify_icon_count++]=icon;break;
            }
            if(rectangle_count<512&&(scroll||(r.right-r.left<.95f&&r.bottom-r.top<.95f)))rectangles[rectangle_count++]=r;
            if(clock){time_rectangle.left=fminf(time_rectangle.left,r.left);time_rectangle.right=fmaxf(time_rectangle.right,r.right);
                time_rectangle.top=fminf(time_rectangle.top,r.top);time_rectangle.bottom=fmaxf(time_rectangle.bottom,r.bottom);time_rectangle_valid=1;}
        }
    }
    atomic_store(&switch_modal_visible,modal);
    if(modal){atomic_store(&switch_menu,0);atomic_store(&switch_available,0);}
    construction_open=(construction_present&7u)!=0;
    law_photo_touch_finish();
    if(dump){
        fprintf(dump,"SNAPSHOT\t%u\t%u\t%u\t%u\n",count,used,rectangle_count,arrow_count);
        fprintf(dump,"LAW_TOUCH\t%llx\t%llx\t%u\t%.6f\t%u\t%u\n",
            (unsigned long long)law_photo_touch_controller,(unsigned long long)law_photo_touch_view,law_photo_touch_id,
            law_photo_empty_left,law_photo_touch_button_count,law_photo_camera_count);
        for(unsigned i=0;i<setting_target_count;++i){touch_rect r=setting_targets[i].bounds;
            fprintf(dump,"SWIPE\t%u\t%u\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\t%.5f\n",setting_targets[i].id,
                setting_targets[i].enabled,r.left,r.top,r.right,r.bottom,setting_targets[i].x[0],setting_targets[i].y[0],setting_targets[i].x[1],setting_targets[i].y[1]);
        }
        fclose(dump);
    }
    if(time_rectangle_valid){float padding=(time_rectangle.bottom-time_rectangle.top)*.5f;
        time_rectangle.left-=padding;time_rectangle.right+=padding;time_rectangle.top-=padding;time_rectangle.bottom+=padding;}
}

static void rotate_building(CPU *cpu,uint64_t system,float angle) {
    (void)system;
    uint64_t painter=painters?rd64(painters+0x80):0;
    if(!painter||rd64(painter)!=GVA(0x1421381E0)){building_gesture=0;return;}
    building_gesture+=angle;
    const float step=.35f;
    while(fabsf(building_gesture)>=step){
        unsigned turns=building_gesture>0?1:3;
        for(unsigned i=0;i<turns;++i)call_value(cpu,UINT64_C(0x141AAA610),painter,0x23);
        building_gesture+=building_gesture>0?-step:step;
    }
}

/* World-map gate for pinch zoom.  FrostpunkGameplayCameraSystem::OnMouseWheel (0x141251480) steps one
 * zoom level per frame; a step out of the last non-distant level (+0x1F4 = current level index of
 * CameraEntry +0x58/+0x60, FrostpunkCameraZoomLevelEntry +0x38 = Distant) enters the world map when
 * +0x21C (distant view available) is set.  The game thread caches the first distant level here;
 * frostpunk_mobile_context reports "the next zoom-out opens the map" from the live level index. */
static _Atomic uint64_t gate_system;
static _Atomic int gate_first_distant=-1;
static uint64_t gate_entry;static uint32_t gate_count;
static uint64_t call_result(CPU *cpu,uint64_t function,uint64_t object){
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=object;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,function);return c.rax;
}
static void map_gate_update(CPU *cpu,uint64_t system){
    uint64_t entry=rd64(system+0xb8);uint32_t count=entry?rd32(entry+0x60):0;
    if(entry!=gate_entry||count!=gate_count){
        gate_entry=entry;gate_count=count;int first=-1;uint64_t links=entry?rd64(entry+0x58):0;
        for(uint32_t i=0;links&&i<count&&i<64;++i){
            uint64_t level=call_result(cpu,UINT64_C(0x14124C6A0),links+16ull*i);
            if(level&&rd8(level+0x38)){first=(int)i;break;}
        }
        atomic_store(&gate_first_distant,first);
        fprintf(stderr,"FP_MAP_GATE levels=%u first_distant=%d\n",count,first);
    }
    {static int logged_level=-2;static unsigned logged_distant=2;int level=(int32_t)rd32(system+0x1F4);unsigned distant=rd8(system+0x21C);
     if(level!=logged_level||distant!=logged_distant){logged_level=level;logged_distant=distant;
        fprintf(stderr,"FP_MAP_LEVEL index=%d distant_available=%u\n",level,distant);}}
    atomic_store(&gate_system,system);
}
static unsigned map_gate_next_opens(void){
    uint64_t system=atomic_load(&gate_system);int first=atomic_load(&gate_first_distant);
    if(!system||first<=0||!rd8(system+0x21C))return 0;
    return (int32_t)rd32(system+0x1F4)==first-1;
}


/* Map limits of the touch camera.  FrostpunkGameplayCameraSystem::Update (141250610) keeps the free
 * RTS camera (controller +0xA0 == 4) inside FrostpunkCameraBoundsBase: system +0x1E0 for city zoom
 * levels, +0x1E8 for distant ones (level entry +0x38), virtual +0xE0 (bounds, out, &target) returns
 * the clamped target; the camera position moves by the same offset (SetPosition 1410B93E0,
 * SetTarget 141C365B0).  The touch pan copies the desired transform to the current one at once, so
 * it must clamp before that copy or the city drifts off the map. */
static void camera_bounds_clamp(CPU *cpu,uint64_t system,uint64_t controller){
    if(rd32(controller+0xa0)!=4)return;
    int32_t level=(int32_t)rd32(system+0x1F4);
    uint64_t list=rd64(system+0xb8);uint32_t count=list?rd32(list+0x60):0;
    if(level<0||(uint32_t)level>=count||count>64)return;
    uint64_t links=rd64(list+0x58);
    uint64_t entry=links?call_result(cpu,UINT64_C(0x14124C6A0),links+16ull*(uint64_t)level):0;
    if(!entry)return;
    uint64_t bounds=rd64(system+(rd8(entry+0x38)?0x1E8:0x1E0));
    if(!bounds||!rd64(bounds))return;
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);
    uint64_t out=c.rsp+0x100,target=c.rsp+0x120;
    memcpy((void*)(uintptr_t)target,(const void*)(uintptr_t)(controller+0x170),16);
    c.rcx=bounds;c.rdx=out;c.r8=target;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,rd64(rd64(bounds)+0xE0)-g_image_delta);
    if(!c.rax)return;
    float clamped[4],before[4],position[4];
    memcpy(clamped,(const void*)(uintptr_t)c.rax,16);memcpy(before,(const void*)(uintptr_t)target,16);
    if(!memcmp(clamped,before,16))return;
    for(unsigned i=0;i<4;++i)if(!isfinite(clamped[i]))return;
    memcpy(position,(const void*)(uintptr_t)(controller+0x60),16);
    for(unsigned i=0;i<4;++i)position[i]+=clamped[i]-before[i];
    call(cpu,UINT64_C(0x1410B93E0),controller,position,0,0,0);
    call(cpu,UINT64_C(0x141C365B0),controller,clamped,0,0,0);
}

/* Zoom level entries (system +0xB8: links +0x58, count +0x60; 14124C6A0 resolves one): +0x1C camera
 * distance, +0x24 pitch, +0x44 focus height, +0x38 distant (world map) level. */
static uint64_t zoom_level_entry(CPU *cpu,uint64_t system,int index){
    uint64_t list=rd64(system+0xb8);uint32_t count=list?rd32(list+0x60):0;uint64_t links=list?rd64(list+0x58):0;
    if(!links||index<0||(uint32_t)index>=count||count>64)return 0;
    return call_result(cpu,UINT64_C(0x14124C6A0),links+16ull*(uint64_t)index);
}
static unsigned zoom_active,zoom_settling,zoom_quiet;static float zoom_z;
static void zoom_apply(CPU *cpu,uint64_t system,uint64_t controller,float delta,unsigned settle,int city){
    int index=(int32_t)rd32(system+0x1F4);unsigned mode=rd32(controller+0xa0);
    if(city<1||index<0||index>=city||(mode!=0&&mode!=4)){
        if(zoom_active)fprintf(stderr,"FP_ZOOM abort level=%d city=%d mode=%u\n",index,city,mode);
        zoom_active=zoom_settling=0;zoom_limit_out=0;return;
    }
    /* A gesture whose end was not reported settles after half a second without zoom. */
    zoom_quiet=delta!=0?0:zoom_quiet+1;
    if(zoom_active&&!zoom_settling&&zoom_quiet>15)settle=1;
    if(delta!=0){
        if(!zoom_active){zoom_active=1;zoom_z=(float)index;
            fprintf(stderr,"FP_ZOOM start level=%d city=%d mode=%u speed=%.3f dt=%.5f\n",index,city,mode,rdss(controller+0x9c),rd64(GVA(0x142898D88))?rdss(rd64(GVA(0x142898D88))+248):-1.f);}
        zoom_settling=0;
        /* A doubled finger distance (log 0.69) crosses about 2.8 levels. */
        float z=zoom_z-delta*4.f,top=(float)(city-1);
        if(delta<0&&z>top+.25f)zoom_limit_out=1;
        zoom_z=fminf(top,fmaxf(0,z));
    }
    if(!zoom_active)return;
    if(settle&&!zoom_settling){
        int nearest=(int)lroundf(zoom_z);zoom_limit_out=0;
        fprintf(stderr,"FP_ZOOM end z=%.3f level=%d->%d\n",zoom_z,index,nearest);
        if(nearest!=index){
            /* SetZoomLevel (1412537F0): the game's own transition from here to the level, with its
             * level of detail, sound and bounds; the next touch frames wait for it (+0x228 > 0). */
            CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=system;c.rdx=(uint64_t)(uint32_t)nearest;c.r8=1;
            push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x1412537F0));
            zoom_active=0;return;
        }
        zoom_settling=1;
    }
    if(zoom_settling){
        float target=(float)index;zoom_z+=(target-zoom_z)*.3f;
        if(fabsf(target-zoom_z)<.005f){zoom_z=target;zoom_active=zoom_settling=0;}
    }
    int lower=(int)floorf(zoom_z);if(lower>city-1)lower=city-1;if(lower<0)lower=0;
    float f=fminf(1,fmaxf(0,zoom_z-(float)lower));
    uint64_t a=zoom_level_entry(cpu,system,lower),b=zoom_level_entry(cpu,system,lower+1<city?lower+1:lower);
    if(!a||!b)return;
    float distance=rdss(a+0x1C)+(rdss(b+0x1C)-rdss(a+0x1C))*f;
    float pitch=rdss(a+0x24)+(rdss(b+0x24)-rdss(a+0x24))*f;
    float height=rdss(a+0x44)+(rdss(b+0x44)-rdss(a+0x44))*f;
    if(!isfinite(distance)||!isfinite(pitch)||!isfinite(height)||distance<=0)return;
    /* Heading of the current rotation (14124D730), then 14124D360(system, out, &focus, heading,
     * pitch, distance, keep the focus height): the eye for that focus. */
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=system;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x14124D730));
    float heading=c.xmm[0].f32[0];if(!isfinite(heading))return;
    c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);
    uint64_t out=c.rsp+0x100,target=c.rsp+0x120;
    /* The current focus (+0x160): the target (+0x170) of an idle controller is the last move's. */
    float focus[4];memcpy(focus,(const void*)(uintptr_t)(controller+0x160),16);focus[1]=height;focus[3]=1;
    memcpy((void*)(uintptr_t)target,focus,16);
    wrss(c.rsp+0x20,pitch);wrss(c.rsp+0x28,distance);wr64(c.rsp+0x30,1);
    c.rcx=system;c.rdx=out;c.r8=target;c.xmm[3].f32[0]=heading;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x14124D360));
    float eye[4];memcpy(eye,(const void*)(uintptr_t)out,16);
    for(unsigned i=0;i<3;++i)if(!isfinite(eye[i]))return;
    memcpy((void*)(uintptr_t)(controller+0x60),eye,16);memcpy((void*)(uintptr_t)(controller+0x170),focus,16);
    camera_bounds_clamp(cpu,system,controller);
    /* Follow at 40 % a frame (the touch pan moves the camera at once); LookAt 141C361A0 turns the
     * camera to the new pitch. */
    float current[4],look[4];
    memcpy(eye,(const void*)(uintptr_t)(controller+0x60),16);memcpy(focus,(const void*)(uintptr_t)(controller+0x170),16);
    memcpy(current,(const void*)(uintptr_t)(controller+0x30),16);memcpy(look,(const void*)(uintptr_t)(controller+0x160),16);
    for(unsigned i=0;i<3;++i){current[i]+=(eye[i]-current[i])*.4f;look[i]+=(focus[i]-look[i])*.4f;}
    if(!zoom_active){memcpy(current,eye,12);memcpy(look,focus,12);}
    memcpy((void*)(uintptr_t)(controller+0x30),current,16);
    call(cpu,UINT64_C(0x141C361A0),controller,look,0,0,0);
}

void fp_mobile_camera_before(CPU *cpu) {
    uint64_t system=cpu->rcx,controller=rd64(system+0xa0);
    map_gate_update(cpu,system);
    float values[4],building_turn;int speed;unsigned reset,available=controller&&!rd32(system+0x1f0)&&rdss(system+0x228)<=0&&!rd64(system+0x230);
    pthread_mutex_lock(&input_mutex);
    camera_available=available;
    uint64_t placement=rd64(GVA(0x143FF63E8));
    building_available=placement&&rd64(placement)==GVA(0x142014120)&&rd32(placement+0x2068)>0;
    uint64_t painter=painters?rd64(painters+0x80):0;
    road_available=painter&&(rd64(painter)==GVA(0x142138308)||rd64(painter)==GVA(0x1421383A8));
    {   /* FrostpunkBuildingPainter, BuildStreet, BuildPipeStreet, Demolish, DemolishStreet painters. */
        uint64_t type=painter?rd64(painter):0;
        unsigned kind=type==GVA(0x1421381E0)?1:type==GVA(0x142138308)?2:type==GVA(0x1421383A8)?3:type==GVA(0x142138448)?4:type==GVA(0x1421384D0)?5:0;
        if(kind==1&&!building_available)kind=0;
        if(kind!=painter_kind){
            street_detail_units=0;
            fprintf(stderr,"FP_CONSTRUCTION painter=%u open=%u\n",kind,construction_open);
        }
        painter_kind=kind;
    }
    if(!building_available)building_gesture=0;
    snapshot_ui();memcpy(values,pending,sizeof(values));memset(pending,0,sizeof(pending));
    reset=reset_pending;reset_pending=0;
    building_turn=pending_building_turn;pending_building_turn=0;
    speed=pending_speed;pending_speed=-1;
    unsigned clear=painter_clear_pending;painter_clear_pending=0;
    unsigned street_tool=street_tool_pending;street_tool_pending=0;
    float zoom_delta=pending_zoom;pending_zoom=0;unsigned zoom_settle=zoom_settle_pending;zoom_settle_pending=0;
    int zoom_city;{int first=atomic_load(&gate_first_distant);uint64_t list=rd64(system+0xb8);
        zoom_city=first>0?first:list?(int)rd32(list+0x60):0;
        int level=(int32_t)rd32(system+0x1F4);
        unsigned was_ok=zoom_city_ok;
        zoom_city_ok=available&&level>=0&&level<zoom_city&&(rd32(controller+0xa0)==0||rd32(controller+0xa0)==4);
        /* Why pinches take the wheel (which the game refuses over its UI, while following an entity
         * or while the camera is locked): logged on the change, at most once a second. */
        static uint64_t refused_logged;uint64_t now=monotonic_ns();
        if(was_ok&&!zoom_city_ok&&now-refused_logged>1000000000u){refused_logged=now;
            fprintf(stderr,"FP_ZOOM refused level=%d city=%d mode=%u locked=%u transition=%.2f follow=%u\n",level,zoom_city,
                controller?rd32(controller+0xa0):~0u,rd32(system+0x1f0),rdss(system+0x228),rd64(system+0x230)!=0);}}
    uint64_t time_system=rd64(GVA(0x142B8D800)),clock=rd64(GVA(0x142B8B390));
    time_state_valid=time_system&&rd64(time_system)==GVA(0x141E049B0)&&clock;
    if(time_state_valid){current_speed=0;
        if(!rd32(clock+0xc0)){float difference=INFINITY,value=rdss(clock+0xd0);
            for(unsigned i=0;i<3;++i){float d=fabsf(value-rdss(GVA(0x142B93B74)+i*4));if(d<difference){difference=d;current_speed=i+1;}}}}
    pthread_mutex_unlock(&input_mutex);
    if(time_state_valid&&speed>=0){
        if(speed>0)call_value(cpu,UINT64_C(0x141107DE0),time_system,(unsigned)speed-1);
        call_value(cpu,UINT64_C(0x141107E40),time_system,speed==0);
    }
    if(building_available&&building_turn!=0)rotate_building(cpu,placement,building_turn);
    if(clear){
        uint64_t input=rd64(GVA(0x143FE8C40));unsigned state=input?rd32(input+0xA8):~0u;
        if(state==2&&painters&&rd64(painters+0x80)){
            call_result(cpu,UINT64_C(0x14150CC00),input);
            fprintf(stderr,"FP_CONSTRUCTION tool cancelled state=%u painter=%u\n",rd32(input+0xA8),rd64(painters+0x80)!=0);
        } else fprintf(stderr,"FP_CONSTRUCTION cancel skipped state=%u\n",state);
    }
    if(street_tool&&available&&atomic_load(&touch_construction)&&(construction_open||painter_kind)){
        uint64_t input=rd64(GVA(0x143FE8C40));
        unsigned state=input?rd32(input+0xa8):~0u;
        if(painters&&input&&rd64(input+0xa0)==painters&&(state==0||state==2||state==3)){
            /* The construction panel uses this same entry for tool 2 (build) and 4 (demolish).
             * It switches painters in place when input is already in paint mode. */
            CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);c.rcx=input;c.rdx=street_tool;c.r8=GVA(0x14296F8F8);
            push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x141864090));
        }
    }
    if(!available)return;
    float position[4],target[4];memcpy(position,(void*)(uintptr_t)(controller+0x30),16);memcpy(target,(void*)(uintptr_t)(controller+0x160),16);
    float distance=sqrtf((position[0]-target[0])*(position[0]-target[0])+(position[1]-target[1])*(position[1]-target[1])+(position[2]-target[2])*(position[2]-target[2]));
    if(!isfinite(distance)||distance<1)return;
    if(saved_controller!=controller){
        saved_controller=controller;memcpy(initial_position,position,16);memcpy(initial_target,target,16);
        memcpy(initial_rotation,(void*)(uintptr_t)(controller+0x20),16);initial_distance=distance;
        fprintf(stderr,"FP_TOUCH_CAMERA controller=%llx distance=%.3f direct=1\n",(unsigned long long)controller,distance);
    }
    if(reset){
        memcpy((void*)(uintptr_t)(controller+0x20),initial_rotation,16);memcpy((void*)(uintptr_t)(controller+0x130),initial_rotation,16);
        memcpy((void*)(uintptr_t)(controller+0x30),initial_position,16);memcpy((void*)(uintptr_t)(controller+0x60),initial_position,16);
        memcpy((void*)(uintptr_t)(controller+0x160),initial_target,16);memcpy((void*)(uintptr_t)(controller+0x170),initial_target,16);
        memset((void*)(uintptr_t)(controller+0x70),0,16);memset((void*)(uintptr_t)(controller+0x180),0,16);
        wr32(controller+0xa0,0);wrss(controller+0x1f8,initial_distance);zoom_active=zoom_settling=0;
        aim_offset[0]=aim_offset[1]=0;return;
    }
    float length=hypotf(values[0],values[1]);
    unsigned aiming=atomic_load(&touch_construction)&&(construction_open||painter_kind);
    if(!aiming)aim_offset[0]=aim_offset[1]=0;
    if(length>0){
        float vector[4]={-values[0],0,values[1],0};
        /* A construction pan starts where the aim points: the part of earlier pans that the camera
         * limits cut off comes back first (141C365D0 moves from the current position and target). */
        if(aim_offset[0]!=0||aim_offset[1]!=0)
            for(unsigned k=0;k<2;++k){uint64_t at=controller+(k?0x160:0x30);
                wrss(at,rdss(at)+aim_offset[0]);wrss(at+8,rdss(at+8)+aim_offset[1]);}
        call(cpu,UINT64_C(0x141C365D0),controller,vector,0,distance*length*1.4f,1);
        float wanted[4];memcpy(wanted,(void*)(uintptr_t)(controller+0x170),16);
        camera_bounds_clamp(cpu,system,controller);
        if(aiming){
            float kept[4];memcpy(kept,(void*)(uintptr_t)(controller+0x170),16);
            aim_offset[0]=wanted[0]-kept[0];aim_offset[1]=wanted[2]-kept[2];
            if(!isfinite(aim_offset[0])||!isfinite(aim_offset[1]))aim_offset[0]=aim_offset[1]=0;
        }
        memcpy(position,(void*)(uintptr_t)(controller+0x60),16);
        memcpy(target,(void*)(uintptr_t)(controller+0x170),16);
        memcpy((void*)(uintptr_t)(controller+0x30),position,16);
        memcpy((void*)(uintptr_t)(controller+0x160),target,16);
    }
    if(values[3]!=0){
        float angle=-values[3],x=position[0]-target[0],z=position[2]-target[2];
        unsigned mode=rd32(controller+0xa0);
        call(cpu,UINT64_C(0x141C36360),controller,NULL,angle*(180.f/3.141592654f),1000,1);
        position[0]=target[0]+cosf(angle)*x+sinf(angle)*z;
        position[2]=target[2]-sinf(angle)*x+cosf(angle)*z;
        memcpy((void*)(uintptr_t)(controller+0x20),(void*)(uintptr_t)(controller+0x130),16);
        memcpy((void*)(uintptr_t)(controller+0x30),position,16);
        memcpy((void*)(uintptr_t)(controller+0x60),position,16);
        /* The turn is applied here at once.  141C36360 also put the controller into its orbit mode 6,
         * and zoom_apply below refuses any mode but 0 and 4, so a pinch lost its zoom in every frame
         * that also carried a turn.  Real fingers turn a little in nearly every touch event; at a low
         * frame rate every frame collects several events, so pinch zoom stopped working once the
         * city grew and the phone heated up.  The orbit would only repeat the placement made here
         * (and snap to the distance it stored before a zoom), so the free or idle mode stays. */
        if(mode==0||mode==4)wr32(controller+0xa0,mode);
    }
    if(zoom_delta!=0||zoom_active)zoom_apply(cpu,system,controller,zoom_delta,zoom_settle,zoom_city);
}

__attribute__((visibility("default"))) unsigned frostpunk_mobile_construction_view(float corners[8],uint16_t *detail,unsigned capacity,unsigned painter){
    pthread_mutex_lock(&input_mutex);
    unsigned ready=atomic_load(&construction_projected);
    unsigned units=street_detail_painter==painter&&street_detail_units<=capacity?street_detail_units:0;
    if(ready)memcpy(corners,construction_plane,sizeof(construction_plane));
    if(units)memcpy(detail,street_detail,units*sizeof(*detail));
    pthread_mutex_unlock(&input_mutex);
    return units*2+ready;
}

static void project_vector(uint64_t matrix,const float point[4],float projected[4]){
    for(unsigned i=0;i<4;++i)
        projected[i]=point[0]*rdss(matrix+4*i)+point[1]*rdss(matrix+16+4*i)
            +point[2]*rdss(matrix+32+4*i)+point[3]*rdss(matrix+48+4*i);
}

static void publish_construction_plane(uint64_t camera,unsigned alternate,const float ground[4]){
    /* Y=0 is the painters' build plane. The forward camera is the inverse of the aim-ray transform. */
    static const float offsets[4][2]={{-7,-7},{7,-7},{7,7},{-7,7}};
    float corners[8];
    for(unsigned i=0;i<4;++i){
        float point[4]={ground[0]+offsets[i][0],0,ground[2]+offsets[i][1],1};
        float clip[4],view[4];
        if(alternate){
            project_vector(camera+0x50,point,view);
            project_vector(camera+0x110,view,clip);
        }else project_vector(camera+0x1f0,point,clip);
        if(!(clip[3]>0)){atomic_store(&construction_projected,0);return;}
        float x=.5f+.5f*(clip[0]/clip[3]),y=.5f-.5f*(clip[1]/clip[3]);
        if(!isfinite(x)||!isfinite(y)||fabsf(x-.5f)>1.5f||fabsf(y-.5f)>1.5f){
            atomic_store(&construction_projected,0);return;
        }
        corners[2*i]=x;corners[2*i+1]=y;
    }
    pthread_mutex_lock(&input_mutex);
    memcpy(construction_plane,corners,sizeof(corners));
    atomic_store(&construction_projected,1);
    pthread_mutex_unlock(&input_mutex);
}

/* Ground intersection of a screen aim (14181BDB0 convention), the projection of the original painters. */
static int aim_ground(CPU *cpu,const float aim[4],float ground[4]){
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);
    win64_pointer(c.rsp-8,256,1);
    uint64_t point=c.rsp+0x80,screen=c.rsp+0x90;
    memcpy((void*)(uintptr_t)screen,aim,16);
    c.rcx=point;c.rdx=screen;c.xmm[2].f32[0]=0;
    push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x14181BDB0));
    memcpy(ground,(const void*)(uintptr_t)point,16);
    return isfinite(ground[0])&&isfinite(ground[1])&&isfinite(ground[2]);
}

void fp_mobile_camera_after(CPU *cpu) {
    static uint64_t last_painter;
    static float last_ground[4];
    if(!atomic_load(&touch_construction)||!camera_available||(!construction_open&&!painter_kind)
       ||pause_menu_visible||atomic_load(&switch_menu)||rd8(GVA(0x142982F8D))){
        last_painter=0;atomic_store(&construction_projected,0);
        aim_point[0]=aim_point[1]=.5f;return;
    }
    unsigned alternate=rd32(GVA(0x142985AA4))==1;
    uint64_t renderer=rd64(GVA(alternate?0x143FC0998:0x142B3B598));
    uint64_t camera=renderer?rd64(renderer+(alternate?0xa8:0x20f8)):0;
    if(!camera){last_painter=0;atomic_store(&construction_projected,0);return;}
    uint64_t painter=painters?rd64(painters+0x80):0;
    float y=painter?.5f:construction_menu_aim;
    /* Ground intersection of the aim ray, using the same projection as the original painters. */
    float aim[4]={.5f,1.f-y,0,1};
    float ground[4];
    if(!aim_ground(cpu,aim,ground)){
        last_painter=0;atomic_store(&construction_projected,0);return;
    }
    if(aim_offset[0]!=0||aim_offset[1]!=0){
        /* The aim moved past the camera limits: its point on the build plane (Y=0), kept on the visible
         * screen; when the screen edge holds it back, the offset shrinks to the point actually aimed at. */
        float point[4]={ground[0]+aim_offset[0],0,ground[2]+aim_offset[1],1},clip[4],view[4];
        if(alternate){project_vector(camera+0x50,point,view);project_vector(camera+0x110,view,clip);}
        else project_vector(camera+0x1f0,point,clip);
        float x=clip[3]>0?.5f+.5f*(clip[0]/clip[3]):.5f,down=clip[3]>0?.5f-.5f*(clip[1]/clip[3]):.5f;
        if(!isfinite(x)||!isfinite(down))x=down=.5f;
        /* Left of the touch construction buttons (TouchControls.constructionX), between the top and
         * bottom HUD bars. */
        float kept_x=fminf(.90f,fmaxf(.04f,x)),kept_down=fminf(.86f,fmaxf(.10f,down));
        float moved[4]={kept_x,1.f-kept_down,0,1},hit[4];
        if(aim_ground(cpu,moved,hit)){
            if(kept_x!=x||kept_down!=down||clip[3]<=0){aim_offset[0]=hit[0]-ground[0];aim_offset[1]=hit[2]-ground[2];}
            memcpy(aim,moved,sizeof(aim));memcpy(ground,hit,sizeof(ground));
        } else aim_offset[0]=aim_offset[1]=0;
    }
    memcpy(aim_point,aim,sizeof(aim_point));
    {   /* The aim as it moves: screen position (x right, y down) and its ground offset from the centre. */
        static uint64_t logged_ns;static float logged[2]={-1,-1};uint64_t now=monotonic_ns();
        if((fabsf(aim[0]-logged[0])>.04f||fabsf(aim[1]-logged[1])>.04f)&&now-logged_ns>250000000u){
            logged_ns=now;logged[0]=aim[0];logged[1]=aim[1];
            fprintf(stderr,"FP_AIM x=%.3f y=%.3f offset=%.1f,%.1f ground=%.1f,%.1f painter=%u\n",aim[0],1.f-aim[1],
                aim_offset[0],aim_offset[1],ground[0],ground[2],painter_kind);
        }
    }
    publish_construction_plane(camera,alternate,ground);
    if(painter&&(painter!=last_painter||fabsf(ground[0]-last_ground[0])>.001f||fabsf(ground[2]-last_ground[2])>.001f))
        call(cpu,rd64(rd64(painter)+40)-g_image_delta,painter,aim,0,0,0);
    last_painter=painter;memcpy(last_ground,ground,16);
}

void fp_mobile_camera_destroy(void) {
    atomic_store(&gate_system,0);atomic_store(&gate_first_distant,-1);gate_entry=0;gate_count=0;
    pthread_mutex_lock(&input_mutex);
    camera_available=building_available=road_available=rectangle_count=setting_target_count=slider_target_count=reset_pending=0;saved_controller=0;
    atomic_store(&switch_menu,0);atomic_store(&switch_available,0);memset(switch_targets,0,sizeof(switch_targets));
    atomic_store(&switch_modal_visible,0);
    construction_close_target=0;construction_open=painter_kind=0;street_tool_pending=painter_clear_pending=0;
    construction_menu_aim=.5f;atomic_store(&construction_projected,0);
    aim_offset[0]=aim_offset[1]=0;aim_point[0]=aim_point[1]=.5f;
    street_detail_units=0;
    painters=panels=0;building_gesture=0;pause_menu_visible=0;debug_ready_since=0;atomic_store(&debug_requested,0);
    memset(pending,0,sizeof(pending));
    pending_building_turn=0;
    pending_speed=-1;time_rectangle_valid=time_state_valid=0;
    pthread_mutex_unlock(&input_mutex);
}
