/* Native system UCRT UTF16 vectors. No original executable code is executed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (__cdecl *invalid_handler)(const wchar_t*,const wchar_t*,const wchar_t*,unsigned,uintptr_t);
typedef invalid_handler (__cdecl *set_handler)(invalid_handler);
typedef int *(__cdecl *get_errno)(void);
typedef int (__cdecl *query_fn)(uint64_t,wchar_t*,size_t,const wchar_t*,void*,const void*);
typedef int (__cdecl *secure_fn)(uint64_t,wchar_t*,size_t,size_t,const wchar_t*,void*,const void*);
typedef struct {
    uint32_t secure,options;uint64_t capacity,count;
    uint32_t null_buffer,invalid_callback;int32_t result,error;
    uint32_t format_bytes,left_bytes,right_bytes,output_bytes;
} vector_header;
_Static_assert(sizeof(wchar_t)==2 && sizeof(vector_header)==56,"Win64 UTF16 native vector ABI");
static unsigned invalid_calls,total,valid,invalid;
static query_fn query;static secure_fn secure;static get_errno native_errno;
static void __cdecl on_invalid(const wchar_t *a,const wchar_t *b,const wchar_t *c,unsigned d,uintptr_t e){(void)a;(void)b;(void)c;(void)d;(void)e;++invalid_calls;}
static void require(int condition,const char *why){if(!condition){fprintf(stderr,"WIDE_ORACLE_FAIL %s\n",why);exit(1);}}
static void bytes(FILE *file,const void *p,size_t n){require(!n||fwrite(p,1,n,file)==n,"vector output");}
static size_t units(const wchar_t *text){return text?(wcslen(text)+1)*2:0;}
static void one(FILE *file,unsigned kind,const wchar_t *format,const wchar_t *left,const wchar_t *right,uint64_t integer,unsigned mode){
    uint64_t va[3]={kind==3?integer:(uintptr_t)left,(uintptr_t)right,0};
    if(kind==6)memcpy(va,left,24);
    int length=query(6,NULL,0,format,NULL,va);require(length>=0&&length<1000,"native query length");
    vector_header h={0};h.secure=mode!=0;h.options=h.secure?4:6;
    h.null_buffer=(kind==3?2u:0u)|(kind==4||kind==5?4u:0u)|(kind==6?8u:0u);
    if(!mode){h.null_buffer|=1;h.capacity=0;}
    else if(mode==1){h.capacity=(uint64_t)length+1;h.count=(uint64_t)length;}
    else if(mode==2){h.capacity=(uint64_t)length+3;h.count=UINT64_MAX;}
    else if(mode==3){h.capacity=3;h.count=2;}
    else if(mode==4){h.capacity=3;h.count=UINT64_MAX;}
    else if(mode==5){h.capacity=3;h.count=3;}
    else if(mode==6){h.capacity=1;h.count=0;}
    else if(mode==7){h.null_buffer|=1;h.capacity=h.count=0;}
    else if(mode==8){h.null_buffer|=1;h.capacity=h.count=3;}
    else if(mode==9){h.capacity=h.count=0;}
    else if(mode==10){h.capacity=(uint64_t)length+3;h.count=h.capacity+4;}
    wchar_t output[2048];for(size_t i=0;i<2048;++i)output[i]=0xa55a;
    wchar_t *buffer=(h.null_buffer&1)?NULL:output;invalid_calls=0;*native_errno()=1927;SetLastError(0x5a37);
    h.result=h.secure?secure(h.options,buffer,(size_t)h.capacity,(size_t)h.count,format,NULL,va):query(h.options,buffer,(size_t)h.capacity,format,NULL,va);
    h.error=*native_errno();h.invalid_callback=invalid_calls!=0;require(GetLastError()==0x5a37,"native LastError preserved");
    h.format_bytes=(uint32_t)units(format);h.left_bytes=kind==6?24:kind==3?8:(uint32_t)units(left);h.right_bytes=(uint32_t)units(right);
    h.output_bytes=(h.null_buffer&1)?0:(uint32_t)(((h.capacity?h.capacity:1)+8)*2);
    bytes(file,&h,sizeof(h));bytes(file,format,h.format_bytes);bytes(file,kind==3?(const void*)&integer:(const void*)left,h.left_bytes);bytes(file,right,h.right_bytes);bytes(file,output,h.output_bytes);
    ++total;if(h.invalid_callback)++invalid;else ++valid;
}
int main(int argc,char **argv){
    require(argc==2,"output vector path");HMODULE ucrt=LoadLibraryExW(L"ucrtbase.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);require(ucrt!=NULL,"system UCRT");
    query=(query_fn)(uintptr_t)GetProcAddress(ucrt,"__stdio_common_vswprintf");secure=(secure_fn)(uintptr_t)GetProcAddress(ucrt,"__stdio_common_vsnwprintf_s");native_errno=(get_errno)(uintptr_t)GetProcAddress(ucrt,"_errno");set_handler set=(set_handler)(uintptr_t)GetProcAddress(ucrt,"_set_invalid_parameter_handler");require(query&&secure&&native_errno&&set,"native UCRT entrypoints");invalid_handler previous=set(on_invalid);
    const wchar_t *formats[]={L"%s/%s",L"%s/11bitstudios/%s",L"%s/%s.dat",L"_%.3d%s",L"%s/custom_localizations",L"%s.alt",L"|#color=%02x%02x%02x|"};
    wchar_t unicode[]={0xd83d,0xde00,0xd800,'Q',0},embedded[]={'A',0,'B',0},long_text[258];for(unsigned i=0;i<257;++i)long_text[i]=(wchar_t)('A'+i%26);long_text[257]=0;
    const wchar_t *lefts[]={L"C:/game/path",L"",unicode,NULL,embedded,long_text};
    const wchar_t *rights[]={L"resource",L"",unicode,NULL,L"Z",L"tail"};
    const uint64_t integers[]={0,7,UINT64_C(0xa5a5a5a5fffffff9),UINT64_C(0xffffffff80000000),999,1234567890};
    FILE *file=fopen(argv[1],"wb");require(file!=NULL,"open vector output");uint32_t header[]={UINT32_C(0x31504657),0};bytes(file,header,sizeof(header));
    for(unsigned kind=0;kind<6;++kind)for(unsigned sample=0;sample<6;++sample)for(unsigned mode=0;mode<11;++mode)one(file,kind,formats[kind],lefts[sample],rights[sample],integers[sample],mode);
    const uint32_t colors[]={0,1,9,15,16,255,256,4095,65535,65536,0x7fffffff,0x80000000,0xffffffff,0x12345678,0xabcdef01,0x80000001};
    for(unsigned sample=0;sample<16;++sample){uint64_t rgb[3];for(unsigned i=0;i<3;++i)rgb[i]=(UINT64_C(0xa5b6c7d8)+i)<<32|colors[(sample+i*5)%16];
        for(unsigned mode=0;mode<11;++mode)one(file,6,formats[6],(const wchar_t*)rgb,NULL,0,mode);}
    for(unsigned sample=0;sample<256;++sample){uint64_t rgb[]={UINT64_C(0xaabbccdd00000000)|sample,UINT64_C(0x1234567800000000)|(255-sample),UINT64_C(0xfedcba9800000000)|(sample^0xa5)};
        for(unsigned mode=0;mode<3;++mode)one(file,6,formats[6],(const wchar_t*)rgb,NULL,0,mode);}
    require(!fseek(file,4,SEEK_SET)&&fwrite(&total,4,1,file)==1&&!fclose(file),"finish exact vector count");set(previous);
    printf("{\"success\":true,\"vectors\":%u,\"valid\":%u,\"invalid_callbacks\":%u,\"formats\":7,\"old_format_vectors\":396,\"color_vectors\":944,\"color_three_U32_high32_ignored\":true,\"LastError_preserved\":true,\"alt_single_UTF16_argument\":true,\"original_game_executed\":false}\n",total,valid,invalid);return 0;
}
