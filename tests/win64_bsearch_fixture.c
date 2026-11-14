/* Native UCRT traces + exact lifted comparator; no original game startup. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include "win64.h"
#include "win64_utility.h"
#include <stdio.h>
#include <errno.h>
static _Thread_local int crt_errno;
static _Thread_local uint32_t guest_error;
typedef struct {uint64_t address;size_t size;int write;} host_region;
static _Thread_local host_region host_regions[32];
void win64_region_add(uint64_t address,size_t size,int writable){for(unsigned i=0;i<32;++i)if(!host_regions[i].size){host_regions[i]=(host_region){address,size,writable};return;}abort();}
void win64_region_remove(uint64_t address){for(unsigned i=0;i<32;++i)if(host_regions[i].address==address)host_regions[i].size=0;}
void *win64_pointer(uint64_t address,size_t bytes,int writable){for(unsigned i=0;i<32;++i){host_region *r=&host_regions[i];if(r->size&&address>=r->address&&bytes<=r->size&&address-r->address<=r->size-bytes&&(!writable||r->write))return(void*)(uintptr_t)address;}fputs("STOP fixture host pointer bounds\n",stderr);_Exit(73);}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4)return i==0?c->rcx:i==1?c->rdx:i==2?c->r8:c->r9;uint64_t v;memcpy(&v,win64_pointer(c->rsp+40+(i-4)*8,8,0),8);return v;}
uint32_t win64_get_error(void){return guest_error;}
void win64_set_error(uint32_t value){guest_error=value;}
void win64_stdio_set_errno(int value){crt_errno=value;}
const char *win64_string_a(uint64_t address){(void)address;abort();}
#else
#include "../android/host/win64.c"
#include "win64_utility.h"
static _Thread_local unsigned char bsearch_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(bsearch_teb))abort();memcpy(&v,bsearch_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(bsearch_teb))abort();memcpy(bsearch_teb+offset,&value,bytes);}
#endif
#include <inttypes.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4702) /* Exact AOT function retains unreachable padding. */
#endif
#include "bsearch_original_comparator.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#pragma pack(push,1)
typedef struct {uint32_t kind,key;uint64_t count,width;uint32_t flags,mode;} input_case;
typedef struct {int32_t index;uint32_t record;int32_t error;uint32_t last_error,callbacks,invalid,exception,traces;int16_t trace[16];} output_case;
typedef struct {input_case in;output_case out;} record;
#pragma pack(pop)
_Static_assert(sizeof(record)==96,"record96");
static unsigned char original_guard[2664+16];static record *records;static uint32_t record_count;
static int one(const record*,unsigned,int,int);
typedef struct callback_context{CPU parent;const record *record;uint64_t key,base;unsigned calls;int16_t trace[16];int corrupt,reenter;uint64_t effects[3];struct callback_context *previous;} callback_context;
static _Thread_local callback_context *active;
static void fail(const char *why){fprintf(stderr,"BSEARCH_FIXTURE_FAIL %s\n",why);exit(2);}
static void need(int ok,const char *why){if(!ok)fail(why);}
void dispatch(CPU *c,uint64_t function){
    callback_context *x=active;if(!x){fputs("STOP fixture unsupported procedure\n",stderr);_Exit(73);}
    uint64_t frame=(x->parent.rsp-48)&~UINT64_C(15);CPU wanted=x->parent;wanted.rcx=x->key;wanted.rdx=c->rdx;wanted.rsp=frame-8;
    need(!memcmp(c,&wanted,sizeof(*c)),"full callback entry CPU and opaque key");
    need(c->rdx>=x->base&&(c->rdx-x->base)%x->record->in.width==0,"pivot aligned within array");
    unsigned index=(unsigned)((c->rdx-x->base)/x->record->in.width);need(index<x->record->in.count&&x->calls<16,"pivot/count bounds");x->trace[x->calls++]=(int16_t)index;
    need(rd64(c->rsp)==UINT64_C(0xe5ef00d0)&&(c->rsp&15)==8,"return sentinel and ABI stack alignment");
    for(unsigned j=0;j<32;++j)((unsigned char*)win64_pointer(c->rsp+8,32,1))[j]=(unsigned char)(0x40+j);
    ++*(uint64_t*)win64_pointer((uintptr_t)&x->effects[1],8,1);
    if(x->reenter&&x->calls==1){need(one(&records[record_count-1],0,0,0),"nested bsearch context");}
    if(x->record->in.flags&8){crt_errno=2345;win64_set_error(1234);errno=3141;}
    if(function==UINT64_C(0x140e1fb90)){
        L_000140E1FB90(c);
        if((int32_t)(uint32_t)c->rax<0)need(c->rax==UINT64_C(0xffffffff),"actual AOT EAX negative zero extension");
    }else if(function==UINT64_C(0xe5e70010)){
        int result=0;if(x->record->in.mode!=2){uint32_t a,b;memcpy(&a,win64_pointer(c->rcx,4,0),4);memcpy(&b,win64_pointer(c->rdx,4,0),4);if(a<b)result=x->record->in.mode?INT32_MIN:-1;else if(a>b)result=x->record->in.mode?INT32_MAX:1;}
        uint64_t rsp=c->rsp;memset(c,0x6d,sizeof(*c));c->rax=UINT64_C(0xfedcba9800000000)|(uint32_t)result;c->rsp=rsp+8;
    }else{fputs("STOP fixture unsupported procedure\n",stderr);_Exit(73);}
    if(x->corrupt==1)c->rsp+=8;
    if(x->corrupt==2)wr64(frame-8,0);
}
static int one(const record *r,unsigned serial,int parent_pop,int reenter){
    _Alignas(16) unsigned char stack[640];unsigned char array[4112],snapshot[4112];struct{uint16_t left,key,right;} key16={0x5aa5,(uint16_t)r->in.key,0xa55a};uint32_t key32=r->in.key;
    memset(stack,0x6b,sizeof(stack));memset(array,0xa5,sizeof(array));
    if(!r->in.kind)memcpy(array,original_guard,sizeof(original_guard));
    unsigned char *base=array+8;size_t array_bytes=r->in.kind?sizeof(array):sizeof(original_guard);
    if(r->in.kind)for(size_t j=0;j<r->in.count;++j){uint32_t v=r->in.kind==3?1:(uint32_t)(r->in.kind==2?j/3:j)*2+1;memcpy(base+j*r->in.width,&v,4);}
    memcpy(snapshot,array,array_bytes);
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=r->in.flags&1?0:(uintptr_t)(r->in.kind?(void*)&key32:(void*)&key16.key);c.rdx=r->in.flags&2?0:(uintptr_t)base;c.r8=r->in.count;c.r9=r->in.width;c.rsp=(uintptr_t)(stack+392);c.rip=UINT64_C(0x140e1fea4);c.mxcsr=0x1f80|((serial&3)<<13)|((serial&4)?0x40:0)|((serial&8)?0x8000:0)|((serial&16)?0x3f:0);
    uint64_t function=r->in.kind?UINT64_C(0xe5e70010):UINT64_C(0x140e1fb90);memcpy(stack+432,&function,8);before=c;
    callback_context ctx={0};ctx.parent=c;ctx.record=r;ctx.key=c.rcx;ctx.base=(uintptr_t)base;ctx.reenter=reenter;ctx.effects[0]=UINT64_C(0xaabbccddeeff0011);ctx.effects[2]=~ctx.effects[0];ctx.previous=active;active=&ctx;
    win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)array,array_bytes,0);win64_region_add((uintptr_t)(r->in.kind?(void*)&key32:(void*)&key16.key),r->in.kind?4:2,0);win64_region_add((uintptr_t)ctx.effects,sizeof(ctx.effects),1);
    crt_errno=1927;win64_set_error(0x5a17);errno=2718;
