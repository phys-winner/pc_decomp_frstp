#include "mobile_controls.h"
#include "port_settings.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
static uint64_t test_milliseconds=1000;
int clock_gettime(clockid_t clock,struct timespec *now){(void)clock;now->tv_sec=test_milliseconds/1000;now->tv_nsec=(test_milliseconds%1000)*1000000;return 0;}

int64_t g_image_delta;
int win64_skip_intro;
static unsigned scale=150,pan_calls,turn_calls,resize_calls,building_calls,position_calls,speed_calls,pause_calls,debug_calls;
static unsigned switch_active;
int frostpunk_switch_ui(void){return switch_active;}
static float pan_distance,turn_angle;
static float building_angle;
static unsigned layout_relayout_calls;
static float test_ime_fraction=1;
static uint64_t test_ime_editor;
static unsigned message_wrap_calls;
static unsigned table_row_calls;
static unsigned scrollbar_refresh_calls;
static unsigned technology_focus_calls;
static float technology_focus_x,technology_focus_y;
float fp_mobile_ime_visible_fraction(void){return test_ime_fraction;}
uint64_t fp_mobile_text_focused_editor(void){return test_ime_editor;}
static _Alignas(16) unsigned char component[0x80];
uint32_t fp_port_setting_get(uint32_t id){if(id==FP_SETTING_TOUCH_OFFSET)return 64;assert(id==FP_SETTING_UI_SCALE);return scale;}
int fp_port_free_building_pending(void){return 0;}
void fp_port_setting_set(uint32_t id,uint32_t value){(void)id;(void)value;assert(!"unexpected settings write");}
void *win64_pointer(uint64_t p,size_t n,int w){(void)n;(void)w;assert(p);return (void*)(uintptr_t)p;}
static void position_original(CPU*c);
static void created_original(CPU*c){(void)c;}
void dispatch(CPU *c,uint64_t target){
    if(target==UINT64_C(0x140ec4c70)){
        ++resize_calls;memcpy((void*)(uintptr_t)(c->rcx+0x210),(void*)(uintptr_t)c->rdx,16);
        const char *name=(const char*)(uintptr_t)rd64(c->rcx+0x1d8);
        uint64_t parent=rd64(c->rcx+0x230),root=parent?rd64(parent+0x230):0;
        const char *owner=root?(const char*)(uintptr_t)rd64(root+0x1d8):NULL;
        if(!name||strcmp(name,"Background")||!owner||strcmp(owner,"EXT_MESSAGE_SCOUTS_PANEL"))
            memcpy((void*)(uintptr_t)(c->rcx+0x100),(void*)(uintptr_t)c->rdx,16);
    }
    else if(target==UINT64_C(0x140f99fb0)){memcpy((void*)(uintptr_t)(c->rcx+0x100),(void*)(uintptr_t)c->rdx,16);}
    else if(target==UINT64_C(0x140fb3c50)){assert(!rd8(c->rcx+0x40e));wr8(c->rcx+0x40e,1);}
    else if(target==UINT64_C(0x140fb4090)){
        assert(rd8(c->rcx+0x40e)&&rdss(c->rcx+0x37c)>0);wr8(c->rcx+0x40e,0);++message_wrap_calls;
        /* Native glyph measuring is exercised in-game; this fixture supplies long/short measurements to test all geometry. */
        float measured=rdss(c->rcx+0x4d4);if(measured<60)wrss(c->rcx+0x4d4,40);
    }
    else if(target==UINT64_C(0x140ec39c0)){
        wr32(c->rcx+0x138,(uint32_t)c->rdx);
        uint64_t parent=rd64(c->rcx+0x230);
        if(parent&&(c->rdx==1||c->rdx==4))wrss(c->rcx+0x100,rdss(parent+0x100));
    }
    else if(target==UINT64_C(0x140ec30a0)){wr32(c->rcx+0x13c,(uint32_t)c->rdx);}
    else if(target==UINT64_C(0x140ec0f60)){wr32(c->rcx+0x1e0,rd32(c->rcx+0x1e0)|(uint32_t)c->rdx);}
    else if(target==UINT64_C(0x140ebe470)){wr32(c->rcx+0x1e0,rd32(c->rcx+0x1e0)&~(uint32_t)c->rdx);}
    else if(target==UINT64_C(0x141096ae0)){++layout_relayout_calls;}
    else if(target==UINT64_C(0x1413b1d00)||target==UINT64_C(0x1413b1c20)){assert(c->rdx==1);++scrollbar_refresh_calls;}
    else if(target==UINT64_C(0x141095700)){
        ++technology_focus_calls;
        if(!strcmp((const char*)(uintptr_t)rd64(c->rcx+0x1d8),"TECHTREE")){technology_focus_x=rdss(c->rdx);technology_focus_y=rdss(c->rdx+4);}
        assert(c->xmm[2].f32[0]==.18f);memcpy((void*)(uintptr_t)(c->rcx+0x330),(void*)(uintptr_t)c->rdx,16);
    }
    else if(target==UINT64_C(0x14109cfa0)){
        ++table_row_calls;wrss(c->rcx+52,c->xmm[1].f32[0]);
        uint64_t table=rd64(c->rcx+24),rows=rd64(table+0x2f0);float y=0;
        for(unsigned i=0;i<rd32(table+0x2f8);++i){uint64_t row=rd64(rows+8*i);wrss(row+48,y);y+=rdss(row+52);}
        wrss(table+0x324,y);
    }
    else if(target==UINT64_C(0x141a60190)){assert(rd32(c->rcx+0xd0)==2);wr32(c->rcx+0xd0,1);}
    else if(target==UINT64_C(0x140ec46e0)){memcpy((void*)(uintptr_t)(c->rcx+0xf0),(void*)(uintptr_t)c->rdx,16);}
    else if(target==UINT64_C(0x140ec4b90)){memcpy((void*)(uintptr_t)(c->rcx+0x110),(void*)(uintptr_t)c->rdx,16);}
    else if(target==UINT64_C(0x140ec3fd0)){++position_calls;CPU rendered=*c;fp_mobile_ui_position(&rendered,position_original);}
    else if(target==UINT64_C(0x141107de0)){
        ++speed_calls;assert(c->rdx<3&&c->r8==0);
        wrss(rd64(GVA(0x142b8b390))+0xd0,rdss(GVA(0x142b93b74)+c->rdx*4));
    }
    else if(target==UINT64_C(0x141107e40)){
        ++pause_calls;assert(c->rdx<2&&c->r8==0);wr32(rd64(GVA(0x142b8b390))+0xc0,(uint32_t)c->rdx);
    }
    else if(target==UINT64_C(0x141aaa610)){++building_calls;assert(c->rdx==0x23);}
    else if(target==UINT64_C(0x1419ed000)){++debug_calls;assert(c->rdx==2&&c->r8==0);}
    else if(target==UINT64_C(0x141c365d0)){
        ++pan_calls;pan_distance+=c->xmm[2].f32[0];
        float next[4];memcpy(next,(void*)(uintptr_t)(c->rcx+0x30),16);next[0]+=c->xmm[2].f32[0];
        memcpy((void*)(uintptr_t)(c->rcx+0x60),next,16);
        memcpy((void*)(uintptr_t)(c->rcx+0x170),(void*)(uintptr_t)(c->rcx+0x160),16);
    }
    else {assert(target==UINT64_C(0x141c36360));++turn_calls;turn_angle+=c->xmm[1].f32[0];}
    c->rsp+=8;
}
static void position_original(CPU*c){assert(rd32(c->rcx+0x138)==0);memcpy((void*)(uintptr_t)(c->rcx+0xc0),(void*)(uintptr_t)c->rdx,16);c->rsp+=8;}
static float expected_projection;
static float expected_template_width,expected_template_height;
static void template_original(CPU*c){
    assert(fabsf(rdss(c->rcx+(rd8(c->rcx+0x385)?0x378:0x37c))-expected_projection)<.001f);
    if(expected_template_width>0){
        assert(fabsf(rdss(c->rcx+0x100)-expected_template_width)<.001f);
        assert(fabsf(rdss(c->rcx+0x104)-expected_template_height)<.001f);
    }
}
#include "mobile_bottom_fixture.inc"
#include "mobile_newsflash_fixture.inc"
#include "mobile_law_selected_fixture.inc"
#include "mobile_dialog_affine_fixture.inc"
#include "mobile_dialog_popup_fixture.inc"
#include "mobile_editor_hit_fixture.inc"
#include "mobile_dlc_glow_fixture.inc"
#include "mobile_confirmation_fixture.inc"
#include "mobile_photo_mask_fixture.inc"
#include "mobile_scouts_fixture.inc"
#include "mobile_selection_close_fixture.inc"
#include "mobile_phone_panels_fixture.inc"
#include "mobile_switch_panels_fixture.inc"
static void check_mobile_bottom_family(CPU *cpu){
    _Alignas(16) unsigned char screen[0x3c0]={0},issues[0x280]={0},search[0x280]={0};
    uint64_t saved=cpu->rcx;cpu->rcx=(uintptr_t)screen;
    wr64((uintptr_t)screen+0x220,(uintptr_t)issues);wr64((uintptr_t)issues+0x240,(uintptr_t)search);
    wr64((uintptr_t)issues+0x1d8,(uintptr_t)"EXT_SOCIETY_ISSUES_PANEL");wr32((uintptr_t)issues+0x1e4,4300);
    wrss((uintptr_t)issues+0xc0,439.5f);wrss((uintptr_t)issues+0xc4,75);wr32((uintptr_t)issues+0x13c,14);
    wr64((uintptr_t)search+0x1d8,(uintptr_t)"SEARCH_CITIZENS_BUTTON");wr32((uintptr_t)search+0x1e4,4301);
    wrss((uintptr_t)search+0xc0,-142);wrss((uintptr_t)search+0xc4,-54);
    _Alignas(16) unsigned char header[0x280]={0},minor[0x280]={0},major[0x280]={0},counter[0x280]={0},primary[0x280]={0};
    wr64((uintptr_t)issues+0x220,(uintptr_t)header);wr64((uintptr_t)header+0x230,(uintptr_t)issues);
    wr64((uintptr_t)header+0x1d8,(uintptr_t)"HEADER");wr64((uintptr_t)header+0x220,(uintptr_t)major);
    wr64((uintptr_t)major+0x240,(uintptr_t)minor);
    uint64_t groups[]={(uintptr_t)major,(uintptr_t)minor},buttons[]={(uintptr_t)primary,(uintptr_t)counter};
    _Alignas(16) unsigned char artwork[2][3][0x280]={0};
    _Alignas(16) unsigned char text_nodes[2][2][0x280]={0};
    const char *artwork_names[]={"Ikona","graphic","circle_circle"};
    for(unsigned i=0;i<2;++i){
        uint64_t g=groups[i],b=buttons[i];wr64(g+0x230,(uintptr_t)header);wr64(g+0x1d8,(uintptr_t)(i?"DANGER_BUTTONS":"IMPORTANT_DANGER_BUTTONS"));
        wr32(g+0x1e4,4302+i);wr64(g+0x220,b);wr64(b+0x230,g);wr64(b,GVA(0x141D79F78));wr32(b+0x1e4,4304+i);
        wrss(g+0xc0,i?90:0);wrss(g+0xc4,i?81:1);wrss(b+0xc0,i?109.5f:200);wrss(b+0xd0,i?-13.5f:-19);
        wrss(b+0xf0,1);wrss(b+0x100,i?27:38);
        wrss(g+0xf4,1);wrss(b+0xf4,1);
        uint64_t parent=b;
        for(unsigned j=0;j<3;++j){
            uint64_t child=(uintptr_t)artwork[i][j];
            wr64(parent+0x220,child);wr64(child+0x230,parent);wr64(child+0x1d8,(uintptr_t)artwork_names[j]);
            wrss(child+0xf4,1);parent=child;
        }
        wrss(parent+0xc4,i?13:25);wrss(parent+0x104,20);
        uint64_t art=(uintptr_t)artwork[i][0],texts=(uintptr_t)text_nodes[i][0],value=(uintptr_t)text_nodes[i][1];
        wr64((uintptr_t)artwork[i][1]+0x240,texts);wr64(texts+0x230,art);wr64(texts+0x220,value);
        wr64(texts+0x1d8,(uintptr_t)(i?"critical_texts_layout":"TEXTS"));
        wr64(value+0x230,texts);wr64(value+0x1d8,(uintptr_t)"COUNT");
        wr32(texts+0x1e4,4310+i);wrss(texts+0xf4,1);wrss(texts+0xc4,i?39:8);
        wrss(value+0xf4,1);wrss(value+0xc4,i?1:2);wrss(value+0x104,i?12:15);
    }
    scale=200;fp_mobile_ui_before(cpu);assert(rdss((uintptr_t)issues+0xc0)==119.5f&&rdss((uintptr_t)issues+0xc4)==75&&rd32((uintptr_t)issues+0x13c)==0);
    assert(rdss((uintptr_t)search+0xc0)==-139.5f&&rdss((uintptr_t)search+0xc4)==-40);
    assert(rdss((uintptr_t)major+0xc0)==-17.5f&&rdss((uintptr_t)minor+0xc0)==113.5f);
    assert(rdss((uintptr_t)major+0xc4)==1&&rdss((uintptr_t)minor+0xc4)==13);
    float aligned_count_y=rdss((uintptr_t)text_nodes[1][0]+0xc4);
    assert(fabsf(aligned_count_y)<.01f);
    fp_mobile_ui_before(cpu);assert(rdss((uintptr_t)text_nodes[1][0]+0xc4)==aligned_count_y);
    wr32((uintptr_t)counter+0x1e0,2);fp_mobile_ui_before(cpu);assert(rdss((uintptr_t)major+0xc0)==0);
    wr32((uintptr_t)counter+0x1e0,0);wr32((uintptr_t)primary+0x1e0,2);fp_mobile_ui_before(cpu);assert(rdss((uintptr_t)minor+0xc0)==90.5f);
    assert(rdss((uintptr_t)minor+0xc4)==1);
    assert(rdss((uintptr_t)text_nodes[1][0]+0xc4)==5.5f);
    wr32((uintptr_t)primary+0x1e0,0);fp_mobile_ui_before(cpu);assert(rdss((uintptr_t)major+0xc0)==-17.5f);
    float position[4]={439.5f,75,0,1};CPU animation=*cpu;animation.rcx=(uintptr_t)issues;animation.rdx=(uintptr_t)position;
    fp_mobile_ui_position(&animation,position_original);assert(rdss((uintptr_t)issues+0xc0)==119.5f&&rdss((uintptr_t)issues+0xc4)==75);
    float frame_top=360+rdss((uintptr_t)search+0xc4)-14.5f-6;
    assert(fabsf(frame_top+21-320.5f)<.001f);
    position[0]=-142;position[1]=-54;animation.rcx=(uintptr_t)search;animation.rdx=(uintptr_t)position;
    fp_mobile_ui_position(&animation,position_original);
    assert(rdss((uintptr_t)search+0xc0)==-139.5f&&rdss((uintptr_t)search+0xc4)==-40);
    scale=100;fp_mobile_ui_before(cpu);assert(rdss((uintptr_t)issues+0xc0)==439.5f&&rdss((uintptr_t)issues+0xc4)==75&&rd32((uintptr_t)issues+0x13c)==14);
    assert(rdss((uintptr_t)search+0xc0)==-142&&rdss((uintptr_t)search+0xc4)==-54);cpu->rcx=saved;
    assert(rdss((uintptr_t)major+0xc0)==0&&rdss((uintptr_t)minor+0xc0)==90&&rdss((uintptr_t)minor+0xc4)==81);
}
static void check_population_inputs(CPU *cpu){
    static _Alignas(16) unsigned char storage[8][0x3c0],controller[0x180];
    uint64_t nodes[8];for(unsigned i=0;i<8;++i)nodes[i]=(uintptr_t)storage[i];
    const char *names[]={"","EXT_POPULATION_PANEL","ANIM","SHOW_TOOLTIP_DETAILS_BUTTON","employment_panel_clip","TOOLTIP_DETAILS","BUTTON_EMPLOYABLE_CHILDREN","OTHER_BUTTON"};
    for(unsigned i=0;i<8;++i){
        wr64(nodes[i],GVA(i==3||i>=6?0x141D79F78:0x141D78A60));wr64(nodes[i]+0x1d8,(uintptr_t)names[i]);
        wr32(nodes[i]+0x1e4,19000+i);wrss(nodes[i]+0xf0,1);wrss(nodes[i]+0xf4,1);
        wrss(nodes[i]+0x210,145);wrss(nodes[i]+0x100,145);wrss(nodes[i]+0x214,29);wrss(nodes[i]+0x104,29);
    }
    const unsigned parents[]={0,0,1,2,2,4,5,5};
    for(unsigned i=1;i<8;++i){uint64_t parent=nodes[parents[i]],child=rd64(parent+0x220);wr64(nodes[i]+0x230,parent);
        if(!child)wr64(parent+0x220,nodes[i]);else{while(rd64(child+0x240))child=rd64(child+0x240);wr64(child+0x240,nodes[i]);}}
    wrss(nodes[1]+0x210,245);wrss(nodes[1]+0x100,245);wrss(nodes[1]+0x214,474);wrss(nodes[1]+0x104,474);
    wr32(nodes[3]+0x1e0,0x10);wr32(nodes[6]+0x1e0,0x10);
    uint64_t owner=(uintptr_t)controller,details=nodes[5],callbacks[]={(uintptr_t)&owner,GVA(0x141A4D700),0,0};
    wr64(nodes[3]+0x258,(uintptr_t)callbacks);wr32(nodes[3]+0x260,1);wr64(owner+0x100,(uintptr_t)&details);
    uint64_t saved=cpu->rcx;cpu->rcx=nodes[0];scale=200;fp_mobile_ui_before(cpu);
    assert(!(rd32(nodes[6]+0x1e0)&0x10)&&(rd32(nodes[3]+0x1e0)&0x10));
    wr8(owner+0x108,1);fp_mobile_ui_before(cpu);assert((rd32(nodes[6]+0x1e0)&0x10)&&!(rd32(nodes[7]+0x1e0)&0x10));
    wr8(owner+0x108,0);fp_mobile_ui_before(cpu);assert(!(rd32(nodes[6]+0x1e0)&0x10));
    scale=100;fp_mobile_ui_before(cpu);assert(rd32(nodes[6]+0x1e0)&0x10);
    callbacks[1]=0;scale=200;fp_mobile_ui_before(cpu);assert(rd32(nodes[6]+0x1e0)&0x10);
    callbacks[1]=GVA(0x141A4D700);fp_mobile_ui_before(cpu);assert(!(rd32(nodes[6]+0x1e0)&0x10));
    cpu->rcx=saved;
}

