#define _GNU_SOURCE
#include "../android/host/win64.c"
#include "../android/host/win64_stdio.c"
#include "../android/host/zip_files.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static _Thread_local unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t at,unsigned n){uint64_t v=0;assert(at+n<=sizeof(fixture_teb));memcpy(&v,fixture_teb+at,n);return v;}
void frostpunk_teb_write(uint64_t at,uint64_t v,unsigned n){assert(at+n<=sizeof(fixture_teb));memcpy(fixture_teb+at,&v,n);}
typedef struct {char game[4096],error[512];} app_context;
#include "game-input-check.inc"
static uint64_t api(const char *name,const uint64_t args[8],int crt){
    CPU c={0};uint64_t stack[16]={0};c.rsp=(uintptr_t)stack;c.rcx=args[0];c.rdx=args[1];c.r8=args[2];c.r9=args[3];
    for(unsigned i=4;i<8;++i)stack[i+1]=args[i];win64_region_add((uintptr_t)stack,sizeof(stack),1);
    assert(crt?win64_stdio_call(&c,name):win64_kernel_call(&c,name));assert(c.rsp==(uintptr_t)stack);win64_region_remove((uintptr_t)stack);return c.rax;
}
#define K(name,...) api(name,(uint64_t[8]){__VA_ARGS__},0)
#define C(name,...) api(name,(uint64_t[8]){__VA_ARGS__},1)
static int check(app_context *context,int dir,const char *text){char copy[1024];assert(strlen(text)<sizeof(copy));strcpy(copy,text);context->error[0]=0;return game_inputs_check_text(context,dir,copy);}

