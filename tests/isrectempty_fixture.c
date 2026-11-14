/* Pure RECT predicate checks; the included legacy regression owns its host mocks. */
#define main window_regression_main
#include "window_fixture.c"
#undef main
#include <strings.h>
#include <sys/mman.h>
#include "isrectempty_route.inc"

typedef struct {int32_t rect[4];uint32_t error;int32_t native_errno;} rect_input;
typedef struct {uint64_t result;uint32_t error;int32_t native_errno;uint32_t rect_same,guards_same;} rect_output;
typedef struct {rect_input input;rect_output output;} rect_case;
_Static_assert(sizeof(rect_input)==24 && sizeof(rect_output)==24 && sizeof(rect_case)==48,"native record layout");
static unsigned focused_calls;

static void predicate(uint64_t address,const rect_output *expected,int imported) {
    _Alignas(16) unsigned char stack[8192],saved[8192];
    memset(stack,0xa7,sizeof(stack));memcpy(saved,stack,sizeof(stack));
    CPU cpu;memset(&cpu,0x65,sizeof(cpu));cpu.rcx=address;cpu.rsp=(uintptr_t)stack+4104;
    cpu.cf=1;cpu.zf=0;cpu.sf=1;cpu.of=0;cpu.pf=1;cpu.af=0;cpu.mxcsr=0xffe5;
    CPU before=cpu;win64_region_add((uintptr_t)stack,sizeof(stack),1);
    win64_set_error(expected->error);errno=expected->native_errno;
    int handled=imported?isrectempty_import(&cpu,"UsEr32.DlL","IsRectEmpty"):win64_window_call(&cpu,"IsRectEmpty");
    int after_errno=errno;uint32_t after_error=win64_get_error();
    require(handled && cpu.rax==expected->result,"native raw RAX predicate result");
    before.rsp+=imported?8:0;cpu.rax=before.rax;
    require(!memcmp(&cpu,&before,sizeof(cpu)),"full CPU/XMM/MXCSR and exact single import pop");
    require(after_errno==expected->native_errno && after_error==expected->error,"native errno and guest LastError preserved");
    require(!memcmp(stack,saved,sizeof(stack)),"whole 8192-byte guest stack unchanged");
    win64_region_remove((uintptr_t)stack);++focused_calls;
}

static void native_row(const rect_case *row) {
    for(unsigned odd=0;odd<2;++odd){
        _Alignas(16) unsigned char bytes[64],saved[64];memset(bytes,0x9d,sizeof(bytes));
        unsigned char *rect=bytes+16+odd;memcpy(rect,row->input.rect,16);memcpy(saved,bytes,sizeof(bytes));
        win64_region_add((uintptr_t)rect,16,0);
        for(int imported=0;imported<2;++imported){predicate((uintptr_t)rect,&row->output,imported);require(!memcmp(bytes,saved,sizeof(bytes)),"readonly RECT and outer guards unchanged");}
        win64_region_remove((uintptr_t)rect);
    }
}

static void readonly_pages(void) {
    long page=sysconf(_SC_PAGESIZE);require(page>=4096,"host page size");
    const int32_t shapes[3][4]={{0,0,960,540},{1,0,0,1},{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX}};
    for(unsigned mode=0;mode<3;++mode)for(unsigned shape=0;shape<3;++shape){
        unsigned char *mapping=mmap(NULL,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        require(mapping!=MAP_FAILED,"owned mmap");size_t offset=mode==0?0:mode==1?7:(size_t)page-16;
        memcpy(mapping+offset,shapes[shape],16);
        require(!mprotect(mapping+page,(size_t)page,PROT_NONE) && !mprotect(mapping,(size_t)page,PROT_READ),"readonly first page and inaccessible following page");
        rect_output expected={.result=shape==1,.error=0x5a17,.native_errno=1927,.rect_same=1,.guards_same=1};
        win64_region_add((uintptr_t)(mapping+offset),16,0);
        for(int imported=0;imported<2;++imported)predicate((uintptr_t)(mapping+offset),&expected,imported);
        require(!memcmp(mapping+offset,shapes[shape],16),"mprotect readonly RECT preserved");
        win64_region_remove((uintptr_t)(mapping+offset));require(!munmap(mapping,(size_t)page*2),"owned mmap cleanup");
    }
}

static void gates(void) {
    CPU cpu;memset(&cpu,0x65,sizeof(cpu));cpu.rcx=1;CPU before=cpu;
    win64_set_error(0x5a17);errno=1927;
    require(!isrectempty_import(&cpu,"kernel32.dll","IsRectEmpty") && !memcmp(&cpu,&before,sizeof(cpu)),"wrong DLL does not read invalid RECT or pop");
    require(!isrectempty_import(&cpu,"user32.dll","IsRectEmptyUnexpected") && !memcmp(&cpu,&before,sizeof(cpu)),"unknown API does not read invalid RECT or pop");
    require(!win64_window_call(&cpu,"IsRectEmptyUnexpected") && !memcmp(&cpu,&before,sizeof(cpu)),"direct unknown API unhandled");
    require(win64_get_error()==0x5a17 && errno==1927,"unhandled routes preserve errors");
}

int main(int argc,char **argv) {
    if(argc==2 && !strncmp(argv[1],"--",2)){
        /* These are injected-registry SIGABRT checks, not production STOP73 parity. */
        CPU cpu={0};unsigned char short_rect[16]={0};
        if(!strcmp(argv[1],"--unmapped"))cpu.rcx=1;
        else if(!strcmp(argv[1],"--short15")){cpu.rcx=(uintptr_t)short_rect;win64_region_add(cpu.rcx,15,0);}
        else if(!strcmp(argv[1],"--overflow"))cpu.rcx=UINT64_MAX-7;
        else return 2;
        win64_window_call(&cpu,"IsRectEmpty");return 3;
    }
    require(argc==2,"prepared native dataset argument");FILE *file=fopen(argv[1],"rb");require(file!=NULL,"open prepared native dataset");
    unsigned char magic[8]={0};uint32_t count=0,row_bytes=0;
    require(fread(magic,1,8,file)==8 && !memcmp(magic,"RECTEMP1",8) && fread(&count,4,1,file)==1 && fread(&row_bytes,4,1,file)==1 && count==21314 && row_bytes==sizeof(rect_case),"prepared dataset header");
    for(uint32_t i=0;i<count;++i){rect_case row;require(fread(&row,sizeof(row),1,file)==1,"native row present");native_row(&row);}
    require(fgetc(file)==EOF && !ferror(file) && !fclose(file),"native dataset exact length");
    rect_output null_expected={.result=1,.error=0x5a17,.native_errno=1927,.rect_same=1,.guards_same=1};
    predicate(0,&null_expected,0);predicate(0,&null_expected,1);readonly_pages();gates();
    require(!region_count,"focused registry cleanup before legacy regression");
    unsigned focused_checks=checks;require(!window_regression_main(),"unchanged whole window regression");
    printf("{\"success\":true,\"native_rows\":%u,\"focused_calls\":%u,\"focused_checks\":%u,\"native_memory_controls\":9,\"NULL_calls\":2,\"full_CPU\":true,\"stack_bytes\":8192,\"legacy_window_regression\":true,\"game_boot_proven\":false}\n",count,focused_calls,focused_checks);
    return 0;
}
