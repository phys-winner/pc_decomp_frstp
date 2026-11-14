/* Bounded SM5 DXBC reflection for Frostpunk's original retail cache.
 * Independent format reader, not a compiler or shader translator. Full
 * D3D11_SHADER_DESC and input signatures are read from RDEF/STAT/SHEX/ISGN.
 * Only IUnknown, GetDesc and GetInputParameterDesc methods are implemented;
 * an actually called different reflection method stops explicitly.
 * DXBC checksum uses the private C11 port of AMD's published checksum helper;
 * original license and provenance are in reference/dxbc-checksum.
 */
#include "win64_shader.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dxbc_checksum.inc"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK shader_lock=SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&shader_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&shader_lock)
#else
#include <pthread.h>
static pthread_mutex_t shader_lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&shader_lock)
#define UNLOCK() pthread_mutex_unlock(&shader_lock)
#endif
#define E_INVALIDARG UINT32_C(0x80070057)
#define E_NOINTERFACE UINT32_C(0x80004002)
#define E_FAIL UINT32_C(0x80004005)
#define E_OUTOFMEMORY UINT32_C(0x8007000e)
#define INVALID_DXBC UINT32_C(0x8876086c)
#define FOUR(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
typedef struct {const unsigned char *data;size_t size;} chunk;
typedef struct shader_object {
    uint64_t guest;unsigned refs;unsigned char *bytes;size_t size;
    unsigned char desc[160];chunk input;struct shader_object *next;
} shader_object;
static shader_object *objects;
static uint64_t vtable;
static const unsigned char reflection_iid[16]={0xa1,0x6c,0x53,0x8d,0xca,0x0c,0x56,0x49,0xa8,0x37,0x78,0x69,0x63,0x75,0x55,0x84};
static const unsigned char unknown_iid[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static void stop(const char *why){fprintf(stderr,"STOP Win64 shader reflection %s\n",why);fflush(stderr);_Exit(73);}
static uint32_t u32(const void*p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t u64(const void*p){uint64_t v;memcpy(&v,p,8);return v;}
static void put32(void*p,uint32_t v){memcpy(p,&v,4);}
static void put64(void*p,uint64_t v){memcpy(p,&v,8);}
static int range(size_t n,size_t offset,size_t bytes){return offset<=n&&bytes<=n-offset;}
static const char *string_at(chunk c,uint32_t offset){
    if(offset>=c.size)return NULL;
    return memchr(c.data+offset,0,c.size-offset)?(const char*)c.data+offset:NULL;
}
static int signature(chunk c,unsigned stride){
    if(!c.data||c.size<8)return 0;
    uint32_t count=u32(c.data);if(count>4096||!range(c.size,8,(size_t)count*stride))return 0;
    for(uint32_t i=0;i<count;++i){const unsigned char*p=c.data+8+(size_t)i*stride;
        unsigned name_offset=stride==28?4:0;
        uint32_t name=u32(p+name_offset);
        if(name<8+(size_t)count*stride||!string_at(c,name)||u32(p+name_offset+12)>3)return 0;
    }
    return 1;
}
/* This deliberately accepts the retail's SM5 STAT148 schema only. A valid
 * other compiler schema does not receive a fabricated partial description. */
static int parse(shader_object *o){
    const unsigned char*b=o->bytes;size_t n=o->size;
    if(n<32||memcmp(b,"DXBC",4)||u32(b+20)!=1||u32(b+24)!=n)return 0;
    uint32_t count=u32(b+28);if(count>256||!range(n,32,(size_t)count*4))return 0;
    chunk rdef={0},stat={0},code={0},input={0},output={0},patch={0};
    unsigned output_stride=24;
    for(uint32_t i=0;i<count;++i){uint32_t off=u32(b+32+i*4);
        if(off<32+(size_t)count*4||!range(n,off,8))return 0;
        uint32_t size=u32(b+off+4);if(!range(n,(size_t)off+8,size))return 0;
        for(uint32_t j=0;j<i;++j){uint32_t other=u32(b+32+j*4),other_size=u32(b+other+4);
            if((uint64_t)off<((uint64_t)other+8+other_size)&&(uint64_t)other<((uint64_t)off+8+size))return 0;}
        chunk c={b+off+8,size};chunk *slot=NULL;
        switch(u32(b+off)){
        case FOUR('R','D','E','F'):slot=&rdef;break;
        case FOUR('S','T','A','T'):slot=&stat;break;
        case FOUR('S','H','E','X'):case FOUR('S','H','D','R'):slot=&code;break;
        case FOUR('I','S','G','N'):slot=&input;break;
        case FOUR('O','S','G','N'):slot=&output;output_stride=24;break;
        case FOUR('O','S','G','5'):slot=&output;output_stride=28;break;
        case FOUR('P','C','S','G'):slot=&patch;break;
        case FOUR('I','S','G','1'):case FOUR('O','S','G','1'):case FOUR('P','S','G','1'):
            return 0; /* unsupported precision schema; never succeeds partially */
        default:break;
        }
        if(slot){if(slot->data)return 0;*slot=c;}
    }
    if(!rdef.data||rdef.size<28||!stat.data||stat.size!=148||!code.data||code.size<8||
       code.size%4||u32(code.data+4)!=code.size/4||!signature(input,24)||!signature(output,output_stride)||
       (patch.data&&!signature(patch,24)))return 0;
    uint32_t version=u32(code.data);if((version&0xffff)!=0x50||(version>>16)>5)return 0;
    const char*creator=string_at(rdef,u32(rdef.data+24));if(!creator)return 0;
    /* RDEF table headers must refer to existing complete SM5 records. */
    if(!range(rdef.size,u32(rdef.data+4),(size_t)u32(rdef.data)*24)||
       !range(rdef.size,u32(rdef.data+12),(size_t)u32(rdef.data+8)*32))return 0;
    memset(o->desc,0,sizeof(o->desc));put32(o->desc,version);put64(o->desc+8,(uintptr_t)creator);
    put32(o->desc+16,u32(rdef.data+20));put32(o->desc+20,u32(rdef.data));put32(o->desc+24,u32(rdef.data+8));
    put32(o->desc+28,u32(input.data));put32(o->desc+32,u32(output.data));
    static const unsigned char stat_map[]={0,1,10,2,3,14,15,16,17,18,4,5,6,7,8,9,11,12,13,24,25,23};
    for(unsigned i=0;i<sizeof(stat_map);++i)put32(o->desc+36+i*4,u32(stat.data+stat_map[i]*4));
    put32(o->desc+124,patch.data?u32(patch.data):0);
    for(unsigned i=0;i<8;++i)put32(o->desc+128+i*4,u32(stat.data+(29+i)*4));
    o->input=input;return 1;
}
static shader_object **find(uint64_t p){shader_object**s=&objects;while(*s&&(*s)->guest!=p)s=&(*s)->next;return s;}
static void ensure_vtable(void){if(vtable)return;vtable=win64_alloc(WIN64_SHADER_SENTINEL_COUNT*8,0);if(!vtable)stop("vtable allocation");
    unsigned char*p=win64_pointer(vtable,WIN64_SHADER_SENTINEL_COUNT*8,1);
    for(unsigned i=0;i<WIN64_SHADER_SENTINEL_COUNT;++i)put64(p+i*8,WIN64_SHADER_SENTINEL_BASE+i*8);
}
int win64_shader_call(CPU*c,const char*name){
    if(strcmp(name,"D3DReflect"))return 0;
    uint64_t source=win64_argument(c,0),bytes=win64_argument(c,1),iid=win64_argument(c,2),out=win64_argument(c,3);
    if(!iid||!out){c->rax=E_INVALIDARG;return 1;}
    const void*identity=win64_pointer(iid,16,0);void*result=win64_pointer(out,8,1);
    if(memcmp(identity,reflection_iid,16)){c->rax=E_INVALIDARG;return 1;}
    if(!source||bytes<32||bytes>64*1024*1024){c->rax=INVALID_DXBC;return 1;}
    const void*data=win64_pointer(source,(size_t)bytes,0);shader_object*o=calloc(1,sizeof(*o));
    if(!o){c->rax=E_OUTOFMEMORY;return 1;}o->bytes=malloc((size_t)bytes);o->size=(size_t)bytes;
    if(!o->bytes){free(o);c->rax=E_OUTOFMEMORY;return 1;}memcpy(o->bytes,data,(size_t)bytes);
    if(memcmp(o->bytes,"DXBC",4)||u32(o->bytes+20)!=1||u32(o->bytes+24)!=o->size){free(o->bytes);free(o);c->rax=INVALID_DXBC;return 1;}
    uint32_t checksum[4];shader_dxbc_checksum(o->bytes,(uint32_t)o->size,checksum);
    if(memcmp(checksum,o->bytes+4,16)){free(o->bytes);free(o);c->rax=E_FAIL;return 1;}
    if(!parse(o)){free(o->bytes);free(o);c->rax=INVALID_DXBC;return 1;}
    LOCK();ensure_vtable();o->guest=win64_alloc(8,0);if(!o->guest){UNLOCK();free(o->bytes);free(o);c->rax=E_OUTOFMEMORY;return 1;}
    put64(win64_pointer(o->guest,8,1),vtable);win64_region_add((uintptr_t)o->bytes,o->size,0);
    o->refs=1;o->next=objects;objects=o;put64(result,o->guest);UNLOCK();c->rax=0;return 1;
}
int win64_shader_import(CPU*c,const char*dll,const char*name){
    static const char expected[]="d3dcompiler_47.dll";
    for(unsigned i=0;i<sizeof(expected);++i){unsigned a=(unsigned char)dll[i];if(a>='A'&&a<='Z')a+='a'-'A';if(a!=(unsigned char)expected[i])return 0;}
    return win64_shader_call(c,name);
}
int win64_shader_dispatch(CPU*c,uint64_t target){
    if(target<WIN64_SHADER_SENTINEL_BASE||target>=WIN64_SHADER_SENTINEL_BASE+WIN64_SHADER_SENTINEL_COUNT*8||((target-WIN64_SHADER_SENTINEL_BASE)&7))return 0;
    unsigned slot=(unsigned)(target-WIN64_SHADER_SENTINEL_BASE)/8;uint64_t guest=win64_argument(c,0);
    LOCK();shader_object**link=find(guest),*o=*link;if(!o)stop("foreign or released COM identity");
    if(u64(win64_pointer(guest,8,0))!=vtable)stop("changed COM vtable");
    if(slot==0){uint64_t iid=win64_argument(c,1),out=win64_argument(c,2);
        if(!iid||!out)c->rax=E_INVALIDARG;
        else{void*p=win64_pointer(out,8,1);const void*id=win64_pointer(iid,16,0);
            if(!memcmp(id,reflection_iid,16)||!memcmp(id,unknown_iid,16)){if(o->refs==UINT32_MAX)stop("reference overflow");++o->refs;put64(p,guest);c->rax=0;}
            else{put64(p,0);c->rax=E_NOINTERFACE;}}
    }else if(slot==1){if(o->refs==UINT32_MAX)stop("reference overflow");c->rax=++o->refs;
    }else if(slot==2){c->rax=--o->refs;if(!o->refs){*link=o->next;win64_region_remove((uintptr_t)o->bytes);free(o->bytes);win64_free(o->guest);free(o);}
    }else if(slot==3){uint64_t out=win64_argument(c,1);if(!out)c->rax=E_FAIL;
        else{unsigned char*p=win64_pointer(out,160,1);/* SDK leaves alignment padding unspecified. */memcpy(p,o->desc,4);memcpy(p+8,o->desc+8,152);c->rax=0;}
    }else if(slot==7){uint32_t index=(uint32_t)win64_argument(c,1);uint64_t out=win64_argument(c,2);
        if(index>=u32(o->input.data)||!out)c->rax=E_INVALIDARG;
        else{const unsigned char*entry=o->input.data+8+(size_t)index*24;unsigned char*p=win64_pointer(out,40,1);
            memset(p,0,40);put64(p,(uintptr_t)string_at(o->input,u32(entry)));put32(p+8,u32(entry+4));
            put32(p+12,u32(entry+16));put32(p+16,u32(entry+8));put32(p+20,u32(entry+12));
            p[24]=entry[20];p[25]=entry[21];c->rax=0;}
    }else{char message[96];snprintf(message,sizeof(message),"unimplemented actual COM method slot%u",slot);stop(message);}
    UNLOCK();return 1;
}
void win64_shader_self_test(void){
    /* Full original shader/oracle fixture is external, not an invented shader. */
    CPU c={0},before;uint64_t out=UINT64_C(0x123456789abcdef0);unsigned char iid[16];memcpy(iid,reflection_iid,16);
    win64_region_add((uintptr_t)iid,16,0);win64_region_add((uintptr_t)&out,8,1);
    c.rcx=0;c.rdx=0;c.r8=(uintptr_t)iid;c.r9=(uintptr_t)&out;c.rsp=0x12345678;c.rbx=0xabcdef;c.cf=1;before=c;
    if(!win64_shader_call(&c,"D3DReflect")||c.rax!=INVALID_DXBC||out!=UINT64_C(0x123456789abcdef0))stop("invalid-bytecode self-test");
    c.rax=before.rax;if(memcmp(&c,&before,sizeof(c)))stop("Win64 ABI self-test");
    win64_region_remove((uintptr_t)&out);win64_region_remove((uintptr_t)iid);
    fprintf(stderr,"SHADER reflection bounded self-test PASS (no game rendering claim)\n");
}
