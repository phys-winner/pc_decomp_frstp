/* The old qualified UI suite stays intact; original bytes remain NX data. */
#define main qualified_ui_main
#include "ui_body.inc"
#undef main

static uint32_t virtual_width=640,virtual_height=360,virtual_hz=120;
static int advertised=1,short_vector,readonly_config;
static unsigned char mode_entries[4096*12];
static unsigned long normalize_cases,apply_cases,pointer_calls;
static uint64_t fixture_caller=UINT64_C(0x140e66287);
static uint32_t fixture_last_error=0xabc123;
int frostpunk_dxvk_get_display_mode(uint32_t*w,uint32_t*h,uint32_t*r){
    errno=4321;if(!advertised)return 0;*w=virtual_width;*h=virtual_height;*r=virtual_hz;return 1;
}
void *win64_pointer(uint64_t p,size_t n,int writing){
    ++pointer_calls;errno=3456;
    if(p>=ptr(engine)&&p-ptr(engine)<sizeof(engine)&&n<=sizeof(engine)-(size_t)(p-ptr(engine))&&(!writing||!readonly_config))return(void*)(uintptr_t)p;
    if(!writing&&p>=ptr(stack_data)&&p-ptr(stack_data)<sizeof(stack_data)&&n<=sizeof(stack_data)-(size_t)(p-ptr(stack_data)))return(void*)(uintptr_t)p;
    size_t available=short_vector?11:sizeof(mode_entries);
    if(!writing&&p>=ptr(mode_entries)&&p-ptr(mode_entries)<available&&n<=available-(size_t)(p-ptr(mode_entries)))return(void*)(uintptr_t)p;
    fprintf(stderr,"STOP resolution fixture pointer span\n");exit(73);
}
#include "resolution_config_candidate.h"
#include "resolution_config_functions.inc"

