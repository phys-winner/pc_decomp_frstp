/* A real keyboard state adapter for the original DirectInput8A consumer.
 * HWND/focus and physical key state come from the sole Android event pump. */
#include "win64_dinput.h"
#include "win64.h"
#include "win64_window.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DI_S_OK UINT32_C(0)
#define DI_S_FALSE UINT32_C(1)
#define DI_INVALID UINT32_C(0x80070057)
#define DI_POINTER UINT32_C(0x80004003)
#define DI_HANDLE UINT32_C(0x80070006)
#define DI_NO_INTERFACE UINT32_C(0x80004002)
#define DI_UNSUPPORTED UINT32_C(0x80004001)
#define DI_NO_AGGREGATION UINT32_C(0x80040110)
#define DI_OUT_OF_MEMORY UINT32_C(0x8007000e)
#define DI_NOT_INITIALIZED UINT32_C(0x80070015)
#define DI_OTHER_APP UINT32_C(0x80070005)
#define DI_ACQUIRED UINT32_C(0x800700aa)
#define DI_NOT_ACQUIRED UINT32_C(0x8007000c)
#define DI_INPUT_LOST UINT32_C(0x8007001e)
#define DI_DEVICE_NOT_REGISTERED UINT32_C(0x80040154)
#define OBJECT_LIMIT 256
#define ROOT_SLOTS 11
#define DEVICE_SLOTS 32
#define DEVICE_BASE (WIN64_DINPUT_SENTINEL_BASE+UINT64_C(0x100))

