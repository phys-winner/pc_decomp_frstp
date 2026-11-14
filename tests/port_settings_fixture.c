#define _GNU_SOURCE
#include "port_settings.h"
#include "cpu64.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static unsigned debug_calls;
int64_t g_image_delta;
static void L_000140E1DC20(CPU *c){c->rsp+=8;}
void fp_dispatch_main(CPU *c,uint64_t target,void (*linked)(CPU*)){
 assert(target==UINT64_C(0x140e1dc20)&&linked==L_000140E1DC20);linked(c);
}
#include "render_defaults_production.inc"
static unsigned option_creates,option_destroys;
void dispatch(CPU *c,uint64_t target){
 if(target==UINT64_C(0x140E538C0)){wr64(c->rcx,(uintptr_t)strdup((const char*)(uintptr_t)c->rdx));++option_creates;}
 else if(target==UINT64_C(0x140E52950)){free((void*)(uintptr_t)rd64(c->rcx));wr64(c->rcx,0);++option_destroys;}
 else if(target==UINT64_C(0x141A14E20)){
  uint32_t count=rd32(c->rcx+0x30);assert(count<rd32(c->rcx+0x34));uint64_t at=rd64(c->rcx+0x28)+(uint64_t)count*16;
  wr32(at,(uint32_t)c->rdx);wr64(at+8,rd64(c->r8));wr64(c->r8,0);wr32(c->rcx+0x30,count+1);
 }else assert(!"unknown frame options dispatch");
 c->rsp+=8;
}
#include "frame_options_production.inc"
static void check_frame_options(CPU *cpu){
 unsigned char row[0x80]={0},options[33*16]={0};wr32((uintptr_t)row+0x20,28);wr64((uintptr_t)row+0x28,(uintptr_t)options);
 wr32((uintptr_t)row+0x30,1);wr32((uintptr_t)row+0x34,33);wr64((uintptr_t)options+8,(uintptr_t)"Auto");
 uint32_t rates[]={60,90,120};fp_port_frame_rates_configure(rates,3);fp_port_setting_set(FP_SETTING_FRAME_LIMIT,120);
 CPU saved=*cpu;fp_frame_options(cpu,(uintptr_t)row);assert(!memcmp(cpu,&saved,sizeof(saved)));
 assert(rd32((uintptr_t)row+0x30)==4&&option_creates==3&&!option_destroys);
 for(unsigned i=0;i<3;++i)assert(rd32((uintptr_t)options+(i+1)*16)==rates[i]);
 fp_frame_options(cpu,(uintptr_t)row);assert(option_creates==3&&!option_destroys&&rd32((uintptr_t)row+0x30)==4);
 uint32_t changed[]={60,120};fp_port_frame_rates_configure(changed,2);fp_frame_options(cpu,(uintptr_t)row);
 assert(rd32((uintptr_t)row+0x30)==3&&option_creates==5&&option_destroys==3);
 assert(rd32((uintptr_t)options+16)==60&&rd32((uintptr_t)options+32)==120&&fp_port_setting_get(FP_SETTING_FRAME_LIMIT)==120);
 assert(!strcmp((const char*)(uintptr_t)rd64((uintptr_t)options+8),"Auto"));
 for(unsigned n=0;n<100;++n)fp_frame_options(cpu,(uintptr_t)row);
 assert(option_creates==5&&option_destroys==3&&!memcmp(cpu,&saved,sizeof(saved)));
 for(unsigned i=1;i<3;++i)free((void*)(uintptr_t)rd64((uintptr_t)options+i*16+8));
 fp_port_setting_set(FP_SETTING_FRAME_LIMIT,0);
}
void fp_mobile_debug_menu_request(void){++debug_calls;}
int main(void){
 _Alignas(16) unsigned char renderer[0x200]={0},stack[1024];
 CPU cpu={.rcx=(uintptr_t)renderer,.rsp=(uintptr_t)stack+sizeof(stack)-32};
 push64(&cpu,0);L_000140E5C1A0(&cpu);
 assert(rdss((uintptr_t)renderer+0xf4)==1.0f&&rdss((uintptr_t)renderer+0xf8)==.25f);
 check_frame_options(&cpu);
 char folder[]="/data/local/tmp/fp-settings-XXXXXX";assert(mkdtemp(folder));
 fp_port_settings_load(folder);
 assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==200&&frostpunk_dxvk_object_highlight()==1);
 fp_port_setting_set(FP_SETTING_UI_SCALE,150);fp_port_settings_load(folder);
 assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==150&&fp_port_setting_get(FP_SETTING_OUTPUT_SCALE)==50);
 /* Switch interface: off by default; the pre-2026-11-14 key switch_ui= is ignored. */
 assert(fp_port_setting_get(FP_SETTING_SWITCH_UI)==0&&frostpunk_switch_ui()==0);
 {char legacy[256];snprintf(legacy,sizeof(legacy),"%s/port-settings.txt",folder);FILE*l=fopen(legacy,"w");assert(l);
  fputs("switch_ui=1\n",l);assert(!fclose(l));fp_port_settings_load(folder);assert(!fp_port_setting_get(FP_SETTING_SWITCH_UI)&&!frostpunk_switch_ui());
  assert(!unlink(legacy));}
 fp_port_setting_set(FP_SETTING_SWITCH_UI,1);assert(fp_port_setting_get(FP_SETTING_SWITCH_UI)==1&&frostpunk_switch_ui()==0);
 fp_port_setting_set(FP_SETTING_SWITCH_UI,2);assert(fp_port_setting_get(FP_SETTING_SWITCH_UI)==1);
 fp_port_settings_load(folder);assert(fp_port_setting_get(FP_SETTING_SWITCH_UI)==1&&frostpunk_switch_ui()==1);
 fp_port_setting_set(FP_SETTING_SWITCH_UI,0);assert(frostpunk_switch_ui()==1);
 fp_port_settings_load(folder);assert(!fp_port_setting_get(FP_SETTING_SWITCH_UI)&&!frostpunk_switch_ui());
 assert(fp_port_setting_get(FP_SETTING_OUTPUT_SCALE)==50&&frostpunk_dxvk_mobile_effects()==1);
 assert(fp_port_setting_get(FP_SETTING_TOUCH_OFFSET)==64&&frostpunk_dxvk_upscaler()==0);
 uint32_t rates[]={60,90,120};fp_port_frame_rates_configure(rates,3);
 assert(fp_port_setting_get(FP_SETTING_FRAME_LIMIT)==0&&frostpunk_android_frame_limit()==120);
 fp_port_setting_set(FP_SETTING_FRAME_LIMIT,90);fp_port_setting_set(FP_SETTING_FRAME_LIMIT,75);
 assert(fp_port_setting_get(FP_SETTING_FRAME_LIMIT)==90&&frostpunk_android_frame_limit()==90);
 fp_port_settings_load(folder);assert(fp_port_setting_get(FP_SETTING_FRAME_LIMIT)==90);
 uint32_t slower[]={60};fp_port_frame_rates_configure(slower,1);
 assert(!fp_port_setting_get(FP_SETTING_FRAME_LIMIT)&&frostpunk_android_frame_limit()==60);
 uint32_t invalid[]={60,60};fp_port_frame_rates_configure(invalid,2);
 uint32_t actual[32];assert(fp_port_frame_rates(actual)==1&&actual[0]==60);
 fp_port_frame_rates_configure(rates,3);fp_port_setting_set(FP_SETTING_FRAME_LIMIT,60);
 struct timespec begin,end;assert(!clock_gettime(CLOCK_MONOTONIC,&begin));
 for(unsigned i=0;i<6;++i)frostpunk_android_frame_pace();
 assert(!clock_gettime(CLOCK_MONOTONIC,&end));
 int64_t elapsed=(int64_t)(end.tv_sec-begin.tv_sec)*INT64_C(1000000000)+end.tv_nsec-begin.tv_nsec;
 assert(elapsed>=INT64_C(1000000000)*5/60);
 fp_port_setting_set(FP_SETTING_FRAME_LIMIT,0);assert(frostpunk_android_frame_limit()==120);
 fp_port_setting_set(FP_SETTING_OUTPUT_SCALE,20);fp_port_setting_set(FP_SETTING_OUTPUT_SCALE,19);
 assert(fp_port_setting_get(FP_SETTING_OUTPUT_SCALE)==20);
 fp_port_setting_set(FP_SETTING_TOUCH_OFFSET,128);fp_port_setting_set(FP_SETTING_TOUCH_OFFSET,129);
 assert(fp_port_setting_get(FP_SETTING_TOUCH_OFFSET)==128);
 fp_port_setting_set(FP_SETTING_UPSCALER,2);fp_port_setting_set(FP_SETTING_UPSCALER,3);assert(frostpunk_dxvk_upscaler()==2);
 fp_port_settings_load(folder);assert(frostpunk_dxvk_upscaler()==2);
 frostpunk_dxvk_upscaler_failed(7);assert(frostpunk_dxvk_upscaler()==1);
 fp_port_settings_load(folder);assert(frostpunk_dxvk_upscaler()==1);
 fp_port_setting_set(FP_SETTING_UPSCALER,0);frostpunk_dxvk_upscaler_failed(7);assert(!frostpunk_dxvk_upscaler());
 for(unsigned scale=100;scale<=200;scale+=10){fp_port_setting_set(FP_SETTING_UI_SCALE,scale);assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==scale);}
 fp_port_setting_set(FP_SETTING_UI_SCALE,201);fp_port_setting_set(FP_SETTING_UI_SCALE,101);assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==200);
 fp_port_setting_set(FP_SETTING_MOBILE_EFFECTS,1);fp_port_setting_set(FP_SETTING_OBJECT_HIGHLIGHT,0);
 assert(frostpunk_dxvk_mobile_effects()==1&&!frostpunk_dxvk_object_highlight());
 fp_port_setting_set(FP_SETTING_OBJECT_HIGHLIGHT,1);fp_port_setting_set(FP_SETTING_MOBILE_EFFECTS,0);
 assert(!frostpunk_dxvk_mobile_effects()&&frostpunk_dxvk_object_highlight()==1);
 fp_port_setting_set(FP_SETTING_DEBUG_MENU,1);assert(debug_calls==1&&fp_port_setting_get(FP_SETTING_DEBUG_MENU)==0);
 fp_port_setting_set(FP_SETTING_DEBUG_MENU,0);assert(debug_calls==1);
 fp_port_settings_load(folder);assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==200);
 char path[256];snprintf(path,sizeof(path),"%s/port-settings.txt",folder);FILE*f=fopen(path,"w");assert(f);fputs("ui_scale=101\nobject_highlight=9\ndebug_menu=1\n",f);assert(!fclose(f));
 fp_port_settings_load(folder);assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==200&&frostpunk_dxvk_object_highlight()==1);
 assert(fp_port_setting_get(FP_SETTING_DEBUG_MENU)==0&&debug_calls==1);
 fp_port_setting_set(FP_SETTING_UI_SCALE,200);fp_port_setting_set(FP_SETTING_SWITCH_UI,1);
 setenv("FROSTPUNK_PORT_DATA","0",1);setenv("FROSTPUNK_SWITCH_DATA","0",1);
 fp_port_settings_load(folder);assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==100&&!fp_port_setting_get(FP_SETTING_SWITCH_UI)&&!frostpunk_switch_ui());
 fp_port_setting_set(FP_SETTING_UI_SCALE,200);fp_port_setting_set(FP_SETTING_SWITCH_UI,1);
 assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==100&&!fp_port_setting_get(FP_SETTING_SWITCH_UI));
 setenv("FROSTPUNK_PORT_DATA","1",1);setenv("FROSTPUNK_SWITCH_DATA","1",1);
 fp_port_setting_set(FP_SETTING_UI_SCALE,200);fp_port_setting_set(FP_SETTING_SWITCH_UI,1);fp_port_settings_load(folder);
 assert(fp_port_setting_get(FP_SETTING_UI_SCALE)==200&&frostpunk_switch_ui());
 fp_port_setting_set(FP_SETTING_MOBILE_EFFECTS,1);
 setenv("PORT_OPTION_SWITCH_GRAPHICS","0",1);fp_port_settings_load(folder);
 assert(!frostpunk_dxvk_mobile_effects());for(unsigned group=0;group<FP_GFX_COUNT;++group)assert(frostpunk_mobile_graphics_pc(group)&&!frostpunk_mobile_graphics_reduced(group));
 setenv("PORT_OPTION_SWITCH_GRAPHICS","1",1);fp_port_settings_load(folder);
 assert(frostpunk_dxvk_mobile_effects());for(unsigned group=0;group<FP_GFX_COUNT;++group)assert(frostpunk_mobile_graphics_pc(group)==(group==FP_GFX_SNOW)&&frostpunk_mobile_graphics_reduced(group)==(group!=FP_GFX_SNOW));
 assert(!unlink(path)&&!rmdir(folder));
 puts("{\"success\":true,\"render_default_100\":true,\"output_default_50\":true,\"phone_and_tablet_default_200\":true,\"saved_ui_150_preserved_at_output_50\":true,\"ui_scale_range_step_10\":true,\"highlight_independent\":true,\"debug_action\":true,\"persistence\":true,\"invalid_settings\":true}");
}