static void init_image(const char *path){
#if defined(_WIN32)
    image=VirtualAlloc((void*)(uintptr_t)UINT64_C(0x140000000),0x4540000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
#else
    image=mmap((void*)(uintptr_t)UINT64_C(0x140000000),0x4540000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    require(ptr(image)==UINT64_C(0x140000000),"new fixed NX image");g_image_delta=0;
    FILE *f=fopen(path,"rb");require(f!=NULL,"mode fixture blocks open");uint32_t n;
    require(fread(&n,4,1,f)==1&&n<100,"blocks count");
    for(unsigned i=0;i<n;++i){uint32_t rva,bytes;require(fread(&rva,4,1,f)==1&&fread(&bytes,4,1,f)==1&&rva<=0x4540000&&bytes<=0x4540000-rva,"mode blocks bounds");require(fread(image+rva,1,bytes,f)==bytes,"mode blocks bytes");}
    require(fgetc(f)==EOF&&!ferror(f)&&!fclose(f),"mode blocks exact tail");
}
static void config_setup(unsigned count,unsigned selected,unsigned stale){
    setup(3);memset(mode_entries,0x42,sizeof(mode_entries));
    for(unsigned i=0;i<count;++i){uint32_t row[3]={320+i*32,180+i*18,60+i};memcpy(mode_entries+i*12,row,12);}
    uint32_t chosen[3]={virtual_width,virtual_height,virtual_hz};memcpy(mode_entries+selected*12,chosen,12);
    wr64(ptr(engine+0xf8),ptr(mode_entries));wr32(ptr(engine+0x100),count);
    wr32(ptr(engine+0xe4),stale?960:virtual_width);wr32(ptr(engine+0xe8),stale?540:virtual_height);wr32(ptr(engine+0xec),stale?60:virtual_hz);
    engine[0xdc]=(unsigned char)(stale&1);wr32(ptr(engine+0xe0),0xdeadbeef);
}
static void paired(unsigned count,unsigned selected,unsigned stale){
    config_setup(count,selected,stale);CPU input=initial();input.rcx=ptr(engine+0xb0);wr64(input.rsp,fixture_caller);CPU expected=input,actual=input;
    unsigned char original[sizeof(engine)],wanted[sizeof(engine)],stack_before[sizeof(stack_data)],stack_after[sizeof(stack_data)];
    unsigned char modes_before[sizeof(mode_entries)];
    memcpy(original,engine,sizeof(engine));memcpy(stack_before,stack_data,sizeof(stack_data));memcpy(modes_before,mode_entries,sizeof(mode_entries));
    int normalize=advertised&&fixture_caller==UINT64_C(0x140e66287);
    if(normalize){wr32(ptr(engine+0xe4),virtual_width);wr32(ptr(engine+0xe8),virtual_height);wr32(ptr(engine+0xec),virtual_hz);}
    errno=2345;uint64_t control=fp_control(),status=fp_status();baseline_mode_match(&expected);
    require(errno==2345&&fp_control()==control&&fp_status()==status,"original match native environment");
    memcpy(wanted,engine,sizeof(engine));memcpy(stack_after,stack_data,sizeof(stack_data));
    memcpy(engine,original,sizeof(engine));memcpy(stack_data,stack_before,sizeof(stack_data));errno=2345;
    unsigned long pointers_before=pointer_calls;candidate_mode_match(&actual);
    require(!memcmp(&expected,&actual,sizeof(CPU)),"full CPU equals unchanged original with normalized input");
    require(!memcmp(wanted,engine,sizeof(engine))&&!memcmp(stack_after,stack_data,sizeof(stack_data)),"full config and guest stack match reference");
    require(!memcmp(modes_before,mode_entries,sizeof(mode_entries)),"mode vector read only");
    require(errno==2345&&fp_control()==control&&fp_status()==status&&fixture_last_error==0xabc123,"candidate native FP errno LastError unchanged");
    require(pointer_calls==pointers_before+(normalize?4:advertised?1:0),"unqualified policy/caller leaves original metadata untouched");
    if(normalize)require(rd32(ptr(engine+0xe0))==selected&&engine[0xdc]==1,"original selects exact first mode and marks valid");
    ++normalize_cases;
}
static void scale_apply(unsigned next){
    config_setup(1,0,1);CPU c=initial();c.rcx=ptr(engine+0xb0);wr64(c.rsp,fixture_caller);candidate_mode_match(&c);
    unsigned char config_before[sizeof(engine)];memcpy(config_before,engine,sizeof(engine));
    wr32(ptr(model+0x40),next);c=initial();c.rcx=ptr(model);c.rdx=ptr(flags_data);c.r8=ptr(flags_data+1);c.r9=ptr(flags_data+2);
    native_apply(&c);require(rd32(ptr(engine+0x1a4))==rs_factors[next],"qualified actual F4 Apply");
    require(rd32(ptr(engine+0xe4))==virtual_width&&rd32(ptr(engine+0xe8))==virtual_height&&rd32(ptr(engine+0xec))==virtual_hz,"scale-only Apply preserves normalized resolution and refresh");
    memcpy(config_before+0x1a4,engine+0x1a4,4);require(!memcmp(config_before,engine,sizeof(engine)),"only F4 changes in original scale Apply");
    ++apply_cases;
}
static CPU negative_cpu,negative_before;
static unsigned char negative_config[sizeof(engine)],negative_modes[sizeof(mode_entries)];
static void no_negative_commit(void){
    if(memcmp(&negative_cpu,&negative_before,sizeof(CPU))||memcmp(engine,negative_config,sizeof(engine))||memcmp(mode_entries,negative_modes,sizeof(mode_entries))){fprintf(stderr,"RESOLUTION_NEGATIVE_COMMITTED\n");_Exit(74);}
}
int main(int argc,char **argv){
    if(argc<2)return 64;
    if(argc==2){require(!qualified_ui_main(argc,argv),"old qualified UI matrix unchanged");}
    init_image(argv[1]);
    if(argc>2){
        config_setup(1,0,1);negative_cpu=initial();negative_cpu.rcx=ptr(engine+0xb0);wr64(negative_cpu.rsp,fixture_caller);
        if(!strcmp(argv[2],"--mode-absent"))wr32(ptr(mode_entries),800);
        else if(!strcmp(argv[2],"--count-zero"))wr32(ptr(engine+0x100),0);
        else if(!strcmp(argv[2],"--count-negative"))wr32(ptr(engine+0x100),UINT32_MAX);
        else if(!strcmp(argv[2],"--count-large"))wr32(ptr(engine+0x100),4097);
        else if(!strcmp(argv[2],"--vector-null"))wr64(ptr(engine+0xf8),0);
        else if(!strcmp(argv[2],"--vector-short"))short_vector=1;
        else if(!strcmp(argv[2],"--config-readonly"))readonly_config=1;
        else if(!strcmp(argv[2],"--bad-policy"))virtual_hz=0;
        else if(!strcmp(argv[2],"--config-null"))negative_cpu.rcx=0;
        else if(!strcmp(argv[2],"--config-overflow"))negative_cpu.rcx=UINT64_MAX-0x53;
        else if(!strcmp(argv[2],"--gate"))image[0xe5fa80]^=1;
        else if(!strcmp(argv[2],"--caller-gate"))image[0xe66282]^=1;
        else return 64;
        negative_before=negative_cpu;memcpy(negative_config,engine,sizeof(engine));memcpy(negative_modes,mode_entries,sizeof(mode_entries));atexit(no_negative_commit);
        fp_android_resolution_config(&negative_cpu);return 99;
    }
    unsigned long before=checks;
    unsigned counts[]={1,2,5,16};uint32_t widths[]={640,800,960};uint32_t rates[]={60,120,144};
    for(unsigned n=0;n<4;++n)for(unsigned selected=0;selected<counts[n];++selected)
        for(unsigned w=0;w<3;++w)for(unsigned hz=0;hz<3;++hz)for(unsigned stale=0;stale<3;++stale){
            virtual_width=widths[w];virtual_height=widths[w]*9/16;virtual_hz=rates[hz];paired(counts[n],selected,stale);}
    advertised=0;for(unsigned i=0;i<3;++i)paired(5,i,1);advertised=1;
    fixture_caller=UINT64_C(0x140123450);for(unsigned i=0;i<3;++i)paired(5,i,1);fixture_caller=UINT64_C(0x140e66287);
    /* Duplicate matching entries deliberately choose the ORIGINAL first one. */
    virtual_width=640;virtual_height=360;virtual_hz=120;config_setup(3,0,1);memcpy(mode_entries+24,mode_entries,12);
    CPU c=initial();c.rcx=ptr(engine+0xb0);wr64(c.rsp,fixture_caller);candidate_mode_match(&c);require(rd32(ptr(engine+0xe0))==0,"duplicate exact mode original first-index semantics");
    /* Reproduce the exact original no-match failure, then show normalization. */
    config_setup(1,0,1);c=initial();c.rcx=ptr(engine+0xb0);wr64(c.rsp,fixture_caller);baseline_mode_match(&c);
    require(engine[0xdc]==1&&!rd32(ptr(engine+0xe0))&&rd32(ptr(engine+0xe4))==960&&rd32(ptr(engine+0xe8))==540&&rd32(ptr(engine+0xec))==60,"original valid index zero can retain stale dimensions");
    c=initial();c.rcx=ptr(engine+0xb0);wr64(c.rsp,fixture_caller);candidate_mode_match(&c);
    require(rd32(ptr(engine+0xe4))==640&&rd32(ptr(engine+0xe8))==360&&rd32(ptr(engine+0xec))==120,"qualified initialization fixes original stale resolution");
    unsigned char normalized[sizeof(engine)];memcpy(normalized,engine,sizeof(engine));
    c=initial();c.rcx=ptr(engine+0xb0);wr64(c.rsp,fixture_caller);candidate_mode_match(&c);require(!memcmp(normalized,engine,sizeof(engine)),"repeated normalization idempotent memory");
    scale_apply(0);scale_apply(5);
    printf("{\"success\":true,\"new_checks\":%lu,\"normalization_cases\":%lu,\"scale_apply_cases\":%lu,\"no_policy_original_unchanged\":true,\"full_CPU_stack_memory_paired\":true,\"original_body_unchanged\":true,\"duplicate_first_match\":true,\"native_UI_RT_hardware_proven\":false,\"game_fps_proven\":false}\n",checks-before,normalize_cases,apply_cases);
    return 0;
}
