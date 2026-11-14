/* Explicit compatibility desktop, independent of the physical Android surface.
 * Metrics DCs and CPU bitmaps are real bounded objects; no drawing/GPU or
 * physical mode-switch capabilities are advertised by this module. */
#include "win64_display.h"
#include "win64_window.h"
#include "win64.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {int32_t left,top,right,bottom;} rect;
typedef struct {uint32_t size;uint16_t name[32],description[128];uint32_t flags;uint16_t id[128],key[128];} display_device;
typedef struct {
    uint16_t device[32],spec,driver,size,extra;uint32_t fields;
    int32_t x,y;uint32_t orientation,fixed;uint16_t color,duplex,y_resolution,tt_option,collate,form[32],logical_pixels;
    uint32_t bits,width,height,flags,frequency,icm_method,icm_intent,media,dither,reserved[2],panning_width,panning_height;
} device_mode;
typedef struct {uint32_t size;rect monitor,work;uint32_t flags;uint16_t name[32];} monitor_info;
typedef struct {uint32_t size;int32_t width,height;uint16_t planes,bits;uint32_t compression,image_bytes;int32_t xppm,yppm;uint32_t used,important,masks[3];} bitmap_info;
_Static_assert(sizeof(display_device)==840 && offsetof(display_device,flags)==324 && offsetof(display_device,id)==328,"Win64 DISPLAY_DEVICEW");
_Static_assert(sizeof(device_mode)==220 && offsetof(device_mode,size)==68 && offsetof(device_mode,fields)==72 && offsetof(device_mode,bits)==168 && offsetof(device_mode,frequency)==184,"Win64 DEVMODEW");
_Static_assert(sizeof(monitor_info)==104 && offsetof(monitor_info,name)==40 && offsetof(monitor_info,flags)==36,"Win64 MONITORINFO/EXW");
_Static_assert(sizeof(bitmap_info)==52 && offsetof(bitmap_info,masks)==40,"Win64 BITMAPINFOHEADER and three RGB masks");