int main(void){
    char temp[]="/data/local/tmp/fp-zip-test-XXXXXX";assert(mkdtemp(temp));char game[4096],profile[4096],archive[4096],index[4096];
    snprintf(game,sizeof(game),"%s/game",temp);snprintf(profile,sizeof(profile),"%s/profile",temp);snprintf(archive,sizeof(archive),"%s/source.zip",temp);snprintf(index,sizeof(index),"%s/index.zmi",temp);
    assert(!mkdir(game,0700)&&!mkdir(profile,0700));int fd=open(archive,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(fd>=0);
    struct {const char *name,*bytes;} entries[]={
        {"common.dat","ORIGINAL"},{"port-ui.common.append","-PORT"},{"common.idx","PCIDX"},{"port-ui.common.idx","PORTIDX"},
        {"Frostpunk.exe","PE-FIXTURE"},{"templates.dat","TEMPLATE"},{"switch-ui.templates.append","-SWITCH"},
        {"templates-switch.idx","OLD"},{"switch-ui.templates-switch.idx","NEW"},{"textures-s3.dat","DXT"}};
    uint64_t offsets[10],position=(UINT64_C(1)<<32)+16384+37;
    for(unsigned i=0;i<10;++i){offsets[i]=position;assert(pwrite(fd,entries[i].bytes,strlen(entries[i].bytes),(off_t)position)==(ssize_t)strlen(entries[i].bytes));position+=4096;}
    uint64_t large_size=(UINT64_C(1)<<32)+4096,archive_size=offsets[9]+large_size+16384;
    assert(!ftruncate(fd,(off_t)archive_size)&&pwrite(fd,"TAIL",4,(off_t)(offsets[9]+large_size-4))==4);
    FILE *out=fopen(index,"wb");assert(out);unsigned char header[48]={0};memcpy(header,"ZMI1",4);uint32_t count=10;memcpy(header+4,&count,4);memcpy(header+8,&archive_size,8);assert(fwrite(header,1,48,out)==48);
    for(unsigned i=0;i<10;++i){uint64_t size=i==9?large_size:strlen(entries[i].bytes),time=1000;uint32_t zero=0;uint16_t length=(uint16_t)strlen(entries[i].name);
        assert(fwrite(&offsets[i],8,1,out)==1&&fwrite(&size,8,1,out)==1&&fwrite(&zero,4,1,out)==1&&fwrite(&zero,4,1,out)==1&&fwrite(&time,8,1,out)==1&&fwrite(&length,2,1,out)==1&&fwrite(entries[i].name,1,length,out)==length);}
    assert(!fclose(out));char descriptor[32];snprintf(descriptor,sizeof(descriptor),"%d",fd);
    assert(!setenv("PORT_PART_GAME",game,1)&&!setenv("PORT_MOUNT_GAME",index,1)&&!setenv("PORT_MOUNT_FD_GAME",descriptor,1));
    assert(!setenv("FROSTPUNK_PORT_DATA","0",1)&&!setenv("FROSTPUNK_SWITCH_DATA","0",1));
    fp_data_file file;char buffer[512]={0};assert(fp_data_open("COMMON.DAT",&file)&&file.size==8);assert(fp_data_read(&file,buffer,99,0)==8&&!memcmp(buffer,"ORIGINAL",8));fp_data_close(&file);
    assert(!setenv("FROSTPUNK_PORT_DATA","1",1)&&!setenv("FROSTPUNK_SWITCH_DATA","1",1));
    assert(fp_data_open("common.dat",&file)&&file.size==13);assert(fp_data_read(&file,buffer,99,6)==7&&!memcmp(buffer,"AL-PORT",7));assert(!fp_data_read(&file,buffer,1,13));
    void *base;size_t length;void *view=fp_data_map(&file,6,7,PROT_READ,NULL,&base,&length);assert(view!=MAP_FAILED&&!memcmp(view,"AL-PORT",7));assert(!munmap(base,length));fp_data_close(&file);
    assert(fp_data_open("Frostpunk.exe",&file));view=fp_data_map(&file,0,10,PROT_READ|PROT_WRITE,NULL,&base,&length);assert(view!=MAP_FAILED&&!memcmp(view,"PE-FIXTURE",10));((char*)view)[0]='X';assert(fp_data_read(&file,buffer,10,0)==10&&buffer[0]=='P');assert(!munmap(base,length));fp_data_close(&file);
    assert(fp_data_open("templates.idx",&file)==0);assert(fp_data_open("templates-switch.idx",&file)&&fp_data_read(&file,buffer,3,0)==3&&!memcmp(buffer,"NEW",3));fp_data_close(&file);
    assert(!fp_data_open("../common.dat",&file)&&!fp_data_open("/common.dat",&file));
    assert(win64_kernel_configure_roots(game,profile));
    const char name[]="common.dat",pattern[]="*";win64_region_add((uintptr_t)name,sizeof(name),0);win64_region_add((uintptr_t)pattern,sizeof(pattern),0);win64_region_add((uintptr_t)buffer,sizeof(buffer),1);
    uint64_t handle=K("CreateFileA",(uintptr_t)name,0x80000000u,1,0,3,0x80,0);assert(handle!=UINT64_MAX);
    uint64_t size=0;uint32_t got=0;win64_region_add((uintptr_t)&size,sizeof(size),1);win64_region_add((uintptr_t)&got,sizeof(got),1);
    assert(K("GetFileSizeEx",handle,(uintptr_t)&size)&&size==13);assert(K("ReadFile",handle,(uintptr_t)buffer,99,(uintptr_t)&got,0)&&got==13&&!memcmp(buffer,"ORIGINAL-PORT",13));
    assert(K("SetFilePointerEx",handle,(uint64_t)-6,(uintptr_t)&size,2)&&size==7);assert(K("ReadFile",handle,(uintptr_t)buffer,99,(uintptr_t)&got,0)&&got==6&&!memcmp(buffer,"L-PORT",6));
    uint64_t mapping=K("CreateFileMappingA",handle,0,2,0,0,0);assert(mapping);uint64_t mapped=K("MapViewOfFile",mapping,4,0,0,13);assert(mapped&&!memcmp((void*)(uintptr_t)mapped,"ORIGINAL-PORT",13));
    assert(K("CloseHandle",mapping)&&K("CloseHandle",handle)&&K("UnmapViewOfFile",mapped));
    const char large[]="textures-s3.dat";win64_region_add((uintptr_t)large,sizeof(large),0);
    handle=K("CreateFileA",(uintptr_t)large,0x80000000u,1,0,3,0x80,0);assert(handle!=UINT64_MAX);
    assert(K("GetFileSizeEx",handle,(uintptr_t)&size)&&size==large_size);
    assert(K("SetFilePointerEx",handle,(uint64_t)-4,(uintptr_t)&size,2)&&size==large_size-4);
    assert(K("ReadFile",handle,(uintptr_t)buffer,8,(uintptr_t)&got,0)&&got==4&&!memcmp(buffer,"TAIL",4));
    mapping=K("CreateFileMappingA",handle,0,2,0,0,0);assert(mapping);mapped=K("MapViewOfFile",mapping,4,1,0,4096);assert(mapped&&!memcmp((char*)(uintptr_t)mapped+4092,"TAIL",4));
    assert(K("UnmapViewOfFile",mapped)&&K("CloseHandle",mapping)&&K("CloseHandle",handle));
    uint64_t find=K("FindFirstFileA",(uintptr_t)pattern,(uintptr_t)buffer);assert(find!=UINT64_MAX);unsigned found=1;while(K("FindNextFileA",find,(uintptr_t)buffer))++found;assert(found>=10&&K("FindClose",find));
    uint16_t wide[]={'c','o','m','m','o','n','.','d','a','t',0},mode[]={'r','b',0};uint64_t stream=0;
    win64_region_add((uintptr_t)wide,sizeof(wide),0);win64_region_add((uintptr_t)mode,sizeof(mode),0);win64_region_add((uintptr_t)&stream,sizeof(stream),1);
    assert(!C("_wfopen_s",(uintptr_t)&stream,(uintptr_t)wide,(uintptr_t)mode)&&stream);assert(C("fread",(uintptr_t)buffer,1,99,stream)==13&&!memcmp(buffer,"ORIGINAL-PORT",13));assert(!C("_fseeki64",stream,6,0));assert(C("fread",(uintptr_t)buffer,1,7,stream)==7&&!memcmp(buffer,"AL-PORT",7));assert(!C("fclose",stream));
    app_context context={0};strcpy(context.game,game);int dir=open(game,O_RDONLY|O_DIRECTORY);assert(dir>=0);
    assert(check(&context,dir,"8,13\tcommon.dat\n*\ttextures-s3.dat\n"));
    const char *bad[]={"8,12\tcommon.dat\n","*\tcommon.dat\n","13,\tcommon.dat\n","-1\tcommon.dat\n","18446744073709551616\tcommon.dat\n","13\t../common.dat\n","13\t/common.dat\n","13\tC:common.dat\n","\tcommon.dat\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)assert(!check(&context,dir,bad[i]));close(dir);
    assert(!unlink(archive)&&!unlink(index)&&!rmdir(game)&&!rmdir(profile)&&!rmdir(temp));close(fd);
    puts("{\"success\":true,\"offset_over_4GiB\":true,\"cross_extent_reads\":true,\"bounded_EOF\":true,\"private_mmap\":true,\"Win64_ReadFile_seek_size_mapping_find\":true,\"CRT_binary_stream\":true,\"manifest_negative_cases\":9,\"no_game_files_extracted\":true}");
}