#ifdef _WIN32
    unsigned native_control=_mm_getcsr();
#else
    uint64_t native_control,native_status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(native_control),"=r"(native_status));
#endif
    need(win64_utility_call(&c,"bsearch"),"handler accepted");uint64_t found=c.rax;
    need(c.rsp==before.rsp,"callee does not pop parent RSP");if(parent_pop)c.rsp+=8;
    c.rax=before.rax;if(parent_pop)before.rsp+=8;
    int bad=memcmp(&c,&before,sizeof(c))||ctx.calls!=r->out.callbacks||memcmp(ctx.trace,r->out.trace,ctx.calls*2)||crt_errno!=r->out.error||win64_get_error()!=r->out.last_error||errno!=((r->in.flags&8)&&ctx.calls?3141:2718);
    if(found!=(r->out.index<0?0:(uintptr_t)(base+(uint64_t)r->out.index*r->in.width)))bad=1;
    if(found){uint32_t value;memcpy(&value,(void*)(uintptr_t)found,4);if(value!=r->out.record)bad=1;}
    if(memcmp(snapshot,array,array_bytes)||key16.left!=0x5aa5||key16.right!=0xa55a||ctx.effects[0]!=UINT64_C(0xaabbccddeeff0011)||ctx.effects[2]!=~ctx.effects[0]||ctx.effects[1]!=ctx.calls)bad=1;
    uintptr_t frame=((uintptr_t)(stack+392)-48)&~(uintptr_t)15;
    for(unsigned j=0;j<sizeof(stack);++j){uintptr_t p=(uintptr_t)(stack+j);if(p>=frame-8&&p<frame+32)continue;unsigned char expected=0x6b;if(j>=432&&j<440)expected=((unsigned char*)&function)[j-432];if(stack[j]!=expected)bad=1;}
