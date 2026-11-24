/* Explicit private ASTC and ETC2 formats; never reinterpret a standard BC format. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texture_formats.h"
#include "win64.h"

int frostpunk_rdvk_active(void);   /* android/vulkan/rdvk_runtime.c */
void L_000140D95650(CPU *c);
void L_000140F499A0(CPU *c);
typedef struct { uint64_t va; size_t size; const unsigned char *bytes; } texture_site;
#include "texture_format_sites.inc"
static int verified;
static _Noreturn void texture_stop(const char *reason) {
    fprintf(stderr,"STOP private texture format %s\n",reason);fflush(stderr);_Exit(73);
}
static uint16_t u16(const unsigned char *p) {uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t u32(const unsigned char *p) {uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t u64(const unsigned char *p) {uint64_t v;memcpy(&v,p,8);return v;}
static int fits(size_t at,size_t bytes,size_t size) {return at<=size&&bytes<=size-at;}
static unsigned private_block(uint64_t format) {
    return (uint32_t)format==FP_ASTC4_D3DFMT?4:(uint32_t)format==FP_ASTC6_D3DFMT?6:0;
}
/* Bytes of one 4x4 ETC2 block, 0 for another format. */
static unsigned etc2_bytes(uint64_t format) {
    return (uint32_t)format==FP_ETC2_RGB_D3DFMT?8:(uint32_t)format==FP_ETC2_RGBA_D3DFMT?16:0;
}

int fp_android_texture_formats_init(const void *mapped_pe,size_t image_size) {
    verified=0;const unsigned char *base=mapped_pe;
    if(!base||!fits(0,64,image_size)||u16(base)!=0x5a4d)return 0;
    uint32_t pe=u32(base+0x3c);
    if(!fits(pe,24,image_size)||memcmp(base+pe,"PE\0\0",4)||u16(base+pe+4)!=0x8664)return 0;
    uint16_t count=u16(base+pe+6),optional_size=u16(base+pe+20);
    size_t optional=(size_t)pe+24,sections=optional+optional_size;
    if(!count||count>96||optional_size<112||!fits(optional,optional_size,image_size)||
       u16(base+optional)!=0x20b||u64(base+optional+24)!=UINT64_C(0x140000000)||
       !fits(sections,(size_t)count*40,image_size))return 0;
    uint32_t declared=u32(base+optional+56);
    if(!declared||declared>image_size)return 0;
    for(size_t i=0;i<sizeof(texture_sites)/sizeof(*texture_sites);++i) {
        const texture_site *site=&texture_sites[i];
        uint64_t offset=site->va-UINT64_C(0x140000000);int code=0;
        if(site->va<UINT64_C(0x140000000)||offset>SIZE_MAX||!fits((size_t)offset,site->size,declared))return 0;
        for(unsigned j=0;j<count;++j) {
            const unsigned char *s=base+sections+(size_t)j*40;
            uint32_t rva=u32(s+12),bytes=u32(s+8);
            if((u32(s+36)&0x20000000)&&offset>=rva&&offset-rva<=bytes&&site->size<=bytes-(offset-rva)){code=1;break;}
        }
        if(!code||memcmp(base+(size_t)offset,site->bytes,site->size))return 0;
    }
    verified=1;return 1;
}

static void map_format(CPU *c) {
    if(!verified)texture_stop("unverified original mapping bytes");
    unsigned block=private_block(c->rcx),etc2=etc2_bytes(c->rcx);
    if(!block&&!etc2){L_000140D95650(c);return;}
    /* The transitional DXVK device has no mapping for the ETC2 codes; the launcher refuses that pair. */
    if(etc2&&!frostpunk_rdvk_active())texture_stop("ETC2 textures need the native Vulkan renderer: select it, or convert the textures to ASTC");
    int saved_errno=errno;
    unsigned srgb=(uint8_t)c->rdx;
    if(srgb>1)texture_stop("sRGB argument must be boolean");
    if(c->rsp>UINT64_MAX-8)texture_stop("return stack overflow");
    win64_pointer(c->rsp,8,0);
    c->rax=etc2==8?(srgb?FP_ETC2_RGB_DXGI_SRGB:FP_ETC2_RGB_DXGI_UNORM):
           etc2==16?(srgb?FP_ETC2_RGBA_DXGI_SRGB:FP_ETC2_RGBA_DXGI_UNORM):
           block==4?(srgb?FP_ASTC4_DXGI_SRGB:FP_ASTC4_DXGI_UNORM):
                    (srgb?FP_ASTC6_DXGI_SRGB:FP_ASTC6_DXGI_UNORM);
    c->rsp+=8;errno=saved_errno;
}

static void format_layout(CPU *c) {
    if(!verified)texture_stop("unverified original layout bytes");
    unsigned block=private_block(c->rcx),etc2=etc2_bytes(c->rcx);
    if(!block&&!etc2){L_000140F499A0(c);return;}
    if(etc2)block=4;
    int saved_errno=errno;
    uint32_t width=(uint32_t)c->rdx,height=(uint32_t)c->r8;
    if(!width||!height)texture_stop("zero ASTC mip dimension");
    uint64_t row=(((uint64_t)width+block-1)/block)*(etc2?etc2:16);
    if(row>UINT32_MAX)texture_stop("ASTC row pitch overflow");
    uint64_t bytes=row*(((uint64_t)height+block-1)/block);
    if(bytes>UINT32_MAX)texture_stop("ASTC mip size overflow");
    if(c->rsp>UINT64_MAX-48)texture_stop("argument stack overflow");
    const unsigned char *stack=win64_pointer(c->rsp,48,0);
    uint64_t row_address;memcpy(&row_address,stack+40,8);
    void *row_output=win64_pointer(row_address,4,1);
    void *size_output=win64_pointer(c->r9,4,1);
    uint32_t r=(uint32_t)row,n=(uint32_t)bytes;
    /* Preserve the original helper's store order for aliased output pointers. */
    memcpy(row_output,&r,4);memcpy(size_output,&n,4);
    c->rsp+=8;errno=saved_errno;
}

void (*fp_android_texture_formats_lookup(uint64_t original_va))(CPU *) {
    if(!verified)return NULL;
    if(original_va==UINT64_C(0x140d95650))return map_format;
    if(original_va==UINT64_C(0x140f499a0))return format_layout;
    return NULL;
}