static void check_mobile_technology(CPU *cpu){
    _Alignas(16) unsigned char storage[23][0x600]={0},tech_controller[0x300]={0},tech_records[3][128]={0};
    uint64_t nodes[23];for(unsigned i=0;i<23;++i)nodes[i]=(uintptr_t)storage[i];
    const char *names[]={"","EXT_TECHNOLOGY_PANEL","FRAME","ANI","TECHTREE_PARENT","TECHTREE","CANVAS_CONTENT_PANEL","DETAILS_WINDOW","bottom","TOTAL_PROGRESS","ALL_EMPLOYEES_TEXT","CITY_RESOURCES_LAYOUT",
        "MAIN_LAYOUT","INFO_PANE_LAYOUT","TABS_LAYOUT","TAB_BUTTON","clip","show_anim","move_anim","NAME_TEXT","GOAL_ENTRIES_SCROLL_PANE","GOAL_ENTRY","TECH_ENTRY"};
    const unsigned parents[]={0,0,1,2,3,4,5,4,3,8,8,3,7,12,4,14,15,16,17,18,4,20,6};
    for(unsigned i=0;i<23;++i){
        wr64(nodes[i],GVA(i==0?0x141D7B0B0:(i==5||i==20)?0x141DA74C8:i==19?0x141DED7D8:0x141D78A60));wr64(nodes[i]+0x1d8,(uintptr_t)names[i]);wr32(nodes[i]+0x1e4,4200+i);
        wrss(nodes[i]+0xf0,1);wrss(nodes[i]+0xf4,1);wrss(nodes[i]+0x210,i==1?900:i==6?1112:1280);wrss(nodes[i]+0x214,i==6?700:600);
        wrss(nodes[i]+0x100,rdss(nodes[i]+0x210));wrss(nodes[i]+0x104,rdss(nodes[i]+0x214));
        if(i){wr64(nodes[i]+0x230,nodes[parents[i]]);uint64_t child=rd64(nodes[parents[i]]+0x220);
            if(child){while(rd64(child+0x240))child=rd64(child+0x240);wr64(child+0x240,nodes[i]);}else wr64(nodes[parents[i]]+0x220,nodes[i]);}
    }
    wr32(nodes[5]+0x1e0,0x04300201);wr32(nodes[5]+0x370,3);wr32(nodes[20]+0x1e0,0x00300211);wr32(nodes[20]+0x370,3);
    wrss(nodes[9]+0x100,200);wrss(nodes[9]+0x104,30);wrss(nodes[10]+0x100,339);wrss(nodes[10]+0x104,64);
    wrss(nodes[11]+0x100,473);wrss(nodes[11]+0x104,60);
    wrss(nodes[12]+0x100,297);wrss(nodes[12]+0x104,520);wrss(nodes[13]+0x100,297);wrss(nodes[13]+0x104,201);wrss(nodes[13]+0xc4,159.5f);
    wrss(nodes[19]+0x37c,127);wrss(nodes[19]+0x380,20);wr8(nodes[19]+0x384,1);
    uint64_t tech=(uintptr_t)tech_controller,tech_ref=tech,tech_callbacks[]={(uintptr_t)&tech_ref,GVA(0x141A68920),0,0};
    wr64(tech,GVA(0x14212EEE0));wr64(tech+0x188,(uintptr_t)&nodes[5]);wr64(tech+0x1a8,(uintptr_t)tech_records);wr32(tech+0x1b0,3);
    for(unsigned i=0;i<2;++i){wr64((uintptr_t)tech_records[i],i+1);wr64((uintptr_t)tech_records[i]+32,(uintptr_t)&nodes[22]);wrss((uintptr_t)tech_records[i]+48,648+100*i);wrss((uintptr_t)tech_records[i]+52,424);}
    wr64((uintptr_t)tech_records[2],3);wr64((uintptr_t)tech_records[2]+16,(uintptr_t)&nodes[21]);wrss((uintptr_t)tech_records[2]+52,136);
    wrss(nodes[6]+0xc4,-80);wrss(nodes[21]+0xc4,345);
    wr64(tech+0xe0,1);wr64(nodes[22]+0x258,(uintptr_t)tech_callbacks);wr32(nodes[22]+0x260,1);
    unsigned focus_before=technology_focus_calls;
    uint64_t saved=cpu->rcx;cpu->rcx=nodes[0];scale=200;fp_mobile_ui_before(cpu);
    assert(rdss(nodes[1]+0xf0)==1&&rdss(nodes[1]+0x210)==640);
    assert(technology_focus_calls==focus_before+1&&fabsf(technology_focus_x-(648-338/.55f/2))<.001f&&fabsf(technology_focus_y-(424-224/.55f/2))<.001f);
    assert(rdss(nodes[21]+0xc4)==56);
    wrss(nodes[6]+0xc4,-120);wrss(nodes[21]+0xc4,300);
    fp_mobile_ui_before(cpu);assert(technology_focus_calls==focus_before+1&&rdss(nodes[21]+0xc4)==16);
    wr64(tech+0xe0,2);fp_mobile_ui_before(cpu);assert(technology_focus_calls==focus_before+2&&fabsf(technology_focus_x-(748-338/.55f/2))<.001f);
    assert(fabsf(rdss(nodes[5]+0x210)*rdss(nodes[5]+0xf0)-338)<.001f&&fabsf(rdss(nodes[5]+0x214)*rdss(nodes[5]+0xf4)-224)<.001f&&rd32(nodes[5]+0x370)==1);
    assert((rd32(nodes[5]+0x1e0)&0x04000010)==0x04000010&&(rd32(nodes[20]+0x1e0)&0x04000010)==0x04000010);
    assert(rdss(nodes[6]+0x210)==1112&&rdss(nodes[6]+0x214)==700&&rdss(nodes[6]+0xf0)==1);
    assert(rdss(nodes[7]+0xc0)>300&&fabsf(rdss(nodes[7]+0xf0)-.65625f)<.001f&&rdss(nodes[13]+0xc4)==0);
    assert(rdss(nodes[5]+0xc0)+rdss(nodes[5]+0x100)*rdss(nodes[5]+0xf0)<=rdss(nodes[7]+0xc0));
    assert(rdss(nodes[20]+0xf0)==rdss(nodes[5]+0xf0));
    assert(rdss(nodes[9]+0xc4)+rdss(nodes[8]+0xc4)+30*rdss(nodes[9]+0xf4)<=360);
    assert(rdss(nodes[11]+0xc4)+60*rdss(nodes[11]+0xf4)<=360);
    assert(rdss(nodes[7]+0xc4)+213*rdss(nodes[7]+0xf4)<306);
    wr32(nodes[7]+0x1e0,2);fp_mobile_ui_before(cpu);
    assert(fabsf(rdss(nodes[5]+0x210)*rdss(nodes[5]+0xf0)-548)<.001f);
    wr32(nodes[7]+0x1e0,0);fp_mobile_ui_before(cpu);
    assert(fabsf(rdss(nodes[5]+0x210)*rdss(nodes[5]+0xf0)-338)<.001f);
    assert(rdss(nodes[15]+0x210)==152&&rdss(nodes[15]+0x214)==40&&rdss(nodes[16]+0x214)==42);
    assert(fabsf(rdss(nodes[19]+0x37c)-142/.95f)<.001f&&rdss(nodes[19]+0x380)==0&&rd8(nodes[19]+0x384)==0);
    /* Actual TECHTREE has no input quad before focus rebuild. Publish its real viewport, including nonzero parent origin. */
    _Alignas(16) uint64_t list[4]={nodes[5],0,nodes[20],0};
    uint64_t old_list=rd64(GVA(0x142A89088));unsigned old_count=rd32(GVA(0x142A89090));
    wr64(GVA(0x142A89088),(uintptr_t)list);wr32(GVA(0x142A89090),2);fp_mobile_camera_before(cpu);
    assert((frostpunk_mobile_context(.4f,.5f)&4098)==4098);
    assert(!(frostpunk_mobile_context(.4f,.2f)&4096));
    wr32(nodes[1]+0x1e0,2);fp_mobile_camera_before(cpu);assert(!(frostpunk_mobile_context(.4f,.5f)&4096));wr32(nodes[1]+0x1e0,0);
    wr64(GVA(0x142A89088),old_list);wr32(GVA(0x142A89090),old_count);
    scale=100;fp_mobile_ui_before(cpu);assert(rdss(nodes[5]+0x210)==1280&&rdss(nodes[6]+0x210)==1112&&rdss(nodes[6]+0xf0)==1);
    assert(technology_focus_calls==focus_before+2);
    assert(rd32(nodes[5]+0x1e0)==0x04300201&&rd32(nodes[20]+0x1e0)==0x00300211&&rd32(nodes[5]+0x370)==3&&rd32(nodes[20]+0x370)==3);
    assert(rdss(nodes[13]+0xc4)==159.5f&&rdss(nodes[19]+0x37c)==127&&rdss(nodes[19]+0x380)==20&&rd8(nodes[19]+0x384)==1);
    cpu->rcx=saved;
}