typedef struct {int active,common;uint64_t owner;} metrics_dc;
typedef struct {int active,kind;uint32_t pixel,width,rows,pitch;uint64_t memory;size_t bytes;} cpu_bitmap;
#define BITMAP_BYTE_LIMIT (64u*1024u*1024u)
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t desktop_width,desktop_height,desktop_hz;
static metrics_dc dcs[256];static unsigned dc_count;
static cpu_bitmap bitmaps[256];static unsigned bitmap_count;
static size_t bitmap_bytes;
static const char adapter_name[]="\\\\.\\DISPLAY1",monitor_name[]="\\\\.\\DISPLAY1\\Monitor0";
static _Noreturn void stop(const char *why){fprintf(stderr,"STOP Win64 display %s\n",why);fflush(stderr);_Exit(73);}
static uint64_t fail(uint32_t error,uint64_t value){win64_set_error(error);return value;}
static int policy(uint32_t *width,uint32_t *height,uint32_t *hz){pthread_mutex_lock(&mutex);*width=desktop_width;*height=desktop_height;*hz=desktop_hz;pthread_mutex_unlock(&mutex);return *width!=0;}
void win64_display_configure(uint32_t width,uint32_t height,uint32_t hz){
    if((width||height||hz) && (!width || !height || !hz || width>16384 || height>16384 || hz>1000))stop("invalid explicit desktop policy");
    pthread_mutex_lock(&mutex);
    /* Existing CPU bitmaps retain their own dimensions across a desktop mode change. */
    desktop_width=width;desktop_height=height;desktop_hz=hz;pthread_mutex_unlock(&mutex);
}
static void wide(uint16_t *out,size_t cap,const char *text){size_t i=0;for(;text[i] && i+1<cap;++i)out[i]=(uint8_t)text[i];out[i]=0;}
static int equal_wide(uint64_t address,const char *text){
    if(!address)return 0;for(size_t i=0;i<128;++i){uint16_t unit;memcpy(&unit,win64_pointer(address+2*i,2,0),2);unsigned char expected=(unsigned char)text[i];if(unit>='a'&&unit<='z')unit-='a'-'A';if(expected>='a'&&expected<='z')expected-='a'-'A';if(unit!=expected)return 0;if(!unit)return 1;}return 0;
}
static int known_device(uint64_t name){return !name || equal_wide(name,adapter_name) || equal_wide(name,monitor_name);}
static metrics_dc *dc_locked(uint64_t handle){for(unsigned i=0;i<dc_count;++i)if(handle==(uintptr_t)&dcs[i] && dcs[i].active)return &dcs[i];return NULL;}
static cpu_bitmap *bitmap_locked(uint64_t handle){for(unsigned i=0;i<bitmap_count;++i)if(handle==(uintptr_t)&bitmaps[i] && bitmaps[i].active)return &bitmaps[i];return NULL;}
static uint64_t create_dc(CPU *cpu){
    uint64_t driver=win64_argument(cpu,0),device=win64_argument(cpu,1);
    if(!driver || !(known_device(driver) || equal_wide(driver,"DISPLAY")) || !known_device(device) || win64_argument(cpu,2) || win64_argument(cpu,3))return fail(50,0);
    pthread_mutex_lock(&mutex);if(dc_count==256){pthread_mutex_unlock(&mutex);return fail(8,0);}metrics_dc *dc=&dcs[dc_count++];*dc=(metrics_dc){.active=1};pthread_mutex_unlock(&mutex);return(uintptr_t)dc;
}
static uint64_t get_dc(CPU *cpu){
    uint64_t owner=win64_argument(cpu,0);
    /* USER32 validation can pump/reenter display APIs: never hold our mutex. */
    if(owner){CPU check=*cpu;check.rcx=owner;if(!win64_window_call(&check,"IsWindow"))stop("IsWindow validator unavailable");if(!check.rax)return fail(1400,0);}
    pthread_mutex_lock(&mutex);if(dc_count==256){pthread_mutex_unlock(&mutex);return fail(8,0);}
    metrics_dc *dc=&dcs[dc_count++];*dc=(metrics_dc){.active=1,.common=1,.owner=owner};pthread_mutex_unlock(&mutex);return(uintptr_t)dc;
}
static uint64_t release_dc(CPU *cpu){
    uint64_t owner=win64_argument(cpu,0),handle=win64_argument(cpu,1);if(!handle)return 0;
    pthread_mutex_lock(&mutex);metrics_dc *dc=dc_locked(handle);
    if(!dc){int retired=0;for(unsigned i=0;i<dc_count;++i)if(handle==(uintptr_t)&dcs[i])retired=1;pthread_mutex_unlock(&mutex);return retired?0:fail(1425,0);}
    /* shortcut: only the original SDL matching common-DC release contract is
     * implemented. Native tolerates some wrong-owner/kind misuse; do not
     * silently emulate that undocumented behavior or retire a different DC. */
    if(!dc->common || dc->owner!=owner){pthread_mutex_unlock(&mutex);stop("ReleaseDC unsupported owner or private DC");}
    dc->active=0;pthread_mutex_unlock(&mutex);return 1;
}
static uint64_t enum_device(CPU *cpu){
    uint64_t name=win64_argument(cpu,0),output=win64_argument(cpu,2);uint32_t index=(uint32_t)win64_argument(cpu,1),flags=(uint32_t)win64_argument(cpu,3),size;
    memcpy(&size,win64_pointer(output,4,0),4);if(size!=840)return fail(87,0);if(flags)return fail(50,0);
    display_device value={.size=840};int adapter=!name,valid=!index && (adapter || equal_wide(name,adapter_name));
    if(valid){wide(value.name,32,adapter?adapter_name:monitor_name);wide(value.description,128,adapter?"Frostpunk Android virtual display":"Frostpunk Android virtual monitor");value.flags=adapter?5:3;wide(value.id,128,adapter?"FROSTPUNK\\VIRTUAL\\DISPLAY1":"FROSTPUNK\\VIRTUAL\\MONITOR0");}
    memcpy(win64_pointer(output,sizeof(value),1),&value,sizeof(value));return valid;
}
static uint64_t enum_settings(CPU *cpu,int extended,uint32_t width,uint32_t height,uint32_t hz){
    uint64_t name=win64_argument(cpu,0),output=win64_argument(cpu,2);uint32_t index=(uint32_t)win64_argument(cpu,1);uint16_t size,extra;
    memcpy(&size,win64_pointer(output+68,2,0),2);memcpy(&extra,win64_pointer(output+70,2,0),2);
    if((size!=188 && size!=220) || extra)return fail(87,0);if(extended && win64_argument(cpu,3))return fail(50,0);
    /* Native observed drivers normalize dmSize to the display prefix188 even
     * on failure; the remaining SDK220 tail is not overwritten. */
    uint16_t prefix=188;memcpy(win64_pointer(output+68,2,1),&prefix,2);
    if(!known_device(name) || (index && index!=UINT32_MAX && index!=UINT32_MAX-1))return 0;
    device_mode value={0};wide(value.device,32,"Frostpunk virtual display");value.spec=0x401;value.driver=1;value.size=188;
    value.fields=0x007c00a0;value.bits=32;value.width=width;value.height=height;value.frequency=hz;value.logical_pixels=96;
    memcpy(win64_pointer(output,188,1),&value,188);return 1;
}
static uint64_t change_settings(CPU *cpu,uint32_t width,uint32_t height,uint32_t hz){
    uint64_t name=win64_argument(cpu,0),input=win64_argument(cpu,1);uint32_t flags=(uint32_t)win64_argument(cpu,3);
    if(win64_argument(cpu,2) || (name && !equal_wide(name,adapter_name)))return UINT32_C(0xfffffffb);
    if((flags&~UINT32_C(6)) || win64_argument(cpu,4))stop("ChangeDisplaySettingsExW unsupported flags or video parameters");
    /* shortcut: the configured virtual desktop has one available/default mode.
     * Applying that same mode, or restoring NULL, is a real no-op; add a mode
     * transition and WM_DISPLAYCHANGE only when a second mode is provided. */
    if(!input)return 0;
    uint16_t size,extra;memcpy(&size,win64_pointer(input+68,2,0),2);memcpy(&extra,win64_pointer(input+70,2,0),2);
    if((size!=188 && size!=220) || extra)stop("ChangeDisplaySettingsExW unsupported DEVMODE prefix or driver data");
    device_mode mode={0};memcpy(&mode,win64_pointer(input,188,0),188);
    if(mode.fields!=UINT32_C(0x7c0000) && mode.fields!=UINT32_C(0x7c00a0))stop("ChangeDisplaySettingsExW unsupported DEVMODE fields");
    if(mode.bits!=32 || mode.width!=width || mode.height!=height || mode.flags || mode.frequency!=hz ||
       ((mode.fields&0x20) && (mode.x || mode.y)) || ((mode.fields&0x80) && mode.orientation))return UINT32_C(0xfffffffe);
    return 0;
}
static uint64_t monitor_info_call(CPU *cpu,uint32_t width,uint32_t height){
    uint64_t output=win64_argument(cpu,1);uint32_t size;if(win64_argument(cpu,0)!=1)return fail(1461,0);
    memcpy(&size,win64_pointer(output,4,0),4);if(size!=40 && size!=104)return fail(87,0);
    monitor_info value={.size=size,.monitor={0,0,(int32_t)width,(int32_t)height},.work={0,0,(int32_t)width,(int32_t)height},.flags=1};wide(value.name,32,adapter_name);
    memcpy(win64_pointer(output,size,1),&value,size);return 1;
}
static int intersect(rect a,rect b,rect *out){
    out->left=a.left>b.left?a.left:b.left;out->top=a.top>b.top?a.top:b.top;out->right=a.right<b.right?a.right:b.right;out->bottom=a.bottom<b.bottom?a.bottom:b.bottom;
    return out->left<out->right && out->top<out->bottom;
}
static uint64_t monitor_from(CPU *cpu,const char *name,uint32_t width,uint32_t height){
    uint32_t flags=(uint32_t)win64_argument(cpu,1);if(flags>2)return fail(87,0);rect area;
    if(!strcmp(name,"MonitorFromPoint")){uint64_t point=win64_argument(cpu,0);int32_t x=(int32_t)point,y=(int32_t)(point>>32);return (x>=0 && y>=0 && (uint32_t)x<width && (uint32_t)y<height)||flags?1:0;}
    if(!strcmp(name,"MonitorFromRect"))memcpy(&area,win64_pointer(win64_argument(cpu,0),16,0),16);
    else {
        CPU check=*cpu;check.rcx=win64_argument(cpu,0);win64_window_call(&check,"IsWindow");if(!check.rax)return fail(1400,0);
        win64_region_add((uintptr_t)&area,sizeof(area),1);check.rdx=(uintptr_t)&area;win64_window_call(&check,"GetWindowRect");win64_region_remove((uintptr_t)&area);if(!check.rax)return 0;
    }
    rect clipped;return intersect(area,(rect){0,0,(int32_t)width,(int32_t)height},&clipped)||flags?1:0;
}
static uint64_t enum_monitors(CPU *cpu,uint32_t width,uint32_t height){
    uint64_t dc=win64_argument(cpu,0),clip=win64_argument(cpu,1),callback=win64_argument(cpu,2),data=win64_argument(cpu,3);if(!callback)return fail(87,0);
    if(dc){pthread_mutex_lock(&mutex);int valid=dc_locked(dc)!=NULL;pthread_mutex_unlock(&mutex);if(!valid)return fail(6,0);}
    rect bounds={0,0,(int32_t)width,(int32_t)height};if(clip){rect area;memcpy(&area,win64_pointer(clip,16,0),16);if(!intersect(bounds,area,&bounds))return 1;}
    uint64_t memory=win64_alloc(16,0);memcpy(win64_pointer(memory,16,1),&bounds,16);CPU saved=*cpu;
    if(saved.rsp<64)stop("monitor callback stack underflow");uint64_t frame=(saved.rsp-48)&~UINT64_C(15);win64_pointer(frame-8,56,1);
    cpu->rsp=frame;cpu->rcx=1;cpu->rdx=dc;cpu->r8=memory;cpu->r9=data;push64(cpu,UINT64_C(0xe5ee0000));dispatch(cpu,callback);
    if(cpu->rsp!=frame)stop("monitor callback return stack imbalance");uint32_t result=(uint32_t)cpu->rax;*cpu=saved;win64_free(memory);return result?1:0;
}
static uint64_t capabilities(CPU *cpu,uint32_t width,uint32_t height,uint32_t hz){
    pthread_mutex_lock(&mutex);int valid=dc_locked(win64_argument(cpu,0))!=NULL;pthread_mutex_unlock(&mutex);if(!valid)return fail(6,0);
    switch((int)win64_argument(cpu,1)){
        case 2:return 1;case 8:case 118:return width;case 10:case 117:return height;case 12:return 32;case 14:return 1;
        case 24:return UINT32_MAX;case 88:case 90:return 96;case 116:return hz;
        default:return fail(50,0);
    }
}
static uint64_t bitmap_create(CPU *cpu){
    pthread_mutex_lock(&mutex);int valid=dc_locked(win64_argument(cpu,0))!=NULL;
    if(!valid){pthread_mutex_unlock(&mutex);return fail(6,0);}if((int)win64_argument(cpu,1)!=1 || (int)win64_argument(cpu,2)!=1){pthread_mutex_unlock(&mutex);return fail(50,0);}
    if(bitmap_count==256){pthread_mutex_unlock(&mutex);return fail(8,0);}cpu_bitmap *bitmap=&bitmaps[bitmap_count++];bitmap->active=1;bitmap->pixel=0;pthread_mutex_unlock(&mutex);return(uintptr_t)bitmap;
}
static uint64_t bitmap_allocate(uint32_t width,uint32_t rows,uint32_t pitch,int kind,const void *source){
    uint64_t bytes=(uint64_t)pitch*rows;
    pthread_mutex_lock(&mutex);
    if(bitmap_count==256 || bytes>BITMAP_BYTE_LIMIT || bytes>BITMAP_BYTE_LIMIT-bitmap_bytes){pthread_mutex_unlock(&mutex);return fail(8,0);}
    uint64_t memory=win64_alloc((size_t)bytes,1);
    if(!memory){pthread_mutex_unlock(&mutex);return fail(8,0);}
    if(source)memcpy(win64_pointer(memory,(size_t)bytes,1),source,(size_t)bytes);
    cpu_bitmap *bitmap=&bitmaps[bitmap_count++];
    *bitmap=(cpu_bitmap){.active=1,.kind=kind,.width=width,.rows=rows,.pitch=pitch,.memory=memory,.bytes=(size_t)bytes};
    bitmap_bytes+=(size_t)bytes;pthread_mutex_unlock(&mutex);return(uintptr_t)bitmap;
}
static uint64_t bitmap_delete(uint64_t handle){
    int saved_errno=errno;uint64_t memory=0;
    pthread_mutex_lock(&mutex);cpu_bitmap *bitmap=bitmap_locked(handle);
    if(bitmap){bitmap->active=0;memory=bitmap->memory;bitmap->memory=0;bitmap_bytes-=bitmap->bytes;bitmap->bytes=0;}
    pthread_mutex_unlock(&mutex);if(memory)win64_free(memory);errno=saved_errno;
    return bitmap?1:fail(6,0);
}
/* SDL WIN_CreateCursor141B20020 uses an unchecked ppvBits before creating its
 * optional native cursor. Supply real pixel storage, then let the unsupported
 * icon boundary fail honestly; a NULL DIB here would leave a bad memcpy target. */
