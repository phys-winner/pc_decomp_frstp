/* Production data-only GDI closure plus the full existing display regression. */
#define wide display_private_wide
#include "../android/host/win64_display.c"
#undef wide
static _Noreturn void fixture_boundary_abort(void);
#define abort fixture_boundary_abort
#define main display_regression_main
#define win64_alloc fixture_base_alloc
#define win64_free fixture_base_free
#include "display_fixture.c"
#undef win64_alloc
#undef win64_free
#undef main
#undef abort
static _Noreturn void fixture_boundary_abort(void){fflush(stderr);_Exit(73);}
#if defined(_WIN32)
#define strcasecmp _stricmp
#endif
#include "cursor_bitmap_route.inc"
static unsigned allocations,frees,cursor_calls;
static int fail_allocation;
uint64_t win64_alloc(size_t bytes,int zero){if(fail_allocation){win64_set_error(8);return 0;}++allocations;return fixture_base_alloc(bytes,zero);}
void win64_free(uint64_t address){++frees;fixture_base_free(address);}
static uint64_t checked(const char *name,int routed,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f){
    _Alignas(16) uint64_t stack[10];memset(stack,0x7b,sizeof(stack));stack[5]=e;stack[6]=f;uint64_t stack_before[10];memcpy(stack_before,stack,sizeof(stack));
    win64_region_add((uintptr_t)stack,sizeof(stack),0);CPU cpu,expected;memset(&cpu,0xa7,sizeof(cpu));cpu.rsp=(uintptr_t)stack;cpu.rcx=a;cpu.rdx=b;cpu.r8=c;cpu.r9=d;expected=cpu;
    win64_set_error(0x5a17);errno=1927;
    int handled=routed?cursor_bitmap_import(&cpu,!strcmp(name,"CreateIconIndirect")?"USER32.dll":"gdi32.dll",name):win64_display_call(&cpu,name);
    require(handled,"GDI/cursor boundary handled");uint64_t result=cpu.rax;expected.rax=result;if(routed)expected.rsp+=8;
    require(!memcmp(&cpu,&expected,sizeof(cpu)),"GDI fullCPU/flags/XMM/RSP preservation");require(errno==1927&&!memcmp(stack,stack_before,sizeof(stack)),"GDI errno/argument stack unchanged");
    win64_region_remove((uintptr_t)stack);return result;
}
static void header(unsigned char *f,int width,int height){memset(f,0,108);put32(f,0,108);put32(f,4,(uint32_t)width);put32(f,8,(uint32_t)height);put16(f,12,1);put16(f,14,32);put32(f,16,3);put32(f,40,0xff0000);put32(f,44,0xff00);put32(f,48,0xff);put32(f,52,0xff000000);}
static int registered(uint64_t address){for(unsigned i=0;i<region_count;++i)if(regions[i].base==address)return 1;return 0;}
static void image_case(int width,int height,unsigned routed){
    unsigned char f[108],before[108];header(f,width,height);memcpy(before,f,sizeof(f));
    struct {uint64_t before,pointer,after;} output={UINT64_C(0x1122334455667788),UINT64_MAX,UINT64_C(0x8877665544332211)};
    win64_region_add((uintptr_t)f,sizeof(f),0);win64_region_add((uintptr_t)&output.pointer,8,1);
    uint64_t dc=api("GetDC",1,V(0));require(dc!=0,"SDL desktop lease");
    uint64_t color=checked("CreateDIBSection",routed,dc,(uintptr_t)f,0,(uintptr_t)&output.pointer,0,3),pixels=output.pointer;
    unsigned rows=height<0?(unsigned)-height:(unsigned)height;size_t bytes=(size_t)width*rows*4;
    require(color&&pixels&&pixels!=UINT64_MAX&&!memcmp(f,before,sizeof(f)),"real DIB result/output/read-only V4 header");
    require(output.before==UINT64_C(0x1122334455667788)&&output.after==UINT64_C(0x8877665544332211),"DIB output exact8 bytes/canaries");
    require(win64_get_error()==0x5a17,"successful supported DIB preserves prior error policy");
    unsigned char *storage=win64_pointer(pixels,bytes,1);for(size_t i=0;i<bytes;++i)storage[i]=(unsigned char)(i*37+19);
    if(width==32&&height==-32){unsigned char source[4096];for(size_t i=0;i<sizeof(source);++i)source[i]=(unsigned char)(i*17+23);memcpy(storage,source,sizeof(source));require(bytes==sizeof(source)&&!memcmp(storage,source,sizeof(source)),"exact SDL32x32 BGRA memcpy4096");}
    cpu_bitmap *record=bitmap_locked(color);require(record&&record->kind==1&&record->width==(unsigned)width&&record->rows==rows&&record->pitch==(unsigned)width*4&&record->bytes==bytes&&record->memory==pixels,"DIB allocation geometry/registry");
    unsigned char query[52],query_before[52];memset(query,0xa5,sizeof(query));put32(query,0,40);put16(query,14,0);memcpy(query_before,query,sizeof(query));win64_region_add((uintptr_t)query,sizeof(query),1);
    require(!api("GetDIBits",7,dc,color,V(0),V(1),V(0),V((uintptr_t)query),V(0))&&win64_get_error()==50&&!memcmp(query,query_before,sizeof(query)),"legacy GetDIBits honestly rejects new bitmap kind without writes");win64_region_remove((uintptr_t)query);
    size_t pitch=((size_t)width+15)/16*2,mask_bytes=pitch*rows;unsigned char *mask=malloc(mask_bytes);require(mask!=NULL,"mask source allocation");
    for(size_t i=0;i<mask_bytes;++i)mask[i]=(unsigned char)(i*13+0x55);win64_region_add((uintptr_t)mask,mask_bytes,1);
    uint64_t mono=checked("CreateBitmap",routed,width,rows,1,1,(uintptr_t)mask,0);record=bitmap_locked(mono);
    require(mono&&record&&record->kind==2&&record->pitch==pitch&&record->bytes==mask_bytes,"real mono WORD-pitch geometry");
    uint64_t mono_memory=record->memory;unsigned char *mono_data=win64_pointer(mono_memory,mask_bytes,0);require(!memcmp(mono_data,mask,mask_bytes),"mono source copied exactly");memset(mask,0,mask_bytes);
    if(width==32&&height==-32)require(mask_bytes==128&&pitch==4,"exact SDL32x32 mono WORDpitch4 bytes128");
    for(size_t i=0;i<mask_bytes;++i)require(mono_data[i]==(unsigned char)(i*13+0x55),"mono independent source lifetime");
    require(api("ReleaseDC",2,V(0),dc)==1,"SDL release before direct pixel memcpy");
    unsigned char icon[32]={0};put32(icon,4,(uint32_t)width/2);put32(icon,8,rows/2);memcpy(icon+16,&mono,8);memcpy(icon+24,&color,8);unsigned char icon_before[32];memcpy(icon_before,icon,32);win64_region_add((uintptr_t)icon,32,0);
    require(!checked("CreateIconIndirect",routed,(uintptr_t)icon,0,0,0,0,0)&&win64_get_error()==50&&!memcmp(icon,icon_before,32),"honest unavailable cursor image: no HICON fabricated");++cursor_calls;
    unsigned freed=frees;require(api("DeleteObject",1,color)==1&&api("DeleteObject",1,mono)==1&&frees==freed+2&&win64_get_error()==50,"both true bitmap allocations freed preserving cursor failure50");
    require(!bitmap_locked(color)&&!bitmap_locked(mono)&&bitmap_bytes==0,"retired unique handles/accounting");
    require(!registered(pixels)&&!registered(mono_memory),"DeleteObject revokes both actual pixel regions");
    win64_set_error(0x5a17);require(!api("DeleteObject",1,color)&&win64_get_error()==6,"double delete rejects retired handle");
    win64_region_remove((uintptr_t)icon);win64_region_remove((uintptr_t)mask);free(mask);win64_region_remove((uintptr_t)&output.pointer);win64_region_remove((uintptr_t)f);
}
int main(int argc,char **argv){
    if(argc==2){unsigned char f[108],mask[128];uint64_t output=UINT64_MAX;header(f,32,-32);win64_display_configure(960,540,60);
        win64_region_add((uintptr_t)f,!strcmp(argv[1],"--short-bmi")?107:108,0);win64_region_add((uintptr_t)&output,!strcmp(argv[1],"--short-output")?7:8,1);
        if(!strcmp(argv[1],"--short-mono")){win64_region_add((uintptr_t)mask,127,0);checked("CreateBitmap",1,32,32,1,1,(uintptr_t)mask,0);}
        else if(!strcmp(argv[1],"--short-bmi")||!strcmp(argv[1],"--short-output"))checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&output,0,0);
        else return 2;return 99;
    }
    require(argc==1,"bitmap fixture arguments");
    require(!display_regression_main(),"existing entire display regression");win64_display_configure(960,540,60);
    const int widths[]={1,17,32,33};for(unsigned r=0;r<2;++r)for(unsigned i=0;i<4;++i){image_case(widths[i],-9,r);image_case(widths[i],9,r);}image_case(32,-32,1);
    unsigned char f[108];header(f,32,-32);uint64_t pixels=UINT64_MAX;win64_region_add((uintptr_t)f,sizeof(f),1);win64_region_add((uintptr_t)&pixels,8,1);
    for(unsigned kind=0;kind<6;++kind){header(f,32,-32);if(kind==0)put32(f,4,0);else if(kind==1)put32(f,8,0);else if(kind==2)put32(f,4,UINT32_MAX);else if(kind==3)put32(f,8,0x80000000);else if(kind==4)put16(f,12,2);pixels=UINT64_MAX;
        require(!checked("CreateDIBSection",kind&1,0,(uintptr_t)f,kind==5?2:0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==87,"native invalid DIB87/outputNULL");}
    header(f,32,-32);pixels=UINT64_MAX;require(!checked("CreateDIBSection",1,0,0,0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==0x5a17,"native NULL info result/output/error");
    uint64_t no_output=checked("CreateDIBSection",1,0,(uintptr_t)f,0,0,0,0);require(no_output&&api("DeleteObject",1,no_output)==1,"native optional bits output");
    fail_allocation=1;pixels=UINT64_MAX;require(!checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==8&&bitmap_bytes==0,"allocation failure has NULL output/no live bitmap");fail_allocation=0;
    pixels=UINT64_MAX;require(!checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,1,0)&&!pixels&&win64_get_error()==50,"unsupported shared section has no fake success");
    put32(f,52,0);require(!checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==50,"unsupported alpha profile explicit failure");
    require(!checked("CreateBitmap",1,UINT32_MAX,9,1,1,0,0)&&win64_get_error()==87,"negative mono geometry87");
    uint64_t empty=checked("CreateBitmap",1,0,9,1,1,0,0);cpu_bitmap *record=bitmap_locked(empty);require(record&&record->width==1&&record->rows==1&&record->pitch==2&&api("DeleteObject",1,empty)==1,"zero extent real1x1mono");
    require(!checked("CreateIconIndirect",1,0,0,0,0,0,0)&&win64_get_error()==87,"bounded HLE NULL icon descriptor policy87");
    uint64_t big[4];header(f,2048,-2048);for(unsigned i=0;i<4;++i){big[i]=checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,0,0);require(big[i]!=0,"bounded real allocation within64MiB");}
    header(f,1,-1);require(!checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==8&&bitmap_bytes==BITMAP_BYTE_LIMIT,"aggregate64MiB capacity fail8");
    for(unsigned i=0;i<4;++i)require(api("DeleteObject",1,big[i])==1,"release budget allocation");require(!bitmap_bytes,"allocation budget returns after delete");
    header(f,INT32_MAX,INT32_MIN);require(!checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==87,"INT_MIN height rejected without signed overflow");
    header(f,INT32_MAX,INT32_MAX);require(!checked("CreateDIBSection",1,0,(uintptr_t)f,0,(uintptr_t)&pixels,0,0)&&!pixels&&win64_get_error()==8,"oversized geometry no product truncation/allocation");
    CPU cpu,before;memset(&cpu,0xa7,sizeof(cpu));before=cpu;require(!cursor_bitmap_import(&cpu,"KERNEL32.dll","CreateBitmap")&&!memcmp(&cpu,&before,sizeof(cpu)),"wrong DLL untouched");
    require(!win64_display_call(&cpu,"CreateDIBitmap")&&!memcmp(&cpu,&before,sizeof(cpu)),"unknown bitmap API untouched");
    win64_region_remove((uintptr_t)&pixels);win64_region_remove((uintptr_t)f);win64_display_configure(0,0,0);
    require(allocations==frees,"all actual data allocations released");
    printf("{\"success\":true,\"checks\":%u,\"image_cases\":%u,\"real_DIB32_and_mono_allocations\":true,\"allocation_budget_bytes\":%u,\"all_allocations_released\":true,\"strict_nonrecycled_handles\":true,\"native_invalid_parameters87\":true,\"cursor_image_unavailable50\":true,\"SDL_cleanup_preserves_error50\":true,\"full_CPU_RSP_errno\":true,\"single_parent_pop\":true,\"legacy_display_regression\":true,\"cursor_handle_created\":false,\"game_boot_proven\":false}\n",checks,cursor_calls,BITMAP_BYTE_LIMIT);return 0;
}