typedef struct {uint32_t size,object_size,flags,data_bytes,objects,pad;uint64_t description;} data_format;
typedef struct {uint64_t guid;uint32_t offset,type,flags,pad;} object_format;
_Static_assert(sizeof(data_format)==32 && sizeof(object_format)==24,"Win64 DirectInput data formats");
static const uint8_t iid_unknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_input8a[16]={0x30,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static const uint8_t iid_device8a[16]={0x80,0x10,0xd4,0x54,0x15,0xdc,0x33,0x48,0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79};
static const uint8_t iid_input8w[16]={0x31,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static const uint8_t clsid_input8[16]={0xe4,0x09,0xe6,0x25,0x59,0xb2,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t guid_keyboard[16]={0x61,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t guid_key[16]={0x20,0x82,0x72,0x55,0x3c,0xd3,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
typedef struct {uint64_t guest,hwnd;uint32_t refs,kind,cooperative;int formatted,acquired,wide,initialized;} input_object;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static uint64_t root_vtable[ROOT_SLOTS],device_vtable[DEVICE_SLOTS];
static input_object objects[OBJECT_LIMIT];static unsigned object_count;

static _Noreturn void stop(const char *message,int status) {
    const char *parts[]={"STOP Win64 DirectInput ",message,"\n"};
    for(unsigned i=0;i<3;++i){const char *p=parts[i];size_t n=strlen(p);while(n){ssize_t done=write(2,p,n);if(done<0&&errno==EINTR)continue;if(done<=0)break;p+=done;n-=(size_t)done;}}
    _Exit(status);
}
static void initialize(void) {
    for(unsigned i=0;i<ROOT_SLOTS;++i)root_vtable[i]=WIN64_DINPUT_SENTINEL_BASE+i*8;
    for(unsigned i=0;i<DEVICE_SLOTS;++i)device_vtable[i]=DEVICE_BASE+i*8;
    win64_region_add((uintptr_t)root_vtable,sizeof(root_vtable),0);
    win64_region_add((uintptr_t)device_vtable,sizeof(device_vtable),0);
}
static void initialized(void) {if(pthread_once(&once,initialize))stop("initialization",73);}
static input_object *find_object(uint64_t address,unsigned kind) {
    for(unsigned i=0;i<object_count;++i)if(objects[i].guest==address && objects[i].refs && objects[i].kind==kind)return &objects[i];
    stop("invalid or released interface",73);
}
static uint64_t make_object(unsigned kind) {
    if(object_count==OBJECT_LIMIT)return 0;
    uint64_t guest=win64_alloc(8,1);if(!guest)return 0;
    uint64_t table=(uintptr_t)(kind==1?root_vtable:device_vtable);
    memcpy(win64_pointer(guest,8,1),&table,8);
    /* Keep a process-lifetime tombstone, so a released COM address cannot be
     * recycled into an unrelated live interface. No native fd/FILE is exposed. */
    win64_region_add(guest,8,0);
    objects[object_count++]=(input_object){.guest=guest,.refs=1,.kind=kind,.initialized=kind==1};return guest;
}
static void write_pointer(uint64_t address,uint64_t value) {memcpy(win64_pointer(address,8,1),&value,8);}
static int guid_equal(uint64_t address,const uint8_t expected[16]) {return !memcmp(win64_pointer(address,16,0),expected,16);}
static uint32_t create_input(CPU *cpu) {
    uint64_t output=win64_argument(cpu,3),outer=win64_argument(cpu,4);
    if(!output)return DI_POINTER;write_pointer(output,0);
    if(outer)return DI_NO_AGGREGATION;
    /* The supplied fixed-base image is the sole supported module handle. */
    if(win64_argument(cpu,0)!=UINT64_C(0x140000000))return DI_INVALID;
    if(!guid_equal(win64_argument(cpu,2),iid_input8a))return DI_NO_INTERFACE;
    uint32_t version=(uint32_t)win64_argument(cpu,1);
    if(!version)return DI_NOT_INITIALIZED;
    if(version<0x800)return UINT32_C(0x80070481);
    if(version>0x800)return UINT32_C(0x8007047e);
    pthread_mutex_lock(&mutex);uint64_t object=make_object(1);pthread_mutex_unlock(&mutex);
    if(!object)return DI_OUT_OF_MEMORY;write_pointer(output,object);return DI_S_OK;
}
static uint32_t query_interface(CPU *cpu,input_object *object) {
    uint64_t output=win64_argument(cpu,2);if(!output)return DI_POINTER;write_pointer(output,0);
    uint64_t iid=win64_argument(cpu,1);
    if(!guid_equal(iid,iid_unknown) && !guid_equal(iid,object->kind==1?(object->wide?iid_input8w:iid_input8a):iid_device8a))return DI_NO_INTERFACE;
    if(object->refs==UINT32_MAX)stop("reference overflow",73);++object->refs;write_pointer(output,object->guest);return DI_S_OK;
}
static uint32_t format_keyboard(input_object *object,uint64_t address) {
    if(object->acquired)return DI_ACQUIRED;
    if(!address)return DI_POINTER;data_format format;memcpy(&format,win64_pointer(address,sizeof(format),0),sizeof(format));
    if(format.size!=32 || format.object_size!=24)return DI_INVALID;
    if(!format.description && format.objects)return DI_POINTER;
    if(format.data_bytes<256 && format.objects==256)return DI_INVALID;
    /* shortcut: the proved original c_dfDIKeyboard layout is the supported
     * format. Mouse, reordered/custom objects and buffered data need their own
     * contracts before any successful availability claim. */
    if(format.flags!=2 || format.data_bytes!=256 || format.objects!=256)return DI_UNSUPPORTED;
    for(unsigned i=0;i<256;++i) {
        if(format.description>UINT64_MAX-(uint64_t)i*24)stop("format object overflow",73);
        object_format key;memcpy(&key,win64_pointer(format.description+(uint64_t)i*24,24,0),24);
        if(key.offset!=i || key.type!=(UINT32_C(0x8000000c)|(i<<8)) || key.flags || !guid_equal(key.guid,guid_key))return DI_UNSUPPORTED;
    }
    object->formatted=1;return DI_S_OK;
}
static int snapshot(input_object *object,uint8_t keys[256]) {
    return win64_window_input_snapshot(object->hwnd,keys);
}
static uint32_t device_method(CPU *cpu,input_object *object,unsigned slot) {
    switch(slot) {
    case 7: {
        if(!object->formatted)return DI_INVALID;
        if(!object->cooperative)stop("default/background cooperative profile is unsupported",70);
        uint8_t keys[256];int state=snapshot(object,keys);
        if(state<1){object->acquired=0;return DI_OTHER_APP;}
        if(object->acquired)return DI_S_FALSE;object->acquired=1;return DI_S_OK;
    }
    case 8: {uint32_t result=object->acquired?DI_S_OK:DI_S_FALSE;object->acquired=0;return result;}
    case 9: {
        if(!object->acquired)return DI_NOT_ACQUIRED;
        if((uint32_t)win64_argument(cpu,1)!=256)return DI_INVALID;
        if(!win64_argument(cpu,2))stop("NULL acquired keyboard state output",73);
        uint8_t keys[256];if(snapshot(object,keys)<1){object->acquired=0;return DI_INPUT_LOST;}
        memcpy(win64_pointer(win64_argument(cpu,2),256,1),keys,256);return DI_S_OK;
    }
    case 11:return format_keyboard(object,win64_argument(cpu,1));
    case 13: {
        uint64_t hwnd=win64_argument(cpu,1);uint32_t flags=(uint32_t)win64_argument(cpu,2);
        if(object->acquired)return DI_ACQUIRED;
        if((flags&~31u) || !(flags&3u) || (flags&3u)==3u || !(flags&12u) || (flags&12u)==12u)return DI_INVALID;
        if(flags!=6 && flags!=0x16)return DI_UNSUPPORTED;
        uint8_t keys[256];if(win64_window_input_snapshot(hwnd,keys)<0)return DI_HANDLE;
        object->hwnd=hwnd;object->cooperative=flags;return DI_S_OK;
    }
    default:stop("unsupported keyboard COM method",70);
    }
}
/* Staged only: trusted caller passes the real current COM apartment state.
 * Unknown CLSIDs remain unhandled for the ordinary class dispatcher. */
int win64_dinput_com_call(CPU *cpu,const char *name,int apartment_initialized) {
    if(strcmp(name,"CoCreateInstance"))return 0;
    if(!guid_equal(win64_argument(cpu,0),clsid_input8))return 0;
    int saved_errno=errno;uint32_t last=win64_get_error(),result;uint64_t output=win64_argument(cpu,4);
    if(!output)result=DI_POINTER;
    else {
        write_pointer(output,0);
        if(!apartment_initialized)result=UINT32_C(0x800401f0);
        else if(win64_argument(cpu,1))result=DI_NO_AGGREGATION;
        else if((uint32_t)win64_argument(cpu,2)!=1)result=DI_UNSUPPORTED;
        else if(!guid_equal(win64_argument(cpu,3),iid_input8w))result=DI_NO_INTERFACE;
        else {
            initialized();pthread_mutex_lock(&mutex);uint64_t guest=make_object(1);
            if(guest){input_object *object=find_object(guest,1);object->wide=1;object->initialized=0;}
            pthread_mutex_unlock(&mutex);result=guest?DI_S_OK:DI_OUT_OF_MEMORY;if(guest)write_pointer(output,guest);
        }
    }
    cpu->rax=result;win64_set_error(last);errno=saved_errno;return 1;
}
static uint32_t wide_root_method(CPU *cpu,input_object *object,unsigned slot) {
    if(slot==7) {
        if(win64_argument(cpu,1)!=UINT64_C(0x140000000))return DI_INVALID;
        uint32_t version=(uint32_t)win64_argument(cpu,2);
        if(!version)return DI_NOT_INITIALIZED;
        if(version<0x800)return UINT32_C(0x80070481);
        if(version>0x800)return UINT32_C(0x8007047e);
        object->initialized=1;return DI_S_OK; /* Native repeat Initialize is S_OK. */
    }
    if(slot==4) {
        if(!win64_argument(cpu,2))return DI_INVALID;
        if(!object->initialized)return DI_NOT_INITIALIZED;
        uint32_t kind=(uint32_t)win64_argument(cpu,1),flags=(uint32_t)win64_argument(cpu,4);
        /* Explicit compatibility capability: no joystick or force-feedback
         * backend exists. Neither profile includes the real keyboard device.
         * Empty enumeration is successful and invokes no guest callbacks. */
        if((kind==4 && flags==1) || (kind==0 && flags==0x101))return DI_S_OK;
        stop("unsupported SDL controller enumeration profile",70);
    }
    stop("unsupported SDL DirectInput8W COM method",70);
}
int win64_dinput_call(CPU *cpu,const char *name) {
    if(strcmp(name,"DirectInput8Create"))return 0;
    int saved_errno=errno;uint32_t last=win64_get_error();initialized();cpu->rax=create_input(cpu);win64_set_error(last);errno=saved_errno;return 1;
}
int win64_dinput_dispatch(CPU *cpu,uint64_t target) {
    unsigned kind,slot;
    if(target>=WIN64_DINPUT_SENTINEL_BASE && target<WIN64_DINPUT_SENTINEL_BASE+ROOT_SLOTS*8){kind=1;slot=(unsigned)(target-WIN64_DINPUT_SENTINEL_BASE)/8;}
    else if(target>=DEVICE_BASE && target<DEVICE_BASE+DEVICE_SLOTS*8){kind=2;slot=(unsigned)(target-DEVICE_BASE)/8;}
    else return 0;
    if(target&7)stop("unaligned COM sentinel",73);
    int saved_errno=errno;uint32_t last=win64_get_error();initialized();pthread_mutex_lock(&mutex);
    input_object *object=find_object(win64_argument(cpu,0),kind);uint32_t result;
    if(slot==0)result=query_interface(cpu,object);
    else if(slot==1){if(object->refs==UINT32_MAX)stop("reference overflow",73);result=++object->refs;}
    else if(slot==2){result=--object->refs;if(!result)object->acquired=0;}
    else if(kind==1 && object->wide)result=wide_root_method(cpu,object,slot);
    else if(kind==1 && slot==3) {
        uint64_t output=win64_argument(cpu,2);if(!output)result=DI_POINTER;
        else {write_pointer(output,0);if(win64_argument(cpu,3))result=DI_NO_AGGREGATION;
            else if(!guid_equal(win64_argument(cpu,1),guid_keyboard))result=DI_DEVICE_NOT_REGISTERED;
            else {uint64_t device=make_object(2);result=device?DI_S_OK:DI_OUT_OF_MEMORY;if(device)write_pointer(output,device);}}
    } else if(kind==2)result=device_method(cpu,object,slot);
    else stop("unsupported root COM method",70);
    pthread_mutex_unlock(&mutex);cpu->rax=result;win64_set_error(last);errno=saved_errno;return 1;
}
void win64_dinput_self_test(void) {
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));before=cpu;
    if(win64_dinput_call(&cpu,"UnknownDirectInput") || win64_dinput_dispatch(&cpu,0) || win64_dinput_dispatch(&cpu,UINT64_MAX) || memcmp(&cpu,&before,sizeof(cpu)))stop("unrelated CPU preservation self-test",73);
    fprintf(stderr,"WIN64_DINPUT_SELF_TEST keyboard_only=1 no_guest_rsp_pop=1 PASS\n");
}