#ifdef _WIN32
    if(_mm_getcsr()!=native_control)bad=1;
#else
    uint64_t nc,ns;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(nc),"=r"(ns));if(nc!=native_control||ns!=native_status)bad=1;
#endif
    if(bad){fprintf(stderr,"BSEARCH_MISMATCH row=%u kind=%u n=%"PRIu64" w=%"PRIu64" key=%u got=%"PRIx64" idx=%d calls=%u/%u guesterrno=%d/%d LastError=%u/%u hosterrno=%d\n",serial,r->in.kind,r->in.count,r->in.width,r->in.key,found,r->out.index,ctx.calls,r->out.callbacks,crt_errno,r->out.error,win64_get_error(),r->out.last_error,errno);return 0;}
    active=ctx.previous;win64_region_remove((uintptr_t)stack);win64_region_remove((uintptr_t)array);win64_region_remove((uintptr_t)(r->in.kind?(void*)&key32:(void*)&key16.key));win64_region_remove((uintptr_t)ctx.effects);return 1;
}
static int negative(const char *name){
    _Alignas(16) uint64_t stack[80];uint32_t array[4]={1,3,5,7},key=3;CPU c={0};memset(stack,0x6b,sizeof(stack));c.rsp=(uintptr_t)&stack[49];c.rcx=(uintptr_t)&key;c.rdx=(uintptr_t)array;c.r8=4;c.r9=4;stack[54]=UINT64_C(0xe5e70010);
    win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)array,sizeof(array),0);win64_region_add((uintptr_t)&key,4,0);
    record r={0};r.in.kind=1;r.in.count=4;r.in.width=4;callback_context ctx={0};ctx.parent=c;ctx.record=&r;ctx.key=c.rcx;ctx.base=c.rdx;ctx.effects[0]=0x123;ctx.effects[2]=~ctx.effects[0];active=&ctx;win64_region_add((uintptr_t)ctx.effects,sizeof(ctx.effects),1);
    if(!strcmp(name,"--zero-width"))c.r9=0;else if(!strcmp(name,"--null-base"))c.rdx=0;else if(!strcmp(name,"--null-compare"))stack[54]=0;else if(!strcmp(name,"--span-overflow")){c.r8=UINT64_MAX;c.r9=8;}else if(!strcmp(name,"--address-overflow"))c.rdx=UINT64_MAX-4;else if(!strcmp(name,"--array-bounds"))c.r8=1024;else if(!strcmp(name,"--frame-bounds")){win64_region_remove((uintptr_t)stack);win64_region_add((uintptr_t)&stack[49],48,1);}else if(!strcmp(name,"--frame-readonly")){win64_region_remove((uintptr_t)stack);win64_region_add((uintptr_t)stack,sizeof(stack),0);}else if(!strcmp(name,"--unsupported-procedure"))stack[54]=UINT64_C(0x12345678);else if(!strcmp(name,"--callback-pop"))ctx.corrupt=1;else if(!strcmp(name,"--callback-sentinel"))ctx.corrupt=2;else return 3;
    ctx.parent=c;ctx.key=c.rcx;ctx.base=c.rdx;win64_utility_call(&c,"bsearch");return 99;
}
#ifdef _WIN32
static DWORD WINAPI worker(void *argument){
#else
static void *worker(void *argument){
#endif
    unsigned start=(unsigned)(uintptr_t)argument;for(unsigned j=0;j<2048;++j)need(one(&records[(start+j*17)%record_count],j,j&1,0),"parallel case");return 0;
}
int main(int argc,char **argv){
    need(argc==2,"dataset or negative");if(argv[1][0]=='-')return negative(argv[1]);
    FILE *f=fopen(argv[1],"rb");need(f!=NULL,"open data");char magic[8];need(fread(magic,1,8,f)==8&&!memcmp(magic,"FPBSR001",8),"magic");need(fread(&record_count,4,1,f)==1&&record_count>65536&&record_count<100000,"case count");memset(original_guard,0xa5,sizeof(original_guard));need(fread(original_guard+8,1,2664,f)==2664,"table");records=calloc(record_count,sizeof(*records));need(records&&fread(records,sizeof(*records),record_count,f)==record_count&&fgetc(f)==EOF&&!fclose(f),"records");
#ifdef _WIN32
    unsigned initial_control=_mm_getcsr();_mm_setcsr(0x1f80|0x4000|0x8040|0x25);
#else
    uint64_t initial_control,initial_status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(initial_control),"=r"(initial_status));uint64_t seed_control=(UINT64_C(1)<<24)|(UINT64_C(2)<<22),seed_status=0x9f;__asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(seed_control),"r"(seed_status):"memory");