static uint64_t dib_create(CPU *cpu){
    uint64_t dc=win64_argument(cpu,0),info=win64_argument(cpu,1),output=win64_argument(cpu,3),section=win64_argument(cpu,4);
    uint32_t usage=(uint32_t)win64_argument(cpu,2);(void)win64_argument(cpu,5);
    uint64_t *bits=output?win64_pointer(output,8,1):NULL;if(bits){uint64_t zero=0;memcpy(bits,&zero,8);}
    if(!info)return 0;
    uint32_t size;memcpy(&size,win64_pointer(info,4,0),4);if(size!=108)return fail(50,0);
    unsigned char header[108];memcpy(header,win64_pointer(info,sizeof(header),0),sizeof(header));
    bitmap_info format;memcpy(&format,header,sizeof(format));
    if(format.width<=0 || !format.height || format.height==INT32_MIN || format.planes!=1 || usage>1)return fail(87,0);
    if(usage || section || format.bits!=32 || format.compression!=3 || format.masks[0]!=0xff0000 || format.masks[1]!=0xff00 || format.masks[2]!=0xff)return fail(50,0);
    uint32_t alpha;memcpy(&alpha,header+52,4);if(alpha!=0xff000000 || format.used)return fail(50,0);
    if(dc){pthread_mutex_lock(&mutex);int valid=dc_locked(dc)!=NULL;pthread_mutex_unlock(&mutex);if(!valid)return fail(6,0);}
    uint32_t rows=format.height<0?(uint32_t)-(int64_t)format.height:(uint32_t)format.height;
    uint64_t pitch=(uint64_t)(uint32_t)format.width*4;
    if(pitch>UINT32_MAX || pitch*rows>BITMAP_BYTE_LIMIT)return fail(8,0);
    uint64_t handle=bitmap_allocate((uint32_t)format.width,rows,(uint32_t)pitch,1,NULL);
    if(handle && bits){pthread_mutex_lock(&mutex);uint64_t memory=bitmap_locked(handle)->memory;pthread_mutex_unlock(&mutex);memcpy(bits,&memory,8);}
    return handle;
}
static uint64_t mono_create(CPU *cpu){
    int32_t width=(int32_t)(uint32_t)win64_argument(cpu,0),height=(int32_t)(uint32_t)win64_argument(cpu,1);
    uint32_t planes=(uint32_t)win64_argument(cpu,2),bits=(uint32_t)win64_argument(cpu,3);uint64_t input=win64_argument(cpu,4);
    if(width<0 || height<0)return fail(87,0);
    if(!width || !height){width=height=1;planes=bits=1;input=0;}
    if(planes!=1 || bits!=1)return fail(50,0);
    uint64_t pitch=(((uint64_t)(uint32_t)width+15)/16)*2,bytes=pitch*(uint32_t)height;
    if(pitch>UINT32_MAX || bytes>BITMAP_BYTE_LIMIT)return fail(8,0);
    const void *source=input?win64_pointer(input,(size_t)bytes,0):NULL;
    return bitmap_allocate((uint32_t)width,(uint32_t)height,(uint32_t)pitch,2,source);
}
static uint64_t bitmap_query(CPU *cpu){
    uint64_t dc=win64_argument(cpu,0),handle=win64_argument(cpu,1),pixels=win64_argument(cpu,4),output=win64_argument(cpu,5);uint32_t start=(uint32_t)win64_argument(cpu,2),lines=(uint32_t)win64_argument(cpu,3),usage=(uint32_t)win64_argument(cpu,6);
    pthread_mutex_lock(&mutex);cpu_bitmap *bitmap=bitmap_locked(handle);int valid=dc_locked(dc)&&bitmap;uint32_t pixel=bitmap?bitmap->pixel:0;pthread_mutex_unlock(&mutex);
    if(!valid)return fail(6,0);if(bitmap->kind || start || lines!=1 || usage || !output)return fail(50,0);
    uint32_t size,compression;uint16_t requested_bits;memcpy(&size,win64_pointer(output,4,0),4);if(size!=40)return fail(50,0);
    memcpy(&requested_bits,win64_pointer(output+14,2,0),2);memcpy(&compression,win64_pointer(output+16,4,0),4);
    if((requested_bits && (requested_bits!=32 || compression!=3)) || (pixels && !requested_bits))return fail(50,0);
    bitmap_info value={.size=40,.width=1,.height=1,.planes=1,.bits=32,.compression=3,.image_bytes=4,.masks={0x00ff0000,0x0000ff00,0x000000ff}};
    /* biBitCount0 is the header-only metadata query: native GetDIBits does not
     * write a palette/masks until the caller repeats with an explicit format. */
    size_t bytes=requested_bits?52:40;memcpy(win64_pointer(output,bytes,1),&value,bytes);if(pixels)memcpy(win64_pointer(pixels,4,1),&pixel,4);return 1;
}
int win64_display_call(CPU *cpu,const char *name){
    if(!strcmp(name,"CreateDIBSection") || !strcmp(name,"CreateBitmap") || !strcmp(name,"CreateIconIndirect")){
        int saved_errno=errno;uint32_t width,height,hz;
        if(!policy(&width,&height,&hz))cpu->rax=fail(50,0);
        else if(!strcmp(name,"CreateDIBSection"))cpu->rax=dib_create(cpu);
        else if(!strcmp(name,"CreateBitmap"))cpu->rax=mono_create(cpu);
        /* This touch profile has no backed native cursor image. Never return
         * an invented HICON/HCURSOR; SDL safely keeps its NULL/default cursor. */
        else {if(cpu->rcx)win64_pointer(cpu->rcx,32,0);cpu->rax=fail(cpu->rcx?50:87,0);}
        errno=saved_errno;return 1;
    }
    if(!strcmp(name,"ChangeDisplaySettingsExW")){
        int saved_errno=errno;uint32_t width,height,hz;
        cpu->rax=policy(&width,&height,&hz)?change_settings(cpu,width,height,hz):fail(50,UINT32_MAX);
        errno=saved_errno;return 1;
    }
    int supported=!strcmp(name,"EnumDisplayDevicesW") || !strcmp(name,"EnumDisplaySettingsW") || !strcmp(name,"EnumDisplaySettingsExW") || !strcmp(name,"EnumDisplayMonitors") || !strcmp(name,"MonitorFromPoint") || !strcmp(name,"MonitorFromRect") || !strcmp(name,"MonitorFromWindow") || !strcmp(name,"GetMonitorInfoW") || !strcmp(name,"GetSystemMetrics") || !strcmp(name,"CreateDCW") || !strcmp(name,"GetDC") || !strcmp(name,"ReleaseDC") || !strcmp(name,"DeleteDC") || !strcmp(name,"GetDeviceCaps") || !strcmp(name,"CreateCompatibleBitmap") || !strcmp(name,"GetDIBits") || !strcmp(name,"DeleteObject") || !strcmp(name,"GetDpiForMonitor") || !strcmp(name,"GetDpiForSystem") || !strcmp(name,"GetDpiForWindow") || !strcmp(name,"AdjustWindowRectEx");
    if(!supported)return 0;uint32_t width,height,hz;if(!policy(&width,&height,&hz)){cpu->rax=fail(50,!strcmp(name,"GetDpiForMonitor")?UINT32_C(0x80004001):0);return 1;}
    if(!strcmp(name,"EnumDisplayDevicesW"))cpu->rax=enum_device(cpu);
    else if(!strcmp(name,"EnumDisplaySettingsW") || !strcmp(name,"EnumDisplaySettingsExW"))cpu->rax=enum_settings(cpu,!strcmp(name,"EnumDisplaySettingsExW"),width,height,hz);
    else if(!strcmp(name,"GetMonitorInfoW"))cpu->rax=monitor_info_call(cpu,width,height);
    else if(!strncmp(name,"MonitorFrom",11))cpu->rax=monitor_from(cpu,name,width,height);
    else if(!strcmp(name,"EnumDisplayMonitors"))cpu->rax=enum_monitors(cpu,width,height);
    else if(!strcmp(name,"CreateDCW"))cpu->rax=create_dc(cpu);
    else if(!strcmp(name,"GetDC"))cpu->rax=get_dc(cpu);
    else if(!strcmp(name,"ReleaseDC"))cpu->rax=release_dc(cpu);
    else if(!strcmp(name,"DeleteDC")){pthread_mutex_lock(&mutex);metrics_dc *dc=dc_locked(win64_argument(cpu,0));if(dc && dc->common){pthread_mutex_unlock(&mutex);stop("DeleteDC unsupported common DC");}if(dc)dc->active=0;pthread_mutex_unlock(&mutex);cpu->rax=dc?1:fail(6,0);}
    else if(!strcmp(name,"GetDeviceCaps"))cpu->rax=capabilities(cpu,width,height,hz);
    else if(!strcmp(name,"CreateCompatibleBitmap"))cpu->rax=bitmap_create(cpu);
    else if(!strcmp(name,"GetDIBits"))cpu->rax=bitmap_query(cpu);
    else if(!strcmp(name,"DeleteObject"))cpu->rax=bitmap_delete(win64_argument(cpu,0));
    else if(!strcmp(name,"GetSystemMetrics")){switch((int)win64_argument(cpu,0)){case 0:case 16:case 61:case 78:cpu->rax=width;break;case 1:case 17:case 62:case 79:cpu->rax=height;break;case 76:case 77:case 0x1000:cpu->rax=0;break;case 80:case 81:cpu->rax=1;break;default:cpu->rax=fail(50,0);break;}}
    else if(!strcmp(name,"GetDpiForMonitor")){
        uint64_t x=win64_argument(cpu,2),y=win64_argument(cpu,3);uint32_t type=(uint32_t)win64_argument(cpu,1),dpi=96;
        if(win64_argument(cpu,0)!=1 || !x || !y || type>2)cpu->rax=UINT32_C(0x80070057);
        else if(type)cpu->rax=UINT32_C(0x80004001);else{memcpy(win64_pointer(x,4,1),&dpi,4);memcpy(win64_pointer(y,4,1),&dpi,4);cpu->rax=0;}
    }
    else if(!strcmp(name,"GetDpiForSystem"))cpu->rax=96;
    else if(!strcmp(name,"GetDpiForWindow")){CPU check=*cpu;win64_window_call(&check,"IsWindow");cpu->rax=check.rax?96:fail(1400,0);}
    else if(!strcmp(name,"AdjustWindowRectEx")){
        uint32_t style=(uint32_t)win64_argument(cpu,1),ex=(uint32_t)win64_argument(cpu,3);
        if(win64_argument(cpu,2) || (style&0x00c40000u) || (ex&0x00020201u))cpu->rax=fail(50,0);
        else{win64_pointer(win64_argument(cpu,0),16,1);cpu->rax=1;}
    }
    return 1;
}
void win64_display_self_test(void){
    uint32_t width,height,hz;int configured=policy(&width,&height,&hz);if(!configured)win64_display_configure(960,540,60);
    CPU cpu={0};cpu.rsp=UINT64_C(0x12345678);cpu.rcx=0;int valid=win64_display_call(&cpu,"GetSystemMetrics") && cpu.rax==(configured?width:960) && cpu.rsp==UINT64_C(0x12345678);
    cpu.rcx=UINT64_C(0xffffffff00000000);cpu.rdx=0;valid=valid && win64_display_call(&cpu,"MonitorFromPoint") && !cpu.rax && !win64_display_call(&cpu,"UnknownDisplayFunction");
    if(!configured)win64_display_configure(0,0,0);if(!valid)stop("virtual desktop/no-pop self-test");
    fprintf(stderr,"WIN64_DISPLAY_SELF_TEST virtual_desktop=%ux%u@%u monitor=1 physical_hardware_claim=0 no_rsp_pop=1 passed\n",configured?width:960,configured?height:540,configured?hz:60);
}