int main(void){
    assert(!frostpunk_dxvk_scene_active());
    unsigned char *image=calloc(1,0x4539000);assert(image);g_image_delta=(intptr_t)image-UINT64_C(0x140000000);
    wrss(GVA(0x142980e60),1280);wrss(GVA(0x142980e64),720);
    fp_mobile_ui_scale();assert(rdss(GVA(0x142980e60))==1280);
    _Alignas(16) unsigned char system[0x340]={0},controller[0x240]={0},stack[2048];
    CPU cpu={.rcx=(uintptr_t)system,.rsp=(uintptr_t)stack+sizeof(stack)-32};
    wr64((uintptr_t)system+0xa0,(uintptr_t)controller);wrss((uintptr_t)controller+0x30,100);wrss((uintptr_t)controller+0x2c,1);
    fp_mobile_camera_before(&cpu);assert(frostpunk_dxvk_scene_active());
    fp_mobile_ui_scale();assert(fabsf(rdss(GVA(0x142980e60))-1280/1.5f)<.001f);
    scale=200;fp_mobile_ui_scale();assert(rdss(GVA(0x142980e60))==640&&rdss(GVA(0x142980e64))==360);
    scale=100;fp_mobile_ui_scale();assert(rdss(GVA(0x142980e60))==1280&&rdss(GVA(0x142980e64))==720);
    wr8(GVA(0x142982F8D),1);assert(frostpunk_mobile_context(.5f,.5f)==2048);wr8(GVA(0x142982F8D),0);
    _Alignas(16) unsigned char screen[0x3c0]={0},frame[0x280]={0},panel[0x280]={0},effect[0x280]={0};
    wr64((uintptr_t)screen+0x220,(uintptr_t)frame);wr64((uintptr_t)frame+0x240,(uintptr_t)panel);
    wrss((uintptr_t)frame+0x210,1280);wrss((uintptr_t)frame+0x214,720);
    wrss((uintptr_t)panel+0x210,400);wrss((uintptr_t)panel+0x214,300);
    cpu.rcx=(uintptr_t)screen;scale=150;fp_mobile_ui_before(&cpu);
    assert(resize_calls==1&&fabsf(rdss((uintptr_t)frame+0x210)-1280/1.5f)<.001f);
    assert(rdss((uintptr_t)panel+0x210)==400);fp_mobile_ui_before(&cpu);assert(resize_calls==1);
    scale=200;fp_mobile_ui_before(&cpu);assert(resize_calls==2&&rdss((uintptr_t)frame+0x210)==640);
    wr64((uintptr_t)frame+0x220,(uintptr_t)effect);wr64((uintptr_t)effect,GVA(0x141d796f8));
    wrss((uintptr_t)effect+0x210,1280);wrss((uintptr_t)effect+0x214,720);wrss((uintptr_t)effect+0xc4,-192);
    fp_mobile_ui_before(&cpu);assert(rd32((uintptr_t)effect+0x138)==5);
    float slide[4]={0,-192,0,1};CPU animation=cpu;animation.rcx=(uintptr_t)effect;animation.rdx=(uintptr_t)slide;
    fp_mobile_ui_position(&animation,position_original);assert(rdss((uintptr_t)effect+0xc4)==-96&&rd32((uintptr_t)effect+0x138)==5);
    assert(position_calls==1);
    wr64((uintptr_t)frame+0x220,0);scale=150;fp_mobile_ui_before(&cpu);assert(position_calls==1);
    wr64((uintptr_t)frame+0x220,(uintptr_t)effect);fp_mobile_ui_before(&cpu);assert(position_calls==2);
    scale=100;fp_mobile_ui_before(&cpu);assert(position_calls==3);
    _Alignas(16) unsigned char mask[0x400]={0};CPU rendering=cpu;rendering.rcx=(uintptr_t)mask;
    wr64((uintptr_t)mask+0x1d8,(uintptr_t)"SFX_TXT_MASK");wr8((uintptr_t)mask+0x385,1);wrss((uintptr_t)mask+0x378,75);
    scale=200;fp_mobile_ui_scale();expected_projection=atanf(tanf(75*(3.141592654f/360))/2)*(360/3.141592654f);
    fp_mobile_ui_template(&rendering,template_original);assert(rdss((uintptr_t)mask+0x378)==75);
    wr8((uintptr_t)mask+0x385,0);wrss((uintptr_t)mask+0x37c,10);expected_projection=20;
    fp_mobile_ui_template(&rendering,template_original);assert(rdss((uintptr_t)mask+0x37c)==10);
    wr64((uintptr_t)mask+0x1d8,(uintptr_t)"SFX_WEATHER");expected_projection=10;fp_mobile_ui_template(&rendering,template_original);
    scale=100;fp_mobile_ui_scale();wr64((uintptr_t)mask+0x1d8,(uintptr_t)"SFX_TXT_MASK");fp_mobile_ui_template(&rendering,template_original);
    _Alignas(16) unsigned char centered[0x280]={0};
    wr64((uintptr_t)frame+0x220,(uintptr_t)centered);
    wrss((uintptr_t)centered+0x210,1280);wrss((uintptr_t)centered+0x214,600);
    scale=200;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)centered+0x210)==640&&rdss((uintptr_t)centered+0x214)==600);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)centered+0x210)==1280);
    _Alignas(16) unsigned char wide[0x280]={0},content[0x280]={0},bottom[0x280]={0};
    wr64((uintptr_t)wide,GVA(0x141d78a60));wr64((uintptr_t)wide+0x220,(uintptr_t)content);
    wr64((uintptr_t)wide+0x240,(uintptr_t)bottom);wr64((uintptr_t)wide+0x230,(uintptr_t)frame);
    wrss((uintptr_t)wide+0x100,1000);wrss((uintptr_t)wide+0x104,200);wrss((uintptr_t)wide+0x210,1000);wrss((uintptr_t)wide+0x214,200);
    wrss((uintptr_t)wide+0xf0,1);wrss((uintptr_t)wide+0xf4,1);wrss((uintptr_t)wide+0xc0,140);
    wr64((uintptr_t)bottom+0x230,(uintptr_t)frame);wr32((uintptr_t)bottom+0x13c,14);
    wr64((uintptr_t)bottom+0x1d8,(uintptr_t)"BOTTOM_static");
    wr64((uintptr_t)frame+0x220,(uintptr_t)wide);cpu.rcx=(uintptr_t)screen;scale=200;fp_mobile_ui_before(&cpu);
    assert(fabsf(rdss((uintptr_t)wide+0xf0)-.624f)<.0001f&&rd32((uintptr_t)wide+0x13c)==22);
    assert(rdss((uintptr_t)bottom+0x100)>0&&rd32((uintptr_t)bottom+0x138)==4);
    assert(rdss((uintptr_t)bottom+0xc4)==360);
    float original_bottom[4]={0,720,0,1};animation.rcx=(uintptr_t)bottom;animation.rdx=(uintptr_t)original_bottom;
    fp_mobile_ui_position(&animation,position_original);assert(rdss((uintptr_t)bottom+0xc4)==360);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)wide+0xf0)==1);
    check_mobile_bottom_family(&cpu);
    check_population_inputs(&cpu);
    check_mobile_technology(&cpu);check_mobile_scouts(&cpu);check_mobile_selection_close(&cpu);check_mobile_phone_panels(&cpu);check_phone_hud_cover(&cpu);
    check_mobile_bottom(&cpu);
    check_mobile_newsflash(&cpu);
    _Alignas(16) unsigned char bar[0x280]={0},group[0x280]={0},first_button[0x280]={0},last_button[0x280]={0};
    wr64((uintptr_t)frame+0x220,(uintptr_t)bar);wr64((uintptr_t)bar+0x220,(uintptr_t)group);
    wrss((uintptr_t)bar+0x210,1280);wrss((uintptr_t)bar+0x214,208);wrss((uintptr_t)bar+0x100,640);wrss((uintptr_t)bar+0x104,208);
    wr64((uintptr_t)group+0x1d8,(uintptr_t)"MAIN_BUTTONS");
    wr64((uintptr_t)group,GVA(0x141d78a60));wr64((uintptr_t)group+0x230,(uintptr_t)bar);
    wr64((uintptr_t)group+0x220,(uintptr_t)first_button);wr32((uintptr_t)group+0x13c,16);
    wrss((uintptr_t)group+0x210,64);wrss((uintptr_t)group+0x214,64);wrss((uintptr_t)group+0x100,64);wrss((uintptr_t)group+0xc0,18);
    wr64((uintptr_t)first_button+0x240,(uintptr_t)last_button);
    wrss((uintptr_t)first_button+0xc0,482);wrss((uintptr_t)last_button+0xc0,765);
    wrss((uintptr_t)first_button+0x100,60);wrss((uintptr_t)last_button+0x100,60);
    scale=200;fp_mobile_ui_before(&cpu);assert(rd32((uintptr_t)group+0x13c)==22&&rdss((uintptr_t)group+0xc0)==-302);
    fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)group+0xc0)==-302);
    float game_position[4]={18,-1,0,2};animation.rcx=(uintptr_t)group;animation.rdx=(uintptr_t)game_position;
    fp_mobile_ui_position(&animation,position_original);assert(rdss((uintptr_t)group+0xc0)==-302);
    _Alignas(16) unsigned char construction[0x280]={0},scroll[0x280]={0},left[0x280]={0},right[0x280]={0};
    wr64((uintptr_t)construction+0x1d8,(uintptr_t)"EXT_CONSTRUCTION_PANEL");
    wr64((uintptr_t)frame+0x220,(uintptr_t)construction);wr64((uintptr_t)construction+0x220,(uintptr_t)scroll);
    wr64((uintptr_t)scroll+0x240,(uintptr_t)left);wr64((uintptr_t)left+0x240,(uintptr_t)right);
    uint64_t widgets[]={(uintptr_t)scroll,(uintptr_t)left,(uintptr_t)right};
    const char *names[]={"BUILDING_SCROLL_PANE","left_button_clip","right_button_clip"};
    for(unsigned i=0;i<3;++i){wr64(widgets[i]+0x230,(uintptr_t)construction);wr64(widgets[i]+0x1d8,(uintptr_t)names[i]);}
    fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)scroll+0xc0)==404&&rdss((uintptr_t)scroll+0x210)==678);
    assert(rdss((uintptr_t)left+0xc0)==331&&rdss((uintptr_t)right+0xc0)==894);
    float scroll_position[4]={84,24,0,0};animation.rcx=(uintptr_t)scroll;animation.rdx=(uintptr_t)scroll_position;
    fp_mobile_ui_position(&animation,position_original);assert(rdss((uintptr_t)scroll+0xc0)==404);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)scroll+0xc0)==84&&rdss((uintptr_t)scroll+0x210)==1318);
    _Alignas(16) unsigned char tabs[0x280]={0};
    wr64((uintptr_t)right+0x240,(uintptr_t)tabs);wr64((uintptr_t)tabs+0x230,(uintptr_t)construction);
    wr64((uintptr_t)tabs+0x1d8,(uintptr_t)"BUILDING_TABS_PANEL");wrss((uintptr_t)tabs+0x210,975);
    scale=200;fp_mobile_ui_before(&cpu);assert(fabsf(rdss((uintptr_t)tabs+0xf0)-610.f/975)<.0001f);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)tabs+0xf0)==1);
    _Alignas(16) unsigned char lawroot[0x280]={0},categories[0x280]={0},lawview[0x3c0]={0},tree[0x280]={0};
    _Alignas(16) unsigned char lawanim[0x280]={0},treeview[0x280]={0},treecontent[0x280]={0},lawsfx[0x280]={0},treeclip[0x280]={0};
    _Alignas(16) unsigned char categoryview[0x280]={0},categorycontent[0x280]={0},nestedframe[0x280]={0};
    uint64_t law_nodes[]={(uintptr_t)lawroot,(uintptr_t)lawanim,(uintptr_t)treeview,(uintptr_t)treecontent,
        (uintptr_t)lawsfx,(uintptr_t)treeclip,(uintptr_t)lawview,(uintptr_t)tree,(uintptr_t)categoryview,
        (uintptr_t)categorycontent,(uintptr_t)categories,(uintptr_t)nestedframe};
    const char *law_names[]={"EXT_SOCIOTECH_PANEL","ANIM","TREE_VIEW","CONTENT","SFX_MASK","TreeClip",
        "SOCIOTECHTREE","TREE_PANEL","CATEGORIES_VIEW","CONTENT","CATEGORIES_LAYOUT","FRAME"};
    const float law_sizes[][2]={{1280,720},{1280,720},{1280,720},{1280,600},{64,64},{1276,721},
        {1280,722},{2600,2400},{1280,720},{1280,600},{1163.95f,600},{64,64}};
    for(unsigned i=0;i<12;++i){
        uint64_t node=law_nodes[i];wr64(node,GVA(0x141D78A60));wr64(node+0x1d8,(uintptr_t)law_names[i]);
        wrss(node+0x100,law_sizes[i][0]);wrss(node+0x104,law_sizes[i][1]);
        wrss(node+0x210,law_sizes[i][0]);wrss(node+0x214,law_sizes[i][1]);
        wrss(node+0xf0,1);wrss(node+0xf4,1);
    }
    const unsigned law_parent_indices[]={0,0,1,2,3,4,5,6,1,8,9,7};
    for(unsigned i=1;i<12;++i)wr64(law_nodes[i]+0x230,law_nodes[law_parent_indices[i]]);
    wr64((uintptr_t)frame+0x220,(uintptr_t)lawroot);wr64((uintptr_t)lawroot+0x230,(uintptr_t)frame);
    wr64((uintptr_t)lawroot+0x220,(uintptr_t)lawanim);wr64((uintptr_t)lawanim+0x220,(uintptr_t)treeview);
    wr64((uintptr_t)treeview+0x240,(uintptr_t)categoryview);wr64((uintptr_t)treeview+0x220,(uintptr_t)treecontent);
    wr64((uintptr_t)treecontent+0x220,(uintptr_t)lawsfx);wr64((uintptr_t)lawsfx+0x220,(uintptr_t)treeclip);
    wr64((uintptr_t)treeclip+0x220,(uintptr_t)lawview);wr64((uintptr_t)lawview+0x220,(uintptr_t)tree);
    wr64((uintptr_t)tree+0x220,(uintptr_t)nestedframe);wr64((uintptr_t)categoryview+0x220,(uintptr_t)categorycontent);
    wr64((uintptr_t)categorycontent+0x220,(uintptr_t)categories);wr64((uintptr_t)categories+0x220,(uintptr_t)content);
    wr64((uintptr_t)content+0x230,(uintptr_t)categories);
    wr64((uintptr_t)lawview,GVA(0x141DA74C8));wr64((uintptr_t)categories,GVA(0x141DE0A48));
    scale=200;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)categories+0xc0)>=0&&rdss((uintptr_t)categories+0xc0)+1163.95f*rdss((uintptr_t)categories+0xf0)<=640);
    assert(rdss((uintptr_t)lawview+0x210)==636&&rdss((uintptr_t)lawview+0x214)==304);
    assert(rdss((uintptr_t)treeclip+0x210)==636&&rdss((uintptr_t)treeclip+0x214)==304);
    assert(rdss((uintptr_t)tree+0xf0)==1&&rdss((uintptr_t)tree+0x210)==2600&&rdss((uintptr_t)tree+0x214)==2400);
    assert(rdss((uintptr_t)nestedframe+0x210)==64&&rdss((uintptr_t)nestedframe+0x214)==64);
    void fp_mobile_law_camera(CPU*,void (*)(CPU*));
    _Alignas(16) unsigned char lawcamera[0x2b0]={0};uint64_t lawview_handle=(uintptr_t)lawview;
    wr64((uintptr_t)lawcamera+0xd8,(uintptr_t)&lawview_handle);
    wrss((uintptr_t)lawcamera+0x260,633.5f);wrss((uintptr_t)lawcamera+0x264,303.5f);
    wrss((uintptr_t)lawcamera+0x270,345);wrss((uintptr_t)lawcamera+0x274,349);
    wrss((uintptr_t)lawcamera+0x280,1280);wrss((uintptr_t)lawcamera+0x284,722);
    CPU law_camera_cpu=cpu;law_camera_cpu.rcx=(uintptr_t)lawcamera;
    fp_mobile_law_camera(&law_camera_cpu,created_original);
    assert(rdss((uintptr_t)lawcamera+0x260)==318&&rdss((uintptr_t)lawcamera+0x264)==152);
    assert(fabsf(rdss((uintptr_t)lawcamera+0x270)-106.8f)<.001f&&rdss((uintptr_t)lawcamera+0x274)==232);
    assert(rdss((uintptr_t)lawcamera+0x280)==636&&rdss((uintptr_t)lawcamera+0x284)==304);
    scale=100;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)lawroot+0x210)==1280&&rdss((uintptr_t)lawroot+0x214)==720);
    assert(rdss((uintptr_t)lawview+0x210)==1280&&rdss((uintptr_t)lawview+0x214)==722);
    assert(rdss((uintptr_t)categories+0xf0)==1&&rdss((uintptr_t)nestedframe+0x210)==64);
    fp_mobile_law_camera(&law_camera_cpu,created_original);
    assert(rdss((uintptr_t)lawcamera+0x260)==633.5f&&rdss((uintptr_t)lawcamera+0x264)==303.5f);
    assert(rdss((uintptr_t)lawcamera+0x270)==345&&rdss((uintptr_t)lawcamera+0x274)==349);
    assert(rdss((uintptr_t)lawcamera+0x280)==1280&&rdss((uintptr_t)lawcamera+0x284)==722);
    scale=200;
    wr64((uintptr_t)lawroot+0x1d8,(uintptr_t)"EXT_INGAME_MENU_SETTINGS_PANEL");
    wr64((uintptr_t)categories+0x1d8,(uintptr_t)"HEADER_LAYOUT");wr64((uintptr_t)content+0x1d8,(uintptr_t)"TEXT");
    wr64((uintptr_t)content+0x230,(uintptr_t)categories);
    fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)lawroot+0xf0)==.5f&&rdss((uintptr_t)lawroot+0x100)==1280);
    _Alignas(16) unsigned char dialog_bg[0x280]={0};
    wr64((uintptr_t)lawroot+0x220,(uintptr_t)dialog_bg);wr64((uintptr_t)dialog_bg+0x230,(uintptr_t)lawroot);
    wr64((uintptr_t)dialog_bg+0x1d8,(uintptr_t)"darken_lefttop");
    wrss((uintptr_t)dialog_bg+0x210,1280);wrss((uintptr_t)dialog_bg+0x214,720);
    wrss((uintptr_t)dialog_bg+0x100,640);wrss((uintptr_t)dialog_bg+0x104,360);
    wrss((uintptr_t)dialog_bg+0xf0,1);wrss((uintptr_t)dialog_bg+0xf4,1);wr32((uintptr_t)dialog_bg+0x138,5);
    fp_mobile_ui_before(&cpu);assert(rd32((uintptr_t)dialog_bg+0x138)==0&&rdss((uintptr_t)dialog_bg+0x100)==1280);
    wr64((uintptr_t)dialog_bg,GVA(0x141D78A60));wr64((uintptr_t)dialog_bg+0x1d8,(uintptr_t)"SETTINGS_DETAILED");
    wr32((uintptr_t)dialog_bg+0x138,6);wrss((uintptr_t)dialog_bg+0x100,640);wrss((uintptr_t)dialog_bg+0x104,400);
    fp_mobile_ui_before(&cpu);assert(rd32((uintptr_t)dialog_bg+0x138)==0&&rdss((uintptr_t)dialog_bg+0x100)==1280&&rdss((uintptr_t)dialog_bg+0x104)==720);
    wr64((uintptr_t)dialog_bg,GVA(0x141DA2AA8));wr64((uintptr_t)dialog_bg+0x1d8,(uintptr_t)"Sequel_splash_illustrationFP2BookFollow");
    wrss((uintptr_t)dialog_bg+0x210,1007);wrss((uintptr_t)dialog_bg+0x214,596);
    wrss((uintptr_t)dialog_bg+0x100,1007);wrss((uintptr_t)dialog_bg+0x104,596);
    fp_mobile_ui_before(&cpu);float art_scale=rdss((uintptr_t)lawroot+0xf0)*rdss((uintptr_t)dialog_bg+0xf0);
    assert(1007*art_scale<=608.001f&&596*art_scale<=280.001f);
    fp_mobile_ui_before(&cpu);assert(fabsf(rdss((uintptr_t)lawroot+0xf0)*rdss((uintptr_t)dialog_bg+0xf0)-art_scale)<.0001f);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)lawroot+0xf0)==1);
    wr64((uintptr_t)lawroot,GVA(0x141D78A60));wr64((uintptr_t)lawroot+0x1d8,(uintptr_t)"BACKGROUND");
    wr64((uintptr_t)dialog_bg+0x1d8,(uintptr_t)"PEOPLE_CANVAS");wr32((uintptr_t)dialog_bg+0x138,5);
    wrss((uintptr_t)dialog_bg+0x210,1280);wrss((uintptr_t)dialog_bg+0x214,720);
    wrss((uintptr_t)dialog_bg+0x100,1280);wrss((uintptr_t)dialog_bg+0x104,720);
    wrss((uintptr_t)dialog_bg+0xf0,2.5f);wrss((uintptr_t)dialog_bg+0xf4,2.5f);
    for(unsigned percent=100;percent<=200;percent+=10){scale=percent;fp_mobile_ui_before(&cpu);
        assert(fabsf(rdss((uintptr_t)lawroot+0xf0)-100.f/percent)<.0001f);
        assert(rdss((uintptr_t)dialog_bg+0x100)==1280&&rdss((uintptr_t)dialog_bg+0xf0)==2.5f);
    }
    wr64((uintptr_t)lawroot+0x1d8,(uintptr_t)"INGAME_MENU_OVERLAY");
    scale=100;fp_mobile_ui_before(&cpu);assert(fabsf(rdss((uintptr_t)lawroot+0xf0)-1)<.0001f);
    scale=200;fp_mobile_ui_before(&cpu);assert(fabsf(rdss((uintptr_t)lawroot+0xf0)-.5f)<.0001f);
    assert(rdss((uintptr_t)dialog_bg+0x100)==1280&&rdss((uintptr_t)dialog_bg+0xf0)==2.5f);
    scale=100;fp_mobile_ui_scale();wrss(GVA(0x142980e60),1280);wrss(GVA(0x142980e64),720);
    _Alignas(16) unsigned char resource_bar[0x280]={0},society_bar[0x280]={0};
    wr64((uintptr_t)frame+0x220,(uintptr_t)resource_bar);wr64((uintptr_t)resource_bar+0x240,(uintptr_t)society_bar);
    wr64((uintptr_t)resource_bar+0x1d8,(uintptr_t)"EXT_RESOURCES_PANEL");
    wr64((uintptr_t)society_bar+0x1d8,(uintptr_t)"EXT_SOCIETY_STATE_PANEL");
    wrss((uintptr_t)resource_bar+0x210,1073);wrss((uintptr_t)resource_bar+0x214,187);
    wrss((uintptr_t)society_bar+0x210,216);wrss((uintptr_t)society_bar+0x214,34);
    wrss((uintptr_t)society_bar+0xd0,-108);wrss((uintptr_t)society_bar+0xd4,-17);
    scale=200;fp_mobile_ui_before(&cpu);
    float resource_scale=rdss((uintptr_t)resource_bar+0xf0);
    assert(fabsf(rdss((uintptr_t)resource_bar+0xc0)+504*resource_scale-320)<.001f&&fabsf(rdss((uintptr_t)resource_bar+0xc4)+12*resource_scale)<.001f);
    assert(fabsf(resource_scale-632.f/840)<.001f&&resource_scale>616.f/1073);
    assert(rdss((uintptr_t)society_bar+0xc0)==320&&rdss((uintptr_t)society_bar+0xc4)==324);
    _Alignas(16) unsigned char bar_blocker[0x280]={0};
    wr64((uintptr_t)resource_bar+0x220,(uintptr_t)bar_blocker);wr64((uintptr_t)bar_blocker+0x230,(uintptr_t)resource_bar);
    wr64((uintptr_t)bar_blocker+0x1d8,(uintptr_t)"lock_click_btn");wr32((uintptr_t)bar_blocker+0x1e4,19500);wr32((uintptr_t)bar_blocker+0x1e0,0x10);
    fp_mobile_ui_before(&cpu);assert(!(rd32((uintptr_t)bar_blocker+0x1e0)&0x10));
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)resource_bar+0xf0)==1);
    assert(rdss((uintptr_t)resource_bar+0xc0)==103.5f&&rdss((uintptr_t)resource_bar+0xc4)==-12);
    assert(rd32((uintptr_t)bar_blocker+0x1e0)&0x10);wr64((uintptr_t)resource_bar+0x220,0);
    wr64((uintptr_t)resource_bar+0x1d8,(uintptr_t)"FEEDBACK_PANEL");
    wrss((uintptr_t)resource_bar+0x210,400);wrss((uintptr_t)resource_bar+0x214,52);wrss((uintptr_t)resource_bar+0xd0,-200);
    scale=200;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)resource_bar+0xc0)==320&&rdss((uintptr_t)resource_bar+0xc4)==154);
    wr64((uintptr_t)resource_bar+0x1d8,(uintptr_t)"EXT_SELECTION_PANEL");
    wrss((uintptr_t)resource_bar+0x210,342);wrss((uintptr_t)resource_bar+0x214,596);wrss((uintptr_t)resource_bar+0xd0,0);
    fp_mobile_ui_before(&cpu);float selected_scale=rdss((uintptr_t)resource_bar+0xf0);
    assert(fabsf(rdss((uintptr_t)resource_bar+0xc4)-12)<.001f);
    assert(fabsf(rdss((uintptr_t)resource_bar+0xc0)+342*selected_scale-628)<.001f);
    float anchored[4]={-334,178,0,1};animation.rcx=(uintptr_t)resource_bar;animation.rdx=(uintptr_t)anchored;
    fp_mobile_ui_position(&animation,position_original);
    assert(fabsf(rdss((uintptr_t)resource_bar+0xc4)+596*selected_scale-292)<.001f);
    _Alignas(16) unsigned char selected_body[0x300]={0},selected_header[0x280]={0},selected_tabs[0x280]={0};
    _Alignas(16) unsigned char selected_ani[0x280]={0},selected_background[0x280]={0},selected_slice[0x280]={0};
    wr64((uintptr_t)resource_bar+0x220,(uintptr_t)selected_ani);wr64((uintptr_t)selected_ani+0x230,(uintptr_t)resource_bar);
    wr64((uintptr_t)selected_ani+0x1d8,(uintptr_t)"ANI");wrss((uintptr_t)selected_ani+0xf0,1);wrss((uintptr_t)selected_ani+0xf4,1);
    wr64((uintptr_t)selected_ani+0x220,(uintptr_t)selected_body);wr64((uintptr_t)selected_body+0x230,(uintptr_t)selected_ani);
    wr64((uintptr_t)selected_body+0x240,(uintptr_t)selected_background);wr64((uintptr_t)selected_background+0x230,(uintptr_t)selected_ani);
    wr64((uintptr_t)selected_background+0x1d8,(uintptr_t)"Background");
    wrss((uintptr_t)selected_background+0x210,360);wrss((uintptr_t)selected_background+0x214,538);
    wrss((uintptr_t)selected_background+0xc0,-23);wrss((uintptr_t)selected_background+0xc4,62);
    wrss((uintptr_t)selected_background+0xf0,1);wrss((uintptr_t)selected_background+0xf4,1);
    wr64((uintptr_t)selected_background+0x220,(uintptr_t)selected_slice);wr64((uintptr_t)selected_slice+0x230,(uintptr_t)selected_background);
    wr64((uintptr_t)selected_slice+0x1d8,(uintptr_t)"top_left");wrss((uintptr_t)selected_slice+0x210,63);wrss((uintptr_t)selected_slice+0x214,56);
    wr64((uintptr_t)selected_body+0x1d8,(uintptr_t)"WORKPLACE_SUBPANEL");wr64((uintptr_t)selected_body+0x220,(uintptr_t)selected_header);
    wr64((uintptr_t)selected_body,GVA(0x141de0a48));wrss((uintptr_t)selected_body+0x100,331);wrss((uintptr_t)selected_body+0x104,480);
    wrss((uintptr_t)selected_body+0x210,331);wrss((uintptr_t)selected_body+0x214,242);wrss((uintptr_t)selected_body+0x294,5);
    wrss((uintptr_t)selected_body+0xf0,1);wrss((uintptr_t)selected_body+0xf4,1);
    wr64((uintptr_t)selected_header+0x230,(uintptr_t)selected_body);wr64((uintptr_t)selected_header+0x240,(uintptr_t)selected_tabs);
    wr64((uintptr_t)selected_header+0x1d8,(uintptr_t)"EXT_BUILDING_HEADER");wrss((uintptr_t)selected_header+0x104,225);
    wrss((uintptr_t)selected_header+0xf0,1);wrss((uintptr_t)selected_header+0xf4,1);
    wrss((uintptr_t)selected_header+0x100,331);wr32((uintptr_t)selected_header+0x13c,6);
    wr64((uintptr_t)selected_tabs+0x230,(uintptr_t)selected_body);wrss((uintptr_t)selected_tabs+0xc4,230);
    wrss((uintptr_t)selected_tabs+0x104,250);wrss((uintptr_t)selected_tabs+0xf0,1);wrss((uintptr_t)selected_tabs+0xf4,1);
    fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)selected_tabs+0xc0)==0&&rdss((uintptr_t)selected_tabs+0xc4)==230); /* Native flow owns positions; tested separately with real original bodies. */
    assert(rdss((uintptr_t)selected_body+0x280)==342&&rd8((uintptr_t)selected_body+0x2a9)==1&&layout_relayout_calls==1);
    assert((rd32((uintptr_t)selected_header+0x1e0)&4)&&!rd32((uintptr_t)selected_header+0x13c));
    assert(rdss((uintptr_t)resource_bar+0x210)==684&&rdss((uintptr_t)resource_bar+0x214)==302);
    assert(rdss((uintptr_t)selected_background+0x210)==702&&rdss((uintptr_t)selected_background+0x214)==244);
    assert(rdss((uintptr_t)selected_background+0xc0)==-23&&rdss((uintptr_t)selected_background+0xc4)==62);
    assert(rdss((uintptr_t)selected_slice+0x210)==63&&rdss((uintptr_t)selected_slice+0x214)==56);
    assert(rdss((uintptr_t)resource_bar+0xf0)>.75f);
    float compact_top=rdss((uintptr_t)resource_bar+0xc4),compact_scale=rdss((uintptr_t)resource_bar+0xf4);
    assert(compact_top>=55.99f&&compact_top+302*compact_scale<=284.001f);
    fp_mobile_ui_position(&animation,position_original);
    assert(fabsf(rdss((uintptr_t)resource_bar+0xc4)+302*compact_scale-284)<.001f);
    fp_mobile_ui_before(&cpu);assert(layout_relayout_calls==1);
    scale=100;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)selected_tabs+0xc0)==0&&rdss((uintptr_t)selected_tabs+0xc4)==230);
    assert(!(rd32((uintptr_t)selected_header+0x1e0)&4)&&rd32((uintptr_t)selected_header+0x13c)==6&&rdss((uintptr_t)selected_body+0x280)==0&&layout_relayout_calls==2);
    assert(rdss((uintptr_t)resource_bar+0x210)==342&&rdss((uintptr_t)resource_bar+0x214)==596);
    assert(rdss((uintptr_t)selected_background+0x210)==360&&rdss((uintptr_t)selected_background+0x214)==538);
    scale=200;fp_mobile_ui_before(&cpu);
    _Alignas(16) unsigned char technology[0x280]={0},tech_ani[0x280]={0},tech_background[0x280]={0},tech_header[0x280]={0};
    wr64((uintptr_t)frame+0x220,(uintptr_t)technology);wr64((uintptr_t)technology+0x1d8,(uintptr_t)"EXT_TECHNOLOGY_PANEL");
    wrss((uintptr_t)technology+0x210,900);wrss((uintptr_t)technology+0x214,600);
    wrss((uintptr_t)technology+0xd0,-450);wrss((uintptr_t)technology+0xd4,-300);
    wr64((uintptr_t)technology+0x220,(uintptr_t)tech_ani);wr64((uintptr_t)tech_ani+0x230,(uintptr_t)technology);
    wrss((uintptr_t)tech_ani+0xf0,1);wrss((uintptr_t)tech_ani+0xf4,1);
    wrss((uintptr_t)tech_ani+0xc0,-7);wrss((uintptr_t)tech_ani+0xc4,-10.5f);
    wr64((uintptr_t)tech_ani+0x220,(uintptr_t)tech_background);wr64((uintptr_t)tech_background+0x240,(uintptr_t)tech_header);
    wr64((uintptr_t)tech_background+0x230,(uintptr_t)tech_ani);wr64((uintptr_t)tech_header+0x230,(uintptr_t)tech_ani);
    wr64((uintptr_t)tech_background+0x1d8,(uintptr_t)"Background");wrss((uintptr_t)tech_background+0xc0,-178);
    wrss((uintptr_t)tech_background+0xc4,68);wrss((uintptr_t)tech_background+0x100,1250);wrss((uintptr_t)tech_background+0x104,583);
    wr64((uintptr_t)tech_header+0x1d8,(uintptr_t)"CITY_RESOURCES_LAYOUT");wrss((uintptr_t)tech_header+0xc0,-136);
    wrss((uintptr_t)tech_header+0xc4,-40);wrss((uintptr_t)tech_header+0x100,473);wrss((uintptr_t)tech_header+0x104,60);
    wrss((uintptr_t)tech_background+0xf0,1);wrss((uintptr_t)tech_background+0xf4,1);
    wrss((uintptr_t)tech_header+0xf0,1);wrss((uintptr_t)tech_header+0xf4,1);
    for(unsigned ui=100;ui<=200;ui+=50){
        scale=ui;fp_mobile_ui_before(&cpu);float s=rdss((uintptr_t)technology+0xf0),x=rdss((uintptr_t)technology+0xc0)-450*s,y=rdss((uintptr_t)technology+0xc4)-300*s;
        if(ui==100){assert(x-185*s>=11.9f&&x+1065*s<=rdss(GVA(0x142980e60))-11.9f);
            assert(y-50.5f*s>=11.9f&&y+640.5f*s<=rdss(GVA(0x142980e64))-11.9f);}
        else assert(s==1&&fabsf(x)<.01f&&fabsf(y)<.01f&&rdss((uintptr_t)technology+0x210)==rdss(GVA(0x142980e60)));
        fp_mobile_ui_before(&cpu);assert(fabsf(rdss((uintptr_t)technology+0xf0)-s)<.0001f);
    }
    _Alignas(16) unsigned char event[0x280]={0},event_mask[0x400]={0};
    wr64((uintptr_t)frame+0x220,(uintptr_t)event);wr64((uintptr_t)event+0x220,(uintptr_t)event_mask);
    wr64((uintptr_t)event+0x1d8,(uintptr_t)"EXT_DILEMMA_SMALL_PANEL");
    wr64((uintptr_t)event_mask,GVA(0x141D796F8));wr64((uintptr_t)event_mask+0x230,(uintptr_t)event);
    scale=200;fp_mobile_ui_before(&cpu);
    assert(fabsf(rdss((uintptr_t)event_mask+0xc0)+170.4f)<.01f&&rdss((uintptr_t)event_mask+0xc4)==-173);
    scale=100;fp_mobile_ui_before(&cpu);
    assert(fabsf(rdss((uintptr_t)event_mask+0xc0)-149.6f)<.01f&&rdss((uintptr_t)event_mask+0xc4)==7);
    cpu.rcx=(uintptr_t)system;
    _Alignas(16) unsigned char main_menu[0x280]={0},ink[0x280]={0},ink_mask[0x280]={0},menu_items[0x280]={0};
    wr64((uintptr_t)frame+0x220,(uintptr_t)main_menu);wr64((uintptr_t)main_menu+0x1d8,(uintptr_t)"EXT_MAIN_MENU_PANEL");
    wr64((uintptr_t)main_menu+0x220,(uintptr_t)ink);wr64((uintptr_t)ink+0x230,(uintptr_t)main_menu);
    wrss((uintptr_t)main_menu+0x210,1280);wrss((uintptr_t)main_menu+0x214,720);
    wr64((uintptr_t)ink+0x1d8,(uintptr_t)"INK_PANEL");wr32((uintptr_t)ink+0x138,5);
    wrss((uintptr_t)ink+0x210,500);wrss((uintptr_t)ink+0x214,500);wrss((uintptr_t)ink+0xf0,1);wrss((uintptr_t)ink+0xf4,1);
    wrss((uintptr_t)ink+0xc0,250);wrss((uintptr_t)ink+0xc4,250);wrss((uintptr_t)ink+0xd0,-250);wrss((uintptr_t)ink+0xd4,-250);
    wr64((uintptr_t)ink+0x220,(uintptr_t)ink_mask);wr64((uintptr_t)ink_mask+0x230,(uintptr_t)ink);
    wr64((uintptr_t)ink_mask+0x220,(uintptr_t)menu_items);wr64((uintptr_t)menu_items+0x230,(uintptr_t)ink_mask);
    wr64((uintptr_t)menu_items+0x1d8,(uintptr_t)"MENU_LAYOUT");wrss((uintptr_t)menu_items+0xf0,.95f);wrss((uintptr_t)menu_items+0xf4,.95f);
    cpu.rcx=(uintptr_t)screen;scale=200;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)ink+0xf0)>1.8f&&rdss((uintptr_t)menu_items+0xf0)==.95f);
    assert(rd32((uintptr_t)ink+0x138)==0&&rdss((uintptr_t)ink+0x210)==1280);
    scale=100;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)ink+0xf0)==1&&rd32((uintptr_t)ink+0x138)==5&&rdss((uintptr_t)ink+0x210)==500);
    assert(rdss((uintptr_t)ink+0xc0)==250&&rdss((uintptr_t)ink+0xc4)==250);
    wr64((uintptr_t)main_menu+0x1d8,(uintptr_t)"EXT_INGAME_MENU_MAIN_PANEL");
    scale=200;fp_mobile_ui_before(&cpu);
    assert(rdss((uintptr_t)ink+0xf0)>1.87f&&rdss((uintptr_t)menu_items+0xf0)==.95f);
    assert(fabsf(rdss((uintptr_t)ink+0xc4)-(360-(362-250)*(656.f/350)))<.01f);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss((uintptr_t)ink+0xc4)==250);
    const char *dialog_names[]={"EXT_SETTINGS_PANEL","EXT_INGAME_MENU_SETTINGS_PANEL","EXT_LOADGAME_PANEL","EXT_INGAME_MENU_LOADGAME_PANEL","EXT_INGAME_MENU_SAVEGAME_PANEL"};
    const float dialog_centers[][2]={{310.05f,336.5f},{637.55f,326.5f},{304.55f,327},{637.55f,327},{643.55f,332}};
    for(unsigned i=0;i<5;++i){
        wr64((uintptr_t)main_menu+0x1d8,(uintptr_t)dialog_names[i]);
        scale=200;fp_mobile_ui_before(&cpu);float factor=rdss((uintptr_t)ink+0xf0);
        assert(fabsf(factor-672.f/449)<.0001f);
        assert(fabsf(rdss((uintptr_t)ink+0xc0)-(640-(dialog_centers[i][0]-250)*factor))<.01f);
        assert(fabsf(rdss((uintptr_t)ink+0xc4)-(360-(dialog_centers[i][1]-250)*factor))<.01f);
        assert(rdss((uintptr_t)menu_items+0xf0)==.95f&&rdss((uintptr_t)ink_mask+0x210)==0);
        scale=100;fp_mobile_ui_before(&cpu);
        assert(rdss((uintptr_t)ink+0xc0)==250&&rdss((uintptr_t)ink+0xc4)==250&&rd32((uintptr_t)ink+0x138)==5);
    }
    // Main/pause dialogs retain the PC canvas (applied_scale=100) while their complete
    // ink composition is enlarged. Its independent paint camera must use that exact ratio.
    fp_mobile_camera_destroy();
    const char *paint_dialogs[]={"EXT_MAIN_MENU_PANEL","EXT_INGAME_MENU_MAIN_PANEL","EXT_SETTINGS_PANEL",
        "EXT_INGAME_MENU_SETTINGS_PANEL","EXT_LOADGAME_PANEL","EXT_INGAME_MENU_LOADGAME_PANEL","EXT_INGAME_MENU_SAVEGAME_PANEL"};
    wr64((uintptr_t)mask+0x230,(uintptr_t)ink);wr64((uintptr_t)mask+0x1d8,(uintptr_t)"SFX_TXT_MASK");
    wrss((uintptr_t)mask+0xf0,.65f);wrss((uintptr_t)mask+0xf4,.65f);
    rendering.rcx=(uintptr_t)mask;
    for(unsigned i=0;i<7;++i){
        wr64((uintptr_t)main_menu+0x1d8,(uintptr_t)paint_dialogs[i]);
        scale=200;fp_mobile_ui_before(&cpu);
        assert(rdss(GVA(0x142980e60))==1280&&rdss(GVA(0x142980e64))==720);
        float factor=rdss((uintptr_t)ink+0xf0);assert(factor>1.4f);
        float paint_matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};rendering.rdx=(uintptr_t)paint_matrix;
        wr8((uintptr_t)mask+0x383,1);expected_template_width=1280*factor;expected_template_height=720*factor;
        wr8((uintptr_t)mask+0x385,1);wrss((uintptr_t)mask+0x378,75);
        expected_projection=75;
        fp_mobile_ui_template(&rendering,template_original);assert(rdss((uintptr_t)mask+0x378)==75);
        wr8((uintptr_t)mask+0x385,0);wrss((uintptr_t)mask+0x37c,10);expected_projection=10*factor;
        fp_mobile_ui_template(&rendering,template_original);assert(rdss((uintptr_t)mask+0x37c)==10);
        // A rectangular mask must scale its viewport as well as its camera. The native
        // perspective path gets its zoom from viewportHeight/targetHeight, so its FOV stays 75.
        wr8((uintptr_t)mask+0x383,0);wr8((uintptr_t)mask+0x385,1);
        wrss((uintptr_t)mask+0x100,1280);wrss((uintptr_t)mask+0x104,720);
        expected_projection=75;expected_template_width=1280*factor;expected_template_height=720*factor;
        fp_mobile_ui_template(&rendering,template_original);
        assert(rdss((uintptr_t)mask+0x378)==75&&rdss((uintptr_t)mask+0x100)==1280&&rdss((uintptr_t)mask+0x104)==720);
        wr8((uintptr_t)mask+0x385,0);expected_projection=10*factor;
        fp_mobile_ui_template(&rendering,template_original);
        assert(rdss((uintptr_t)mask+0x37c)==10&&rdss((uintptr_t)mask+0x100)==1280&&rdss((uintptr_t)mask+0x104)==720);
        scale=100;fp_mobile_ui_before(&cpu);expected_projection=10;
        expected_template_width=1280;expected_template_height=720;
        fp_mobile_ui_template(&rendering,template_original);assert(rdss((uintptr_t)mask+0x37c)==10);
    }
    // An identically named wrapper in another feature does not opt into menu scaling.
    wr64((uintptr_t)main_menu+0x1d8,(uintptr_t)"EXT_TECHNOLOGY_PANEL");
    wrss((uintptr_t)ink+0xf0,1.8f);wrss((uintptr_t)ink+0xf4,1.8f);expected_projection=10;
    fp_mobile_ui_template(&rendering,template_original);assert(rdss((uintptr_t)mask+0x37c)==10);
    expected_template_width=expected_template_height=0;
    wr64((uintptr_t)mask+0x230,0);wr64((uintptr_t)main_menu+0x1d8,(uintptr_t)"EXT_INGAME_MENU_SAVEGAME_PANEL");
    wrss((uintptr_t)ink+0xf0,1);wrss((uintptr_t)ink+0xf4,1);
    law_selected_fixture(&cpu,(uintptr_t)screen,(uintptr_t)frame);
    check_mobile_dialog_popups(&cpu);
    check_mobile_dialog_affine(&cpu);
    check_mobile_editor_hits(&cpu);
    check_mobile_dlc_glow(&cpu);
    check_mobile_confirmation(&cpu);
    check_mobile_photo_mask(&cpu);
    cpu.rcx=(uintptr_t)system;
    wr64((uintptr_t)system+0xa0,(uintptr_t)controller);wrss((uintptr_t)controller+0x30,100);
    wrss((uintptr_t)controller+0x3c,1);wrss((uintptr_t)controller+0x2c,1);
    fp_mobile_camera_before(&cpu);assert(frostpunk_mobile_context(.5f,.5f)&1);assert(frostpunk_dxvk_scene_active());
    assert(!frostpunk_mobile_submit(6,0,0,0,0)&&!frostpunk_mobile_submit(5,0,0,0,0)&&!frostpunk_mobile_submit(4,.5f,0,0,0)&&!frostpunk_mobile_submit(0,NAN,0,0,0));
    assert(!frostpunk_mobile_submit(0,0,0,INFINITY,0)&&!frostpunk_mobile_context(-1,0));
    _Alignas(16) unsigned char time_system[0x100]={0},clock[0x100]={0},clockpanel[0x280]={0},clockbutton[0x3c0]={0},hit[0x50]={0};
    uint64_t controls[2]={(uintptr_t)clockbutton,0};
    wr64(GVA(0x142b8d800),(uintptr_t)time_system);wr64((uintptr_t)time_system,GVA(0x141e049b0));
    wr64(GVA(0x142b8b390),(uintptr_t)clock);
    for(unsigned i=0;i<3;++i)wrss(GVA(0x142b93b74)+i*4,(float[]){1,3,12}[i]);
    wr64(GVA(0x142a89088),(uintptr_t)controls);wr32(GVA(0x142a89090),1);
    wr64((uintptr_t)clockbutton+0x1d8,(uintptr_t)"BUTTON_FAST");wr64((uintptr_t)clockbutton+0x230,(uintptr_t)clockpanel);
    wr64((uintptr_t)clockpanel+0x1d8,(uintptr_t)"EXT_TIME_PANEL");wr32((uintptr_t)clockbutton+0x1e4,500);
    wr64((uintptr_t)clockbutton+0x248,(uintptr_t)screen);wr64((uintptr_t)screen,GVA(0x141d7b0b0));
    wr64((uintptr_t)screen+0x2f0,(uintptr_t)hit);wr32((uintptr_t)screen+0x2f8,1);wr32((uintptr_t)hit+0x40,500);
    for(unsigned i=0;i<4;++i){wrss((uintptr_t)hit+i*16,(i&1)?-.2f:-.6f);wrss((uintptr_t)hit+i*16+4,(i&2)?.6f:.8f);}
    for(unsigned selection=0;selection<4;++selection){
        assert(frostpunk_mobile_submit(4,(float)selection,0,0,0));fp_mobile_camera_before(&cpu);fp_mobile_camera_before(&cpu);
        assert(((frostpunk_mobile_context(.3f,.15f)>>9)&3)==selection);
        assert((frostpunk_mobile_context(.3f,.15f)&(256|8|2))==(256|8|2));
        assert(!(frostpunk_mobile_context(.8f,.8f)&8));
    }
    assert(speed_calls==3&&pause_calls==4);
    frostpunk_mobile_submit(4,1,0,0,0);frostpunk_mobile_submit(4,3,0,0,0);fp_mobile_camera_before(&cpu);
    assert(speed_calls==4&&rdss((uintptr_t)clock+0xd0)==12);
    frostpunk_mobile_submit(4,0,0,0,0);frostpunk_mobile_submit(2,0,0,0,0);fp_mobile_camera_before(&cpu);
    assert(pause_calls==5&&rd32((uintptr_t)clock+0xc0)==0);
    wr32((uintptr_t)hit+0x40,600);wr32((uintptr_t)hit+0x44,0x300211);
    cpu.rcx=(uintptr_t)screen;fp_mobile_ui_before(&cpu);cpu.rcx=(uintptr_t)system;
    assert((frostpunk_mobile_context(.3f,.15f)&2)&&!(frostpunk_mobile_context(.3f,.15f)&8));
    _Alignas(16) unsigned char panel_manager[0x900]={0},panel_vtable[0x180]={0};
    wr64((uintptr_t)panel_manager,(uintptr_t)panel_vtable);
    wr64((uintptr_t)panel_vtable+0x170,GVA(0x1419ed000));
    cpu.rcx=(uintptr_t)panel_manager;fp_mobile_panels_created(&cpu,created_original);
    wr64((uintptr_t)clockbutton+0x1d8,(uintptr_t)"EXT_INGAME_MENU_SETTINGS_PANEL");
    cpu.rcx=(uintptr_t)screen;scale=200;fp_mobile_ui_before(&cpu);
    fp_mobile_debug_menu_request();fp_mobile_ui_before(&cpu);assert(!debug_calls);
    assert(rdss(GVA(0x142980e60))==1280&&scale==200);
    for(unsigned requested=100;requested<=200;requested+=50){
        scale=requested;fp_mobile_ui_before(&cpu);
        assert(rdss(GVA(0x142980e60))==1280&&rdss(GVA(0x142980e64))==720&&scale==requested);
    }
    wr32((uintptr_t)clockpanel+0x1e0,2);fp_mobile_ui_before(&cpu);fp_mobile_ui_before(&cpu);test_milliseconds+=199;fp_mobile_ui_before(&cpu);assert(!debug_calls);
    assert(rdss(GVA(0x142980e60))==640&&scale==200);
    ++test_milliseconds;fp_mobile_ui_before(&cpu);assert(debug_calls==1);
    fp_mobile_ui_before(&cpu);assert(debug_calls==1);wr32((uintptr_t)clockpanel+0x1e0,0);cpu.rcx=(uintptr_t)system;
    wr32((uintptr_t)hit+0x44,0x300213);fp_mobile_camera_before(&cpu);assert(!(frostpunk_mobile_context(.3f,.15f)&2));
    wr64((uintptr_t)clockbutton,GVA(0x141DA74C8));wr32((uintptr_t)hit+0x40,500);wr32((uintptr_t)hit+0x44,0x300211);
    wr64((uintptr_t)clockbutton+0x1d8,(uintptr_t)"SOCIOTECHTREE");
    wr32((uintptr_t)clockbutton+0x370,1);
    for(unsigned i=0;i<4;++i){wrss((uintptr_t)hit+i*16,(i&1)?1:-1);wrss((uintptr_t)hit+i*16+4,(i&2)?-.9f:.9f);}
    fp_mobile_camera_before(&cpu);assert((frostpunk_mobile_context(.8f,.8f)&4098)==4098);
    wr64((uintptr_t)clockpanel+0x1d8,(uintptr_t)"EXT_CONSTRUCTION_PANEL");
    wr64((uintptr_t)clockbutton+0x1d8,(uintptr_t)"BUILDING_SCROLL_PANE");wr32((uintptr_t)clockbutton+0x370,0);
    fp_mobile_camera_before(&cpu);assert((frostpunk_mobile_context(.8f,.8f)&(32|16384|4098))==(32|16384|4098));
    assert(rd32((uintptr_t)clockbutton+0x370)==1);
    wr64((uintptr_t)clockpanel+0x1d8,(uintptr_t)"TIME_PANEL");
    wr64((uintptr_t)clockbutton+0x1d8,(uintptr_t)"SOCIOTECHTREE");
    for(unsigned i=0;i<10;++i)assert(frostpunk_mobile_submit(0,.01f,0,0,.01f));
    fp_mobile_camera_before(&cpu);assert(pan_calls==1&&turn_calls==1&&fabsf(pan_distance-14)<.001f&&fabsf(turn_angle+5.729578f)<.001f);
    frostpunk_mobile_submit(1,0,0,0,0);fp_mobile_camera_before(&cpu);
    fp_mobile_camera_before(&cpu);assert(pan_calls==1&&turn_calls==1);
    assert(!frostpunk_mobile_submit(0,0,0,logf(2),0));fp_mobile_camera_before(&cpu);assert(rdss((uintptr_t)controller+0x30)==100);
    frostpunk_mobile_submit(1,0,0,0,0);fp_mobile_camera_before(&cpu);assert(rdss((uintptr_t)controller+0x30)==100&&rdss((uintptr_t)controller+0x2c)==1);
    frostpunk_mobile_submit(0,1,1,1,1);frostpunk_mobile_submit(2,0,0,0,0);fp_mobile_camera_before(&cpu);assert(pan_calls==1&&turn_calls==1);
    _Alignas(16) unsigned char placement[0x21e0]={0},entity[0x200]={0};uint64_t entry=(uintptr_t)entity;
    wr64(GVA(0x143ff63e8),(uintptr_t)placement);wr64((uintptr_t)placement,GVA(0x142014120));
    wr64((uintptr_t)placement+0x2060,(uintptr_t)&entry);wr32((uintptr_t)placement+0x2068,1);
    wrss((uintptr_t)entity+0x138,1);wrss((uintptr_t)entity+0x140,1);wrss((uintptr_t)entity+0x144,2);wrss((uintptr_t)entity+0x148,3);
    _Alignas(16) unsigned char manager[0x100]={0},painter[0x100]={0};
    wr64((uintptr_t)manager+0x80,(uintptr_t)painter);wr64((uintptr_t)painter,GVA(0x1421381E0));
    cpu.rcx=(uintptr_t)manager;fp_mobile_painters_created(&cpu,created_original);cpu.rcx=(uintptr_t)system;
    frostpunk_mobile_submit(3,0,0,0,.36f);fp_mobile_camera_before(&cpu);
    assert((frostpunk_mobile_context(.5f,.5f)&4)&&building_calls==1&&turn_calls==1);
    frostpunk_mobile_submit(3,0,0,0,-.36f);fp_mobile_camera_before(&cpu);
    assert(building_calls==4&&turn_calls==1);
    wr32((uintptr_t)system+0x1f0,1);frostpunk_mobile_submit(0,1,1,1,1);fp_mobile_camera_before(&cpu);assert(!(frostpunk_mobile_context(.5f,.5f)&1)&&pan_calls==1);
    assert(frostpunk_dxvk_scene_active());
    _Alignas(16) unsigned char selector[0x300]={0},model[0x70]={0},slider[0x280]={0},left_arrow[0x280]={0},right_arrow[0x280]={0};
    uint64_t selectors[8]={(uintptr_t)left_arrow,0,(uintptr_t)right_arrow,0};
    unsigned char *crowded_hits=calloc(516,0x50);assert(crowded_hits);
    wr64((uintptr_t)selector+0x1d8,(uintptr_t)"SETTINGS_SELECTOR");wr32((uintptr_t)selector+0x1e4,800);
    wr64((uintptr_t)selector+0x200,(uintptr_t)model);wr32((uintptr_t)model+0x30,11);wr32((uintptr_t)model+0x40,5);
    wr32((uintptr_t)selector+0x2a8,11);wr8((uintptr_t)selector+0x2d0,1);wr32((uintptr_t)selector+0x2d4,5);
    wr64((uintptr_t)slider+0x230,(uintptr_t)selector);
    wrss((uintptr_t)slider+0xc0,256);wrss((uintptr_t)slider+0xc4,162);wrss((uintptr_t)slider+0xf0,1);wrss((uintptr_t)slider+0xf4,1);
    wrss((uintptr_t)slider+0x100,832);wrss((uintptr_t)slider+0x104,36);
    for(unsigned side=0;side<2;++side){uint64_t arrow=side?(uintptr_t)right_arrow:(uintptr_t)left_arrow;
        wr64(arrow+0x1d8,(uintptr_t)(side?"ARROW_RIGHT":"ARROW_LEFT"));wr32(arrow+0x1e4,801+side);
        wr64(arrow+0x230,(uintptr_t)slider);wr64(arrow+0x248,(uintptr_t)screen);
    }
    for(unsigned i=0;i<514;++i){uint64_t at=(uintptr_t)crowded_hits+i*0x50;
        float x=i==513?.6f:-.6f;
        wr32(at+0x40,i>=512?801+i-512:900+i);wr32(at+0x44,0x300211);
        for(unsigned v=0;v<4;++v){wrss(at+v*16,x+((v&1)?.1f:0));wrss(at+v*16+4,(v&2)?.4f:.6f);}
    }
    wr64(GVA(0x142a89088),(uintptr_t)selectors);wr32(GVA(0x142a89090),2);
    wr64((uintptr_t)screen+0x2f0,(uintptr_t)crowded_hits);wr32((uintptr_t)screen+0x2f8,514);
    fp_mobile_camera_before(&cpu);assert((frostpunk_mobile_context(.5f,.25f)&8194)==8194);
    uint64_t target=frostpunk_mobile_swipe_target(.5f,.25f,1);uint32_t bx=target>>32,by=target;
    float tx,ty;memcpy(&tx,&bx,4);memcpy(&ty,&by,4);assert(fabsf(tx-.825f)<.0001f&&fabsf(ty-.25f)<.0001f);
    assert(!frostpunk_mobile_swipe_target(.5f,.25f,0)&&!frostpunk_mobile_swipe_target(NAN,.25f,1));
    assert(frostpunk_mobile_swipe_target(.19f,.25f,2)&&((frostpunk_mobile_context(.19f,.25f)&8194)==8194));
    wr64((uintptr_t)selector+0x1d8,(uintptr_t)"SCENARIO_SETTING_SELECTOR");fp_mobile_camera_before(&cpu);
    assert(frostpunk_mobile_swipe_target(.5f,.25f,1));
    wr64((uintptr_t)selector+0x1d8,(uintptr_t)"SETTINGS_LANGUAGE_SELECTOR");fp_mobile_camera_before(&cpu);
    assert(frostpunk_mobile_swipe_target(.5f,.25f,1)&&frostpunk_mobile_swipe_target(.19f,.25f,2));
    /* A closed dialog can leave enabled native arrow quads until the next hit-list rebuild. */
    CPU selector_cpu=cpu;
    const char *selector_names[]={"SETTINGS_SELECTOR","SETTINGS_SELECTOR_ADDITIONAL","SETTINGS_LANGUAGE_SELECTOR","SCENARIO_SETTING_SELECTOR"};
    for(unsigned kind=0;kind<4;++kind){
        wr64((uintptr_t)selector+0x1d8,(uintptr_t)selector_names[kind]);
        for(unsigned limit=0;limit<2;++limit){uint64_t hidden=limit?(uintptr_t)left_arrow:(uintptr_t)right_arrow;
            wr32(hidden+0x1e0,2);fp_mobile_camera_before(&cpu);
            assert((frostpunk_mobile_context(.5f,.25f)&8192)&&frostpunk_mobile_swipe_target(.5f,.25f,limit?1:-1));
            assert(!frostpunk_mobile_swipe_target(.5f,.25f,limit?-1:1)&&!frostpunk_mobile_swipe_target(.5f,.25f,2));
            wr32(hidden+0x1e0,0);fp_mobile_camera_before(&cpu);
            assert(frostpunk_mobile_swipe_target(.5f,.25f,-1)&&frostpunk_mobile_swipe_target(.5f,.25f,1));
        }
        for(unsigned parent=0;parent<2;++parent){uint64_t hidden=parent?(uintptr_t)slider:(uintptr_t)selector;
            wr32(hidden+0x1e0,2);fp_mobile_camera_before(&cpu);
            assert(!(frostpunk_mobile_context(.5f,.25f)&8192));
            assert(!frostpunk_mobile_swipe_target(.5f,.25f,-1)&&!frostpunk_mobile_swipe_target(.5f,.25f,1));
            assert(!frostpunk_mobile_swipe_target(.19f,.25f,2));
            wr32(hidden+0x1e0,0);fp_mobile_camera_before(&cpu);
            assert((frostpunk_mobile_context(.5f,.25f)&8192)&&frostpunk_mobile_swipe_target(.5f,.25f,-1)
                &&frostpunk_mobile_swipe_target(.5f,.25f,1)&&frostpunk_mobile_swipe_target(.19f,.25f,2));
        }
        const unsigned limits[][3]={{11,0,2},{11,10,1},{11,5,3},{1,0,0},{0,0,0},{UINT32_MAX,0,0},{11,11,0},{11,UINT32_MAX,0}};
        for(unsigned i=0;i<sizeof(limits)/sizeof(limits[0]);++i){
            wr32((uintptr_t)model+0x30,limits[i][0]);wr32((uintptr_t)model+0x40,limits[i][1]);
            wr32((uintptr_t)selector+0x2a8,limits[i][0]);wr32((uintptr_t)selector+0x2d4,limits[i][1]);
            fp_mobile_camera_before(&cpu);
            assert(frostpunk_mobile_context(.5f,.25f)&8192); /* Keep axis lock and reversal at either limit. */
            assert(!!frostpunk_mobile_swipe_target(.5f,.25f,-1)==!!(limits[i][2]&1));
            assert(!!frostpunk_mobile_swipe_target(.5f,.25f,1)==!!(limits[i][2]&2));
            assert(frostpunk_mobile_swipe_target(.19f,.25f,2)&&frostpunk_mobile_swipe_target(.86f,.25f,2)); /* Taps retain native wrapping. */
        }
        wr32((uintptr_t)model+0x30,11);wr32((uintptr_t)model+0x40,5);
        wr32((uintptr_t)selector+0x2a8,11);wr32((uintptr_t)selector+0x2d4,5);
        wr64((uintptr_t)selector+0x200,0);
        if(kind==3){wr8((uintptr_t)selector+0x2d0,0);fp_mobile_camera_before(&cpu);
            assert(!frostpunk_mobile_swipe_target(.5f,.25f,-1)&&!frostpunk_mobile_swipe_target(.5f,.25f,1));
            wr8((uintptr_t)selector+0x2d0,1);wr64((uintptr_t)selector+0x200,1);fp_mobile_camera_before(&cpu);
            assert(frostpunk_mobile_swipe_target(.5f,.25f,-1)&&frostpunk_mobile_swipe_target(.5f,.25f,1)); /* Cyclic widget never dereferences an ordinary-setting model. */
        }else{fp_mobile_camera_before(&cpu);assert(!frostpunk_mobile_swipe_target(.5f,.25f,-1)&&!frostpunk_mobile_swipe_target(.5f,.25f,1));}
        wr64((uintptr_t)selector+0x200,(uintptr_t)model);
    }
    /* Padded rows overlap: a clamped nearest row must not fall through to its neighbor. */
    _Alignas(16) unsigned char neighbor[0x300]={0},neighbor_model[0x70]={0},neighbor_slider[0x280]={0},neighbor_arrows[2][0x280]={0};
    wr64((uintptr_t)selector+0x1d8,(uintptr_t)"SETTINGS_SELECTOR");
    wr64((uintptr_t)neighbor+0x1d8,(uintptr_t)"SETTINGS_SELECTOR");wr32((uintptr_t)neighbor+0x1e4,1800);
    wr64((uintptr_t)neighbor+0x200,(uintptr_t)neighbor_model);wr32((uintptr_t)neighbor_model+0x30,11);wr32((uintptr_t)neighbor_model+0x40,5);
    memcpy(neighbor_slider,slider,sizeof(slider));wr64((uintptr_t)neighbor_slider+0x230,(uintptr_t)neighbor);wrss((uintptr_t)neighbor_slider+0xc4,190.8f);
    for(unsigned side=0;side<2;++side){uint64_t arrow=(uintptr_t)neighbor_arrows[side],at=(uintptr_t)crowded_hits+(514+side)*0x50;
        wr64(arrow+0x1d8,(uintptr_t)(side?"ARROW_RIGHT":"ARROW_LEFT"));wr32(arrow+0x1e4,1801+side);
        wr64(arrow+0x230,(uintptr_t)neighbor_slider);wr64(arrow+0x248,(uintptr_t)screen);selectors[4+side*2]=arrow;
        wr32(at+0x40,1801+side);wr32(at+0x44,0x300211);
        for(unsigned v=0;v<4;++v){
            wrss(at+v*16,(side?.6f:-.6f)+((v&1)?.1f:0));wrss(at+v*16+4,(v&2)?.40f:.44f);
            wrss((uintptr_t)crowded_hits+(512+side)*0x50+v*16+4,(v&2)?.48f:.52f);
        }
    }
    wr32(GVA(0x142a89090),4);wr32((uintptr_t)screen+0x2f8,516);
    wr32((uintptr_t)model+0x40,0);fp_mobile_camera_before(&cpu);
    assert((frostpunk_mobile_context(.5f,.269f)&8192)&&!frostpunk_mobile_swipe_target(.5f,.269f,-1));
    assert(frostpunk_mobile_swipe_target(.5f,.269f,1)&&frostpunk_mobile_swipe_target(.225f,.269f,2));
    wr32((uintptr_t)model+0x40,10);fp_mobile_camera_before(&cpu);
    assert(!frostpunk_mobile_swipe_target(.5f,.269f,1)&&frostpunk_mobile_swipe_target(.5f,.269f,-1));
    assert(frostpunk_mobile_swipe_target(.5f,.29f,1)); /* The neighbor remains independently adjustable. */
    wr32((uintptr_t)model+0x40,5);fp_mobile_camera_before(&cpu);
    target=frostpunk_mobile_swipe_target(.5f,.269f,1);by=target;memcpy(&ty,&by,4);assert(fabsf(ty-.25f)<.0001f);
    wr32(GVA(0x142a89090),2);wr32((uintptr_t)screen+0x2f8,514);
    assert(!memcmp(&cpu,&selector_cpu,sizeof(cpu)));
    _Alignas(16) unsigned char difficulty[0x280]={0},handle[0x280]={0};
    wr64((uintptr_t)difficulty+0x1d8,(uintptr_t)"DIFFICULTY_SLIDER");wrss((uintptr_t)difficulty+0xc0,128);wrss((uintptr_t)difficulty+0xc4,72);
    wrss((uintptr_t)difficulty+0x100,256);wrss((uintptr_t)difficulty+0x104,36);wrss((uintptr_t)difficulty+0xf0,1);wrss((uintptr_t)difficulty+0xf4,1);
    wr64((uintptr_t)handle+0x1d8,(uintptr_t)"DRAG_ELEMENT");wr64((uintptr_t)handle+0x230,(uintptr_t)difficulty);wr64((uintptr_t)handle+0x248,(uintptr_t)screen);wr32((uintptr_t)handle+0x1e4,1800);
    selectors[0]=(uintptr_t)handle;wr32(GVA(0x142a89090),1);wr32((uintptr_t)screen+0x2f8,1);
    wr32((uintptr_t)crowded_hits+0x40,1800);wr32((uintptr_t)crowded_hits+0x44,0x300211);
    for(unsigned v=0;v<4;++v){wrss((uintptr_t)crowded_hits+v*16,(v&1)?-.1f:-.2f);wrss((uintptr_t)crowded_hits+v*16+4,(v&2)?.45f:.55f);}
    fp_mobile_camera_before(&cpu);assert((frostpunk_mobile_context(.3f,.25f)&66)==66);
    target=frostpunk_mobile_swipe_target(.3f,.25f,0);bx=target>>32;by=target;memcpy(&tx,&bx,4);memcpy(&ty,&by,4);
    assert(fabsf(tx-.425f)<.0001f&&fabsf(ty-.25f)<.0001f);
    wr32((uintptr_t)difficulty+0x1e0,2);fp_mobile_camera_before(&cpu);assert(!(frostpunk_mobile_context(.3f,.25f)&64)&&!frostpunk_mobile_swipe_target(.3f,.25f,0));
    wr32((uintptr_t)difficulty+0x1e0,0);wr64((uintptr_t)handle,GVA(0x141DA74C8));wr64((uintptr_t)handle+0x1d8,(uintptr_t)"TECHTREE");wr32((uintptr_t)handle+0x370,1);
    wr32((uintptr_t)crowded_hits+0x44,0x4300201);fp_mobile_camera_before(&cpu);
    assert((frostpunk_mobile_context(.425f,.25f)&4098)==4098);
    free(crowded_hits);
    check_switch_panels(&cpu);
    _Alignas(16) unsigned char radial_buttons[5][0x500]={0};
    _Alignas(16) unsigned char radial_art[5][0x300]={0},radial_modal[0x500]={0};
    _Alignas(16) unsigned char radial_hits[5][0x50]={0};
    uint64_t radial_controls[10]={0};
    const char *radial_names[]={"ECONOMY_BUTTON","SEARCH_CITIZENS_BUTTON","SOCIOTECH_BUTTON","TECHNOLOGY_BUTTON","CONSTRUCTION_BUTTON"};
    for(unsigned i=0;i<5;++i){
        uint64_t button=(uintptr_t)radial_buttons[i];radial_controls[i*2]=button;
        wr64(button,GVA(0x141D79F78));wr64(button+0x1d8,(uintptr_t)radial_names[i]);wr32(button+0x1e4,3000+i);
        wr64(button+0x230,(uintptr_t)screen);wr64(button+0x248,(uintptr_t)screen);wr32(button+0x1e0,0x10);
        if(i<4)wr64(button+0x240,(uintptr_t)radial_buttons[i+1]);
        wrss(button+0x100,60);wrss(button+0x104,60);wrss(button+0x210,60);wrss(button+0x214,60);
        wrss(button+0xf0,1);wrss(button+0xf4,1);
        wrss(button+0xc0,150+i*70);wrss(button+0xc4,260);wr32(button+0x13c,3);
        wrss(button+0x110,.2f);wrss(button+0x114,.4f);wrss(button+0x118,.6f);wrss(button+0x11c,1);
        uint64_t art=(uintptr_t)radial_art[i];wr64(button+0x220,art);wr64(art+0x230,button);wr64(art,GVA(0x141DA2AA8));wr64(art+0x1d8,(uintptr_t)"ART");
    }
    wr64((uintptr_t)screen+0x220,(uintptr_t)radial_buttons[0]);wr8((uintptr_t)screen+0x2d5,0);
    wrss((uintptr_t)screen+0xf0,1);wrss((uintptr_t)screen+0xf4,1);
    wr64(GVA(0x142a89088),(uintptr_t)radial_controls);wr32(GVA(0x142a89090),5);wr32((uintptr_t)screen+0x2f8,0);
    wr64((uintptr_t)system+0xa0,(uintptr_t)controller);wr32((uintptr_t)system+0x1f0,0);wrss((uintptr_t)system+0x228,0);wr64((uintptr_t)system+0x230,0);
    wr64(GVA(0x143ff63e8),0);
    cpu.rcx=(uintptr_t)system;fp_mobile_camera_before(&cpu);switch_active=1;
    cpu.rcx=(uintptr_t)screen;scale=200;fp_mobile_ui_before(&cpu);
    assert((frostpunk_mobile_context(.5f,.5f)&(1u<<26))!=0);
    assert(frostpunk_mobile_submit(5,1,0,0,0));CPU before_radial=cpu;fp_mobile_ui_before(&cpu);
    assert(!memcmp(&cpu,&before_radial,sizeof(cpu)));
    assert((frostpunk_mobile_context(.5f,.5f)&((1u<<25)|2))==((1u<<25)|2));
    for(unsigned i=0;i<5;++i){
        uint64_t node=(uintptr_t)radial_buttons[i],hit=(uintptr_t)radial_hits[i];
        assert(rdss(node+0x110)==.2f&&rdss(node+0x11c)==0&&(rd32(node+0x1e0)&0x10));
        assert(rd32((uintptr_t)radial_art[i]+0x1e0)&2);
        if(i!=1)assert(rdss(node+0xc0)==150+i*70&&rdss(node+0xc4)==260&&rdss(node+0xf0)==1);
        float x=.3f+i*.1f,y=.85f;
        for(unsigned v=0;v<4;++v){wrss(hit+v*16,(x+(v&1?.02f:-.02f))*2-1);wrss(hit+v*16+4,1-(y+(v&2?.02f:-.02f))*2);}
        wr32(hit+0x40,3000+i);wr32(hit+0x44,0x10);
    }
    wr64((uintptr_t)screen+0x2f0,(uintptr_t)radial_hits);wr32((uintptr_t)screen+0x2f8,5);
    cpu.rcx=(uintptr_t)system;fp_mobile_camera_before(&cpu);
    for(unsigned i=0;i<5;++i){
        uint64_t target=frostpunk_mobile_swipe_target(.5f,.5f,3+i);assert(target);
        uint32_t bits[2]={(uint32_t)(target>>32),(uint32_t)target};float xy[2];memcpy(xy,bits,8);
        assert(fabsf(xy[0]-(.3f+i*.1f))<.0001f&&fabsf(xy[1]-.85f)<.0001f);
    }
    wr32((uintptr_t)radial_buttons[2]+0x1e0,0);cpu.rcx=(uintptr_t)system;fp_mobile_camera_before(&cpu);
    assert(!frostpunk_mobile_swipe_target(.5f,.5f,5));
    assert(!frostpunk_mobile_submit(5,.5f,0,0,0)&&!frostpunk_mobile_submit(5,1,1,0,0)&&!frostpunk_mobile_submit(5,1,NAN,0,0));
    assert(!frostpunk_mobile_swipe_target(.5f,.5f,8)&&!frostpunk_mobile_swipe_target(NAN,.5f,3));
    uint64_t modal=(uintptr_t)radial_modal;wr64(modal,GVA(0x141D78A60));wr64(modal+0x1d8,(uintptr_t)"EXT_SOCIOTECH_PANEL");
    wr64(modal+0x230,(uintptr_t)screen);wr32(modal+0x1e4,4000);wrss(modal+0xf0,1);wrss(modal+0xf4,1);
    wr64((uintptr_t)radial_buttons[4]+0x240,modal);cpu.rcx=(uintptr_t)system;fp_mobile_camera_before(&cpu);
    assert(!(frostpunk_mobile_context(.5f,.5f)&((1u<<25)|(1u<<26))));
    cpu.rcx=(uintptr_t)screen;scale=100;fp_mobile_ui_before(&cpu);
    assert(!(frostpunk_mobile_context(.5f,.5f)&(1u<<26)));
    wr32(modal+0x1e0,2);cpu.rcx=(uintptr_t)system;fp_mobile_camera_before(&cpu);cpu.rcx=(uintptr_t)screen;fp_mobile_ui_before(&cpu);
    assert((frostpunk_mobile_context(.5f,.5f)&(1u<<26))&&frostpunk_mobile_submit(5,1,0,0,0));
    assert(frostpunk_mobile_submit(5,0,0,0,0));cpu.rcx=(uintptr_t)screen;fp_mobile_ui_before(&cpu);
    assert(!(frostpunk_mobile_context(.5f,.5f)&(1u<<25))&&!frostpunk_mobile_swipe_target(.5f,.5f,3));
    for(unsigned i=0;i<5;++i)assert(rd32((uintptr_t)radial_buttons[i]+0x1e0)&2);
    switch_active=0;
    fp_mobile_camera_destroy();assert(!frostpunk_dxvk_scene_active());
    free(image);puts("{\"success\":true,\"ui_scale\":true,\"ui_root_resize\":true,\"ui_partial_canvas_restore\":true,\"ui_scale_live_tree_only\":true,\"paint_mask_projection\":true,\"fixed_dialog_fit\":true,\"construction_widgets\":true,\"law_tree_viewport_and_content\":true,\"settings_header\":true,\"small_event_layout\":true,\"input_during_loading_blocked\":true,\"ui_context_while_camera_suspended\":true,\"right_drag_scroll_pane\":true,\"native_time_selection\":true,\"time_hit_rectangles\":true,\"building_rotation_both_directions\":true,\"analog_accumulation\":true,\"reset_position_rotation_zoom\":true,\"cancel_and_camera_lock\":true,\"invalid_inputs\":true,\"phone_table_rows_and_restore\":true,\"phone_parent_measure_invalidation\":true,\"phone_scrollbar_geometry_cache\":true,\"technology_detail_viewport_reflow\":true,\"building_blur_guest_return\":true}");
}