#endif
    for(unsigned j=0;j<record_count;++j)need(one(&records[j],j,j&1,0),"all native cases");
    for(unsigned j=0;j<32;++j)need(one(&records[65536+84],j,j&1,1),"nested callback");
    record revised=records[0];for(unsigned j=0;j<65536;++j)if(records[j].out.index>=0){revised=records[j];break;}need(revised.out.index>=0,"mutable mapped record");uint32_t prior;memcpy(&prior,original_guard+8+revised.out.index*4,4);uint32_t changed=prior^UINT32_C(0x5a5a0000);memcpy(original_guard+8+revised.out.index*4,&changed,4);revised.out.record=changed;need(one(&revised,record_count,0,0),"table writable mapping notcached");memcpy(original_guard+8+revised.out.index*4,&prior,4);
#ifdef _WIN32
    HANDLE threads[4];for(unsigned j=0;j<4;++j){threads[j]=CreateThread(NULL,0,worker,(void*)(uintptr_t)j,0,NULL);need(threads[j]!=NULL,"thread");}need(WaitForMultipleObjects(4,threads,TRUE,60000)==WAIT_OBJECT_0,"join");for(unsigned j=0;j<4;++j)need(CloseHandle(threads[j]),"close thread");
#else
    pthread_t threads[4];for(unsigned j=0;j<4;++j)need(!pthread_create(&threads[j],NULL,worker,(void*)(uintptr_t)j),"thread");for(unsigned j=0;j<4;++j)need(!pthread_join(threads[j],NULL),"join");
#endif
#ifdef _WIN32
    _mm_setcsr(initial_control);
#else
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(initial_control),"r"(initial_status):"memory");
#endif
    printf("{\"success\":true,\"native_cases\":%u,\"original_keys\":65536,\"original_records\":666,\"parallel_calls\":8192,\"reentrant_cases\":32,\"mutable_table_mapping\":1,\"full_CPU\":true,\"stack_shadow_canaries\":true,\"synthetic_parent_pop\":1,\"actual_AOT_comparator\":true,\"host_FP_errno\":true}\n",record_count);free(records);return 0;
}
