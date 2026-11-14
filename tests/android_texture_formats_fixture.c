/* Host executes only two pinned original integer leaves as an oracle. ARM
 * executes static C exclusively, replaying the host oracle's dataset. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "texture_formats.h"
#include "win64.h"
typedef struct {uint64_t va;size_t size;const unsigned char *bytes;} texture_site;
#include "texture_format_sites.inc"
#include "canonical_bodies.inc"
int64_t g_image_delta;
static _Thread_local uint64_t native_tls=UINT64_C(0x123456789abcdef0);
static _Thread_local struct {uint64_t base;size_t bytes;int write;} spans[8];
static void span(unsigned n,void *base,size_t bytes,int write){spans[n].base=(uintptr_t)base;spans[n].bytes=bytes;spans[n].write=write;}
void *win64_pointer(uint64_t address,size_t bytes,int writable){
    for(unsigned i=0;i<8;i++)if(address>=spans[i].base&&bytes<=spans[i].bytes&&address-spans[i].base<=spans[i].bytes-bytes&&(!writable||spans[i].write))return(void*)(uintptr_t)address;
    fputs("STOP texture fixture pointer boundary\n",stderr);_Exit(73);
}
void dispatch(CPU *c,uint64_t target){(void)c;fprintf(stderr,"Unexpected canonical tail %"PRIx64"\n",target);abort();}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
void frostpunk_unsupported(uint64_t va,const char *text){fprintf(stderr,"Unexpected canonical instruction %"PRIx64" %s\n",va,text);abort();}
static void w16(void *p,uint16_t v){memcpy(p,&v,2);}
static void w32(void *p,uint32_t v){memcpy(p,&v,4);}
static void w64(void *p,uint64_t v){memcpy(p,&v,8);}
static uint32_t rng(uint32_t *v){*v^=*v<<13;*v^=*v>>17;*v^=*v<<5;return *v;}
static const size_t image_bytes=0x1000000;
static unsigned char *image;
static unsigned init_image(void){
#ifdef _WIN32
    image=VirtualAlloc((void*)(uintptr_t)UINT64_C(0x140000000),image_bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    image=mmap((void*)(uintptr_t)UINT64_C(0x140000000),image_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    assert((uintptr_t)image==UINT64_C(0x140000000));w16(image,0x5a4d);w32(image+0x3c,0x80);memcpy(image+0x80,"PE\0\0",4);
    w16(image+0x84,0x8664);w16(image+0x86,1);w16(image+0x94,0xf0);w16(image+0x98,0x20b);
    w64(image+0xb0,UINT64_C(0x140000000));w32(image+0xd0,(uint32_t)image_bytes);
    w32(image+0x190,(uint32_t)image_bytes);w32(image+0x1ac,0x20000000);
    for(unsigned i=0;i<2;i++)memcpy(image+texture_sites[i].va-UINT64_C(0x140000000),texture_sites[i].bytes,texture_sites[i].size);
    g_image_delta=(intptr_t)image-INT64_C(0x140000000);assert(fp_android_texture_formats_init(image,image_bytes));
    unsigned gates=0;
    for(unsigned i=0;i<2;i++)for(size_t n=0;n<texture_sites[i].size;n++){
        unsigned char *p=image+texture_sites[i].va-UINT64_C(0x140000000)+n;*p^=1;
        assert(!fp_android_texture_formats_init(image,image_bytes));assert(!fp_android_texture_formats_lookup(texture_sites[i].va));
        *p^=1;++gates;
    }
    w32(image+0x1ac,0);assert(!fp_android_texture_formats_init(image,image_bytes));w32(image+0x1ac,0x20000000);
    w64(image+0xb0,0);assert(!fp_android_texture_formats_init(image,image_bytes));w64(image+0xb0,UINT64_C(0x140000000));
    assert(!fp_android_texture_formats_init(image,63));assert(!fp_android_texture_formats_init(NULL,image_bytes));
    w32(image+0xd0,0xf49c23);assert(!fp_android_texture_formats_init(image,image_bytes));w32(image+0xd0,(uint32_t)image_bytes);
    assert(fp_android_texture_formats_init(image,image_bytes));assert(!fp_android_texture_formats_lookup(UINT64_C(0x140d95651)));
    return gates;
}
typedef struct {uint32_t format,width,height,srgb,mapping,row,bytes;} record;
static int is_private(uint32_t format){return format==FP_ASTC4_D3DFMT||format==FP_ASTC6_D3DFMT;}
static unsigned char stack[512];
static uint32_t outputs[4];
static void seed(CPU *c,record r,int layout){
    uint32_t state=r.format^r.width*137^r.height*917^r.srgb;unsigned char *bytes=(unsigned char*)c;
    for(size_t n=0;n<sizeof(*c);n++)bytes[n]=(unsigned char)rng(&state);
    c->mxcsr=0x1f80;c->rsp=((uintptr_t)(stack+192)&~UINT64_C(15))+8;c->rcx=r.format;
    c->rip=layout?UINT64_C(0x140f499a0):UINT64_C(0x140d95650);
    c->rdx=layout?r.width:((uint64_t)rng(&state)<<32)|r.srgb;c->r8=r.height;c->r9=(uintptr_t)&outputs[0];
    w64((void*)(uintptr_t)c->rsp,UINT64_C(0xe5ef00d8));w64((void*)(uintptr_t)(c->rsp+40),(uintptr_t)&outputs[1]);
    span(0,stack,sizeof(stack),1);span(1,outputs,sizeof(outputs),1);
    outputs[0]=outputs[1]=0xcccccccc;outputs[2]=0x12345678;outputs[3]=0xabcdef01;
}
static void test_record(record r,unsigned *old,unsigned *private_cases){
    for(int layout=0;layout<2;layout++){
        CPU cpu,wanted;seed(&cpu,r,layout);wanted=cpu;
        if(is_private(r.format)){
            wanted.rsp+=8;if(!layout)wanted.rax=r.mapping;
        }else{
            if(layout)L_000140F499A0(&wanted);else L_000140D95650(&wanted);
            seed(&cpu,r,layout);
        }
        errno=33;native_tls=UINT64_C(0x123456789abcdef0);
        fp_android_texture_formats_lookup(cpu.rip)(&cpu);
        assert(!memcmp(&cpu,&wanted,sizeof(cpu)));assert(errno==33&&native_tls==UINT64_C(0x123456789abcdef0));
        if(layout)assert(outputs[0]==r.bytes&&outputs[1]==r.row&&outputs[2]==0x12345678&&outputs[3]==0xabcdef01);
        else assert(cpu.rax==r.mapping);
    }
    if(is_private(r.format))++*private_cases;else ++*old;
}
static void negative(const char *name){
    record r={FP_ASTC4_D3DFMT,256,256,0,FP_ASTC4_DXGI_UNORM,1024,65536};CPU c;seed(&c,r,1);
    if(!strcmp(name,"width0"))c.rdx=0;
    else if(!strcmp(name,"height0"))c.r8=0;
    else if(!strcmp(name,"rowoverflow"))c.rdx=UINT32_MAX;
    else if(!strcmp(name,"sizeoverflow")){c.rdx=1;c.r8=UINT32_MAX;}
    else if(!strcmp(name,"badstack"))c.rsp=1;
    else if(!strcmp(name,"stackoverflow"))c.rsp=UINT64_MAX-8;
    else if(!strcmp(name,"nullsize"))c.r9=0;
    else if(!strcmp(name,"nullrow"))w64((void*)(uintptr_t)(c.rsp+40),0);
    else if(!strcmp(name,"readonly"))spans[1].write=0;
    else if(!strcmp(name,"srgb2")){seed(&c,r,0);c.rdx=2;}
    else if(!strcmp(name,"mapstack")){seed(&c,r,0);c.rsp=1;}
    else abort();
    fp_android_texture_formats_lookup(c.rip)(&c);abort();
}
int main(int argc,char **argv){
    unsigned gates=init_image();if(argc==3&&!strcmp(argv[1],"--negative"))negative(argv[2]);
    assert(argc==3);unsigned old=0,private_cases=0,count=0;
    if(!strcmp(argv[1],"--dataset")){
        FILE *f=fopen(argv[2],"rb");assert(f);uint32_t header[2];assert(fread(header,sizeof(header),1,f)==1&&header[0]==0x46544153&&header[1]==2304);
        record r;for(unsigned i=0;i<header[1];i++){assert(fread(&r,sizeof(r),1,f)==1);test_record(r,&old,&private_cases);++count;}
        assert(fgetc(f)==EOF);fclose(f);
    }
#ifdef _WIN32
    else if(!strcmp(argv[1],"--oracle")){
        for(unsigned i=0;i<2;i++){DWORD previous;assert(VirtualProtect(image+texture_sites[i].va-UINT64_C(0x140000000),texture_sites[i].size,PAGE_EXECUTE_READ,&previous));}
        uint32_t(*native_map)(uint32_t,int)=(void*)(image+0xd95650);
        void(*native_size)(uint32_t,uint32_t,uint32_t,uint32_t*,uint32_t*)=(void*)(image+0xf499a0);
        uint32_t formats[]={0x31545844,0x33545844,0x35545844,21,28,111,FP_ASTC4_D3DFMT,FP_ASTC6_D3DFMT};
        uint32_t widths[]={1,2,4,5,6,7,13,31,125,256,513,1024};
        FILE *f=fopen(argv[2],"wb");assert(f);uint32_t header[]={0x46544153,2304};assert(fwrite(header,sizeof(header),1,f)==1);
        for(unsigned k=0;k<8;k++)for(unsigned w=0;w<12;w++)for(unsigned h=0;h<12;h++)for(unsigned s=0;s<2;s++){
            record r={formats[k],widths[w],widths[h],s,0,0,0};
            if(k<6){r.mapping=native_map(r.format,s);native_size(r.format,r.width,r.height,&r.bytes,&r.row);}
            else{unsigned block=k==6?4:6;r.mapping=k==6?(s?FP_ASTC4_DXGI_SRGB:FP_ASTC4_DXGI_UNORM):(s?FP_ASTC6_DXGI_SRGB:FP_ASTC6_DXGI_UNORM);r.row=((r.width+block-1)/block)*16;r.bytes=r.row*((r.height+block-1)/block);}
            test_record(r,&old,&private_cases);assert(fwrite(&r,sizeof(r),1,f)==1);++count;
        }fclose(f);
    }
#endif
    else abort();
    assert(old==1728&&private_cases==576&&count==2304&&gates==1292);
    // Partial output alias retains the original row-then-size order.
    record r={FP_ASTC6_D3DFMT,13,7,0,FP_ASTC6_DXGI_UNORM,48,96};CPU c;seed(&c,r,1);
    w64((void*)(uintptr_t)(c.rsp+40),(uintptr_t)&outputs[0]);fp_android_texture_formats_lookup(c.rip)(&c);assert(outputs[0]==96);
    printf("{\"success\":true,\"old_native_oracle_records\":%u,\"ASTC_records\":%u,\"full_CPU_calls\":%u,\"opcode_mutations\":%u,\"errno_TLS_preserved\":true,\"output_alias_order\":true}\n",old,private_cases,count*2,gates);
    return 0;
}
