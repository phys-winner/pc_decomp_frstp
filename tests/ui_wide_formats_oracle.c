/* Native UCRT only; appends four original UI formats to frozen accepted vectors. */
#define main accepted_wide_oracle_main
#include "wide_stdio_oracle.c"
#undef main

static void precision_guards(void) {
    unsigned char *pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    require(pages!=NULL,"precision guard allocation");DWORD previous;
    require(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&previous),"precision guard protection");
    wchar_t *tail=(wchar_t*)(pages+4096-4);tail[0]=0xd83d;tail[1]='B';
    uint64_t va[2]={UINT64_C(0xaabbccdd00000002),(uintptr_t)tail};wchar_t output[8];
    require(query(6,NULL,0,L"%.*s...",NULL,va)==5,"precision2 does not read a terminator");
    require(secure(4,output,6,5,L"%.*s...",NULL,va)==5&&output[0]==0xd83d&&output[1]=='B'&&output[2]=='.'&&output[3]=='.'&&output[4]=='.'&&!output[5],"precision2 exact UTF16 units");
    va[0]=UINT64_C(0xaabbccdd00000000);va[1]=(uintptr_t)(pages+4096);
    require(query(6,NULL,0,L"%.*s...",NULL,va)==3,"precision0 does not read invalid string");
    require(secure(4,output,4,3,L"%.*s...",NULL,va)==3&&output[0]=='.'&&output[1]=='.'&&output[2]=='.'&&!output[3],"precision0 exact literal");
    require(VirtualFree(pages,0,MEM_RELEASE),"precision guard cleanup");
}
int main(int argc,char **argv) {
    require(argc==3,"accepted vector input and extended vector output");
    HMODULE ucrt=LoadLibraryExW(L"ucrtbase.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);require(ucrt!=NULL,"system UCRT");
    query=(query_fn)(uintptr_t)GetProcAddress(ucrt,"__stdio_common_vswprintf");
    secure=(secure_fn)(uintptr_t)GetProcAddress(ucrt,"__stdio_common_vsnwprintf_s");
    native_errno=(get_errno)(uintptr_t)GetProcAddress(ucrt,"_errno");
    set_handler set=(set_handler)(uintptr_t)GetProcAddress(ucrt,"_set_invalid_parameter_handler");
    require(query&&secure&&native_errno&&set,"system UCRT entrypoints");invalid_handler previous=set(on_invalid);
    FILE *old=fopen(argv[1],"rb"),*file=fopen(argv[2],"wb");require(old&&file,"vector files");
    uint32_t header[2];require(fread(header,sizeof(header),1,old)==1&&header[0]==0x31504657&&header[1]==1340,"accepted seven-format vector header");
    bytes(file,header,sizeof(header));unsigned char buffer[4096];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),old)))bytes(file,buffer,n);
    require(!ferror(old)&&!fclose(old),"accepted vector copy");total=1340;valid=1185;invalid=155;
    const wchar_t *lefts[]={L"slot_name",L"",L"A\xd83d\xde00\xd800Q",NULL,L"A",L"long saved game title"};
    const wchar_t *rights[]={L"save",L"",L"\xd800Z",NULL,L"world",L"chapter"};
    const uint64_t signed_values[]={0,7,UINT64_C(0xaabbccddfffffff9),999,UINT64_C(0xffffffff80000000),0x7fffffff};
    const uint64_t precisions[]={UINT64_C(0xaabbccddffffffff),0,1,2,3,0x7fffffff};
    for(unsigned sample=0;sample<6;++sample)for(unsigned mode=0;mode<11;++mode){
        one(file,0,L"%s.%s",lefts[sample],rights[sample],0,mode);
        one(file,0,L"%s - %s",lefts[sample],rights[sample],0,mode);
        one(file,3,L"Saved game %3d",NULL,NULL,signed_values[sample],mode);
        one(file,3,L"%.*s...",NULL,lefts[sample],precisions[sample],mode);
    }
    require(!fseek(file,4,SEEK_SET)&&fwrite(&total,4,1,file)==1&&!fclose(file),"extended vector count");
    precision_guards();set(previous);
    printf("{\"success\":true,\"vectors\":%u,\"valid\":%u,\"invalid_callbacks\":%u,\"formats\":11,\"accepted_vector_prefix\":1340,\"new_UI_vectors\":264,\"precision_guard_probes\":4,\"original_game_executed\":false}\n",total,valid,invalid);
    return 0;
}
