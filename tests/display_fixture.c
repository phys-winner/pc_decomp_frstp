/* Controlled virtual-desktop fixture. It does not create an Android surface. */
#include "win64_display.h"
#include "win64.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint64_t base;size_t bytes;int writable;} region;
static region regions[1024];static unsigned region_count,checks,callbacks;static uint32_t error_value;static uint64_t callback_result=1;static int32_t callback_rect[4];
static void require(int valid,const char *why){if(!valid){fprintf(stderr,"DISPLAY_FIXTURE_FAIL %s error=%u\n",why,error_value);exit(1);}++checks;}
void win64_set_error(uint32_t value){error_value=value;}uint32_t win64_get_error(void){return error_value;}
void win64_region_add(uint64_t address,size_t bytes,int writable){require(region_count<1024,"region capacity");regions[region_count++]=(region){address,bytes,writable};}
void win64_region_remove(uint64_t address){for(unsigned i=0;i<region_count;++i)if(regions[i].base==address){regions[i]=regions[--region_count];return;}abort();}
void *win64_pointer(uint64_t address,size_t bytes,int writable){for(unsigned i=0;i<region_count;++i)if(address>=regions[i].base && address-regions[i].base<=regions[i].bytes && bytes<=regions[i].bytes-(address-regions[i].base) && (!writable || regions[i].writable))return(void*)(uintptr_t)address;fprintf(stderr,"DISPLAY_FIXTURE_BADPTR %llx %zu write%d\n",(unsigned long long)address,bytes,writable);abort();}
uint64_t win64_alloc(size_t bytes,int zero){void *p=zero?calloc(1,bytes):malloc(bytes);require(p!=NULL,"allocation");win64_region_add((uintptr_t)p,bytes,1);return(uintptr_t)p;}
void win64_free(uint64_t address){win64_region_remove(address);free((void*)(uintptr_t)address);}
uint64_t win64_argument(CPU *c,unsigned i){uint64_t first[]={c->rcx,c->rdx,c->r8,c->r9};if(i<4)return first[i];uint64_t value;memcpy(&value,win64_pointer(c->rsp+40+8*(i-4),8,0),8);return value;}
int win64_window_call(CPU *c,const char *name){if(!strcmp(name,"IsWindow")){c->rax=c->rcx==0x1111 || c->rcx==0x2222;return 1;}if(!strcmp(name,"GetWindowRect")){if(c->rcx!=0x1111){c->rax=0;win64_set_error(50);return 1;}int32_t r[]={0,0,960,540};memcpy(win64_pointer(c->rdx,16,1),r,16);c->rax=1;return 1;}abort();}
#define V(x) ((uint64_t)(x))
static uint64_t api(const char *name,unsigned count,...){
    _Alignas(16) uint64_t stack[256]={0};uint64_t arguments[8]={0};va_list list;va_start(list,count);for(unsigned i=0;i<count;++i)arguments[i]=va_arg(list,uint64_t);va_end(list);
    win64_region_add((uintptr_t)stack,sizeof(stack),1);CPU c={0};c.rsp=(uintptr_t)&stack[129];c.rcx=arguments[0];c.rdx=arguments[1];c.r8=arguments[2];c.r9=arguments[3];c.rax=0xfedcba98;c.rbx=0xabcdef;c.cf=c.of=1;CPU before=c;
    for(unsigned i=4;i<count;++i)stack[134+i-4]=arguments[i];require(win64_display_call(&c,name),"handled display import");uint64_t result=c.rax;c.rax=before.rax;require(!memcmp(&c,&before,sizeof(c)),"all caller CPU/RSP/flags preserved");win64_region_remove((uintptr_t)stack);return result;
}
void dispatch(CPU *cpu,uint64_t target){
    require(target==UINT64_C(0x140001000) && cpu->rcx==1 && cpu->r9==UINT64_C(0x123456789abcdef0),"original static monitor callback arguments");
    require((cpu->rsp&15)==8 && *(uint64_t*)win64_pointer(cpu->rsp,8,0)==UINT64_C(0xe5ee0000),"monitor callback alignment/return sentinel");
    uint64_t *shadow=win64_pointer(cpu->rsp+8,32,1);shadow[0]=0x11223344;shadow[3]=0xaabbccdd;
    memcpy(callback_rect,win64_pointer(cpu->r8,16,0),16);++callbacks;require(api("GetSystemMetrics",1,V(0))==960,"callback reentry outside module mutex");cpu->rax=callback_result;cpu->rsp+=8;
}
static uint32_t u32(const void *p,size_t off){uint32_t v;memcpy(&v,(const char*)p+off,4);return v;}
static uint16_t u16(const void *p,size_t off){uint16_t v;memcpy(&v,(const char*)p+off,2);return v;}
static void put32(void *p,size_t off,uint32_t value){memcpy((char*)p+off,&value,4);}static void put16(void *p,size_t off,uint16_t value){memcpy((char*)p+off,&value,2);}
static void wide(uint16_t *out,const char *text){for(unsigned i=0;;++i){out[i]=(uint8_t)text[i];if(!text[i])break;}}
int main(void){
    require(!api("GetSystemMetrics",1,V(0)) && error_value==50,"default disabled policy");win64_display_self_test();require(!api("GetSystemMetrics",1,V(0)),"selftest restores disabled state");win64_display_configure(960,540,60);
    require(api("GetSystemMetrics",1,V(0))==960 && api("GetSystemMetrics",1,V(1))==540 && api("GetSystemMetrics",1,V(78))==960 && api("GetSystemMetrics",1,V(79))==540 && api("GetSystemMetrics",1,V(80))==1,"one coherent virtual desktop");
    uint16_t adapter[64],child[64],driver[16],unknown[16];wide(adapter,"\\\\.\\DISPLAY1");wide(child,"\\\\.\\DISPLAY1\\Monitor0");wide(driver,"DISPLAY");wide(unknown,"MISSING");
    win64_region_add((uintptr_t)adapter,sizeof(adapter),0);win64_region_add((uintptr_t)child,sizeof(child),0);win64_region_add((uintptr_t)driver,sizeof(driver),0);win64_region_add((uintptr_t)unknown,sizeof(unknown),0);
    unsigned char device[856];memset(device,0xa5,sizeof(device));put32(device,0,840);win64_region_add((uintptr_t)device,sizeof(device),1);
    require(api("EnumDisplayDevicesW",4,V(0),V(0),V((uintptr_t)device),V(0)) && !memcmp(device+4,adapter,22) && u32(device,324)==5,"virtual adapter name/state");
    for(unsigned i=840;i<856;++i)require(device[i]==0xa5,"DISPLAY_DEVICE canary");
    require(api("EnumDisplayDevicesW",4,V((uintptr_t)adapter),V(0),V((uintptr_t)device),V(0)) && !memcmp(device+4,child,40) && u32(device,324)==3,"nested actual monitor enumeration");
    win64_set_error(0x5a17);require(!api("EnumDisplayDevicesW",4,V(0),V(1),V((uintptr_t)device),V(0)) && error_value==0x5a17 && u32(device,0)==840,"end enumeration preserves cb/error");for(unsigned i=4;i<840;++i)require(!device[i],"end enumeration clears only documented device body");
    unsigned char mode[236],before_mode[236];memset(mode,0xa5,sizeof(mode));put16(mode,68,220);put16(mode,70,0);win64_region_add((uintptr_t)mode,sizeof(mode),1);
    require(api("EnumDisplaySettingsW",3,V((uintptr_t)adapter),V(UINT32_MAX),V((uintptr_t)mode)) && u16(mode,68)==188 && u32(mode,168)==32 && u32(mode,172)==960 && u32(mode,176)==540 && u32(mode,184)==60 && !u32(mode,76) && !u32(mode,80),"DEVMODE native prefix and declared virtual current mode");
    for(unsigned i=188;i<236;++i)require(mode[i]==0xa5,"DEVMODE SDK tail and canary untouched");
    require(api("EnumDisplaySettingsExW",4,V((uintptr_t)child),V(0),V((uintptr_t)mode),V(0))!=0,"one enumerated virtual mode");memcpy(before_mode,mode,sizeof(mode));put16(mode,68,220);put16(before_mode,68,188);win64_set_error(0x5a17);
    require(!api("EnumDisplaySettingsW",3,V(0),V(1),V((uintptr_t)mode)) && !memcmp(mode,before_mode,sizeof(mode)) && error_value==0x5a17,"normal no-more-modes mutation matches native observed prefix normalization");
    unsigned char monitor[120];memset(monitor,0xa5,sizeof(monitor));put32(monitor,0,104);win64_region_add((uintptr_t)monitor,sizeof(monitor),1);
    require(api("GetMonitorInfoW",2,V(1),V((uintptr_t)monitor)) && u32(monitor,12)==960 && u32(monitor,16)==540 && u32(monitor,28)==960 && u32(monitor,32)==540 && u32(monitor,36)==1 && !memcmp(monitor+40,adapter,22),"MONITORINFOEX virtual bounds/work/name/primary");for(unsigned i=104;i<120;++i)require(monitor[i]==0xa5,"MONITORINFOEX canary");
    require(api("MonitorFromPoint",2,V(((uint64_t)539<<32)|959),V(0))==1 && !api("MonitorFromPoint",2,V(960),V(0)) && api("MonitorFromPoint",2,UINT64_MAX,V(2))==1,"monitor signed point edge/default-nearest");
    int32_t clip[]={0,0,1,1};win64_region_add((uintptr_t)clip,sizeof(clip),1);require(api("MonitorFromRect",2,V((uintptr_t)clip),V(0))==1 && api("MonitorFromWindow",2,V(0x1111),V(0))==1 && !api("MonitorFromWindow",2,V(0x3333),V(0)),"rect/window monitor validation");
    require(api("EnumDisplayMonitors",4,V(0),V((uintptr_t)clip),UINT64_C(0x140001000),UINT64_C(0x123456789abcdef0))==1 && callbacks==1 && !memcmp(callback_rect,clip,16),"monitor callback sees clipped rectangle");
    callback_result=0;require(!api("EnumDisplayMonitors",4,V(0),V(0),UINT64_C(0x140001000),UINT64_C(0x123456789abcdef0)) && callbacks==2 && callback_rect[2]==960 && callback_rect[3]==540,"callback false stops enumeration");callback_result=1;
    callback_result=UINT64_C(0x8000000000000000);require(!api("EnumDisplayMonitors",4,V(0),V(0),UINT64_C(0x140001000),UINT64_C(0x123456789abcdef0)) && callbacks==3,"callback BOOL uses low32 only");callback_result=1;
    clip[0]=1000;clip[2]=1001;require(api("EnumDisplayMonitors",4,V(0),V((uintptr_t)clip),UINT64_C(0x140001000),UINT64_C(0x123456789abcdef0)) && callbacks==3,"nonintersecting clip no callback");
    uint64_t dc=api("CreateDCW",4,V((uintptr_t)adapter),V(0),V(0),V(0));require(dc && api("GetDeviceCaps",2,dc,V(8))==960 && api("GetDeviceCaps",2,dc,V(10))==540 && api("GetDeviceCaps",2,dc,V(12))==32 && api("GetDeviceCaps",2,dc,V(88))==96 && api("GetDeviceCaps",2,dc,V(90))==96 && api("GetDeviceCaps",2,dc,V(116))==60,"metrics-only DC coordinates/format/DPI/Hz");
    require(!api("GetDeviceCaps",2,dc,V(4)) && error_value==50,"no fabricated physical millimeters");uint64_t bitmap=api("CreateCompatibleBitmap",3,dc,V(1),V(1));require(bitmap && !api("CreateCompatibleBitmap",3,dc,V(2),V(2)) && error_value==50,"genuine bounded CPU bitmap object");
    unsigned char info[68];memset(info,0xa5,sizeof(info));put32(info,0,40);put16(info,14,0);uint32_t pixel=0xaabbccdd;win64_region_add((uintptr_t)info,sizeof(info),1);win64_region_add((uintptr_t)&pixel,4,1);
    require(api("GetDIBits",7,dc,bitmap,V(0),V(1),V(0),V((uintptr_t)info),V(0))==1 && u32(info,4)==1 && u32(info,8)==1 && u16(info,12)==1 && u16(info,14)==32 && u32(info,16)==3 && u32(info,20)==4,"SDL header-only bitmap query");
    for(unsigned i=40;i<68;++i)require(info[i]==0xa5,"header-only GetDIBits does not overwrite masks/palette");
    require(api("GetDIBits",7,dc,bitmap,V(0),V(1),V(0),V((uintptr_t)info),V(0))==1 && u32(info,40)==0xff0000 && u32(info,44)==0xff00 && u32(info,48)==0xff,"SDL second RGB888 BI_BITFIELDS query writes three masks");
    unsigned char header40[40],header44[44];memset(header40,0xa5,sizeof(header40));memset(header44,0xa5,sizeof(header44));put32(header40,0,40);put16(header40,14,0);put32(header44,0,40);put16(header44,14,0);win64_region_add((uintptr_t)header40,sizeof(header40),1);win64_region_add((uintptr_t)header44,sizeof(header44),1);
    require(api("GetDIBits",7,dc,bitmap,V(0),V(1),V(0),V((uintptr_t)header40),V(0))==1 && u16(header40,14)==32 && u32(header40,16)==3,"legal exact40-byte header-only registered output");
    require(api("GetDIBits",7,dc,bitmap,V(0),V(1),V(0),V((uintptr_t)header44),V(0))==1 && u32(header44,40)==0xa5a5a5a5,"legal44-byte BITMAPINFO header query preserves color slot");
    require(api("GetDIBits",7,dc,bitmap,V(0),V(1),V((uintptr_t)&pixel),V((uintptr_t)info),V(0))==1 && !pixel,"real CPU zero pixel read");for(unsigned i=52;i<68;++i)require(info[i]==0xa5,"BITMAPINFO RGB masks canary");
    require(!api("DeleteDC",1,bitmap) && error_value==6 && !api("DeleteObject",1,dc) && error_value==6,"DC and bitmap namespaces reject foreign kinds");
    require(api("DeleteObject",1,bitmap) && !api("DeleteObject",1,bitmap) && error_value==6 && api("DeleteDC",1,dc) && !api("GetDeviceCaps",2,dc,V(8)) && error_value==6,"strict object lifetime/tombstone handles");
    dc=api("CreateDCW",4,V((uintptr_t)driver),V((uintptr_t)adapter),V(0),V(0));require(dc && api("DeleteDC",1,dc),"standard DISPLAY driver plus adapter name");
    uint32_t dpi[2]={0xaaaaaaaa,0xbbbbbbbb};win64_region_add((uintptr_t)dpi,sizeof(dpi),1);require(!api("GetDpiForMonitor",4,V(1),V(0),V((uintptr_t)dpi),V((uintptr_t)(dpi+1))) && dpi[0]==96 && dpi[1]==96 && api("GetDpiForSystem",0)==96 && api("GetDpiForWindow",1,V(0x1111))==96,"explicit virtual coordinate DPI");
    require(api("GetDpiForMonitor",4,V(1),V(1),V((uintptr_t)dpi),V((uintptr_t)(dpi+1)))==0x80004001 && dpi[0]==96 && dpi[1]==96,"raw physical DPI explicitly unavailable");
    clip[0]=0;clip[1]=0;clip[2]=960;clip[3]=540;int32_t original[4];memcpy(original,clip,16);
    require(api("AdjustWindowRectEx",4,V((uintptr_t)clip),V(0x86000000),V(0),V(0))==1 && !memcmp(original,clip,16),"actual SDL borderless popup has no invented chrome");require(!api("AdjustWindowRectEx",4,V((uintptr_t)clip),V(0xcf0000),V(0),V(0)) && error_value==50 && !memcmp(original,clip,16),"windowed chrome unsupported without fake geometry");
    CPU c={0};c.rsp=123;c.rax=456;CPU before=c;require(!win64_display_call(&c,"BitBlt") && !memcmp(&c,&before,sizeof(c)),"drawing remains unhandled");win64_display_self_test();win64_display_configure(0,0,0);require(!api("GetSystemMetrics",1,V(0)),"disable virtual policy");
    printf("DISPLAY_FIXTURE_PASS checks=%u callbacks=%u ABI840_220_40_104_52=1 virtual960x540x32at60=1 metrics_DC=1 CPU_bitmap=1 strict_handles=1 callback_shadow_reentry=1 no_rsp_pop=1 actual_android_surface=0 original_game_exec=0\n",checks,callbacks);return 0;
}
