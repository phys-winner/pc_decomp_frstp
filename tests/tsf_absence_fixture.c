/* Staged real Core/COM/message providers. No PE entry or native input changes. */
#include "../android/host/win64.c"
#include "../android/host/win64_com.c"
#define allocations message_allocations
#include "../android/host/win64_error_message.c"
#undef allocations
#define win64_com_call unrelated_com_decline
#define win64_error_message_call unrelated_message_decline
#define win64_dinput_call unrelated_dinput_decline
#include "../build/import-routing-acceleration/decline-stubs.h"
#undef win64_com_call
#undef win64_error_message_call
#undef win64_dinput_call
int win64_stdio_call(CPU*c,const char*n){(void)c;(void)n;return 0;}
int win64_window_input_snapshot(uint64_t hwnd,uint8_t keys[256]){(void)hwnd;memset(keys,0,256);return -1;}
void dispatch(CPU*c,uint64_t target){(void)c;(void)target;abort();}
void win64_jump_set_round(unsigned mode){(void)mode;abort();}
static _Thread_local unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(fixture_teb))abort();memcpy(&v,fixture_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(fixture_teb))abort();memcpy(fixture_teb+offset,&value,bytes);}
static _Atomic unsigned checked_calls;
static _Thread_local uint32_t initial_error=0x5a17;
static const unsigned char tsf_class[16]={0x6b,0x9e,0x9a,0x52,0x87,0x65,0x23,0x4f,0xab,0x9e,0x9c,0x7d,0x68,0x3e,0x3c,0x50};
static const unsigned char tsf_iid[16]={0x01,0xe8,0x80,0xaa,0x21,0x20,0xd2,0x11,0x93,0xe0,0x00,0x60,0xb0,0x67,0xb8,0x6e};
static const unsigned char di_class[16]={0xe4,0x09,0xe6,0x25,0x59,0xb2,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const unsigned char di_iid[16]={0x31,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static const unsigned char bad_guid[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static void need(int condition,const char*why){if(!condition){fprintf(stderr,"TSF_ABSENCE_FIXTURE_FAIL %s\n",why);abort();}}
static uint64_t invoke(const char*dll,const char*name,uint64_t a,uint64_t b,uint64_t d,uint64_t e,
                       uint64_t f,uint64_t g,uint64_t h,int direct,int handled,uint32_t error){
 unsigned char stack[8192],before_stack[8192];memset(stack,0x6b,sizeof(stack));uint64_t *sp=(uint64_t *)(void *)(stack+4096);sp[5]=f;sp[6]=g;sp[7]=h;memcpy(before_stack,stack,sizeof(stack));
 CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)sp;c.rcx=a;c.rdx=b;c.r8=d;c.r9=e;CPU before=c;
 win64_region_add((uintptr_t)stack,sizeof(stack),0);win64_set_error(initial_error);crt_errno=31337;errno=1927;
 int result=direct==1?win64_com_call(&c,name):direct==2?win64_error_message_call(&c,name):win64_dispatch_import(&c,dll,name);
 need(result==handled,"handled/DLL/name profile");uint64_t value=c.rax;
 if(handled){if(strcmp(name,"CoUninitialize"))c.rax=before.rax;if(!direct)before.rsp+=8;}
 need(!memcmp(&c,&before,sizeof(c)),"full CPU and direct-no-pop/common-single-pop");
 need(!memcmp(stack,before_stack,sizeof(stack)),"entire8192-byte stack unchanged");
 need(win64_get_error()==error&&crt_errno==31337&&errno==1927,"LastError/guesterrno/nativeerrno policy");
 win64_region_remove((uintptr_t)stack);atomic_fetch_add(&checked_calls,1);return value;
}
static uint32_t apartment(uint32_t flags){return(uint32_t)invoke("ole32.dll","CoInitializeEx",0,flags,0,0,0,0,0,0,1,0x5a17);}
static void uninitialize(void){invoke("OLE32.DLL","CoUninitialize",0,0,0,0,0,0,0,0,1,0x5a17);}
static uint32_t tsf(int routed,uint64_t output){return(uint32_t)invoke("OlE32.DlL","CoCreateInstance",(uintptr_t)tsf_class,0,UINT64_C(0xabcdef0000000001),(uintptr_t)tsf_iid,output,0,0,!routed,1,0x5a17);}
static void *worker(void *unused){
 (void)unused;uint64_t out[3]={0x1122334455667788,0xa5a5a5a5a5a5a5a5,0x8877665544332211};win64_region_add((uintptr_t)out,sizeof(out),1);
 for(unsigned ready=0;ready<2;++ready){if(ready)need(apartment(0)==0,"worker own MTA");for(unsigned i=0;i<512;++i){out[1]=UINT64_MAX;need(tsf(i&1,(uintptr_t)&out[1])==(ready?0x80040154u:0x800401f0u)&&!out[1],"thread-local TSF absence");need(out[0]==0x1122334455667788&&out[2]==0x8877665544332211,"outputcanaries");}}
 uninitialize();win64_region_remove((uintptr_t)out);return NULL;
}
typedef struct {uint32_t identifier,language,capacity,null_output,initial,result,error,crt;uint16_t buffer[1040];} wide_record;
_Static_assert(sizeof(wide_record)==2112,"native format record");
static void wide_corpus(const char *path){
 FILE *file=fopen(path,"rb");need(file!=NULL,"native corpus");uint32_t header[2];need(fread(header,sizeof(header),1,file)==1&&header[0]==0x47534d57&&header[1]==240,"native header");
 uint16_t buffer[1040];win64_region_add((uintptr_t)buffer,sizeof(buffer),1);
 for(unsigned i=0;i<header[1];++i){wide_record row;need(fread(&row,sizeof(row),1,file)==1,"native row");memset(buffer,0xa5,sizeof(buffer));
  initial_error=row.initial;
  uint64_t result=invoke(i%3==2?"kernelbase.dll":"KERNEL32.DLL","FormatMessageW",UINT64_C(0xfeca000000001000),0,UINT64_C(0xaabb000000000000)|row.identifier,UINT64_C(0xaabb000000000000)|row.language,
    row.null_output?0:(uintptr_t)&buffer[8],UINT64_C(0xaabb000000000000)|row.capacity,0,i%3==0?2:0,1,row.error);
  need(result==row.result&&!memcmp(buffer,row.buffer,sizeof(buffer)),"native return/full UTF16 prefix/NUL/guards");
 }initial_error=0x5a17;need(fgetc(file)==EOF&&!fclose(file),"native EOF");win64_region_remove((uintptr_t)buffer);
}
int main(int argc,char **argv){
 const unsigned char *guids[]={tsf_class,tsf_iid,di_class,di_iid,bad_guid};for(unsigned i=0;i<5;++i)win64_region_add((uintptr_t)guids[i],16,0);
 uint64_t out[3]={0x1122334455667788,UINT64_MAX,0x8877665544332211};win64_region_add((uintptr_t)out,sizeof(out),1);
 if(argc==2){
  if(!strcmp(argv[1],"--unknown-module")){static const char name[]="imm32-unavailable-other.dll";win64_region_add((uintptr_t)name,sizeof(name),0);invoke("kernel32.dll","LoadLibraryA",(uintptr_t)name,0,0,0,0,0,0,0,1,126);}
  else if(!strcmp(argv[1],"--readonly-output")){win64_region_remove((uintptr_t)out);win64_region_add((uintptr_t)out,sizeof(out),0);tsf(1,(uintptr_t)&out[1]);}
  else if(!strcmp(argv[1],"--short-output")){unsigned char bytes[7];win64_region_add((uintptr_t)bytes,sizeof(bytes),1);tsf(1,(uintptr_t)bytes);}
  else if(!strcmp(argv[1],"--bad-CLSID"))invoke("ole32.dll","CoCreateInstance",1,0,1,(uintptr_t)tsf_iid,(uintptr_t)&out[1],0,0,0,1,0x5a17);
  else if(!strcmp(argv[1],"--wide-readonly")){uint16_t text[43];win64_region_add((uintptr_t)text,sizeof(text),0);invoke("kernel32.dll","FormatMessageW",0x1000,0,126,0,(uintptr_t)text,43,0,0,1,0x5a17);}
  else return 2;return 99;
 }
 need(argc==3,"W50/W126 datasets");
 for(unsigned ready=0;ready<2;++ready){if(ready){need(apartment(2)==0,"STA");need(apartment(6)==1,"S_FALSE balance");need(apartment(0)==0x80010106u,"changed mode");}
  for(unsigned routed=0;routed<2;++routed){out[1]=UINT64_MAX;need(tsf(routed,(uintptr_t)&out[1])==(ready?0x80040154u:0x800401f0u)&&!out[1],"TSF native failure");need(tsf(routed,0)==0x80070057u,"NULLout priority");}
 }
 out[1]=UINT64_MAX;need(invoke("OLE32.dll","CoCreateInstance",(uintptr_t)di_class,0,1,(uintptr_t)di_iid,(uintptr_t)&out[1],0,0,0,1,0x5a17)==0&&out[1],"unchanged real DI class first");
 uint64_t table;memcpy(&table,win64_pointer(out[1],8,0),8);uint64_t release;memcpy(&release,win64_pointer(table+16,8,0),8);CPU release_cpu={0};release_cpu.rcx=out[1];need(win64_dinput_dispatch(&release_cpu,release)&&!release_cpu.rax,"real DI Release");
 pthread_t threads[4];for(unsigned i=0;i<4;++i)need(!pthread_create(&threads[i],NULL,worker,NULL),"worker create");for(unsigned i=0;i<4;++i)need(!pthread_join(threads[i],NULL),"worker join");
 uninitialize();out[1]=UINT64_MAX;need(tsf(1,(uintptr_t)&out[1])==0x80040154u,"one STA reference remains");uninitialize();out[1]=UINT64_MAX;need(tsf(1,(uintptr_t)&out[1])==0x800401f0u,"balanced teardown");
 for(unsigned profile=0;profile<5;++profile){out[1]=UINT64_MAX;
  invoke(profile==3?"user32.dll":"ole32.dll","CoCreateInstance",profile==0?(uintptr_t)bad_guid:(uintptr_t)tsf_class,profile==1?1:0,profile==2?2:1,profile==0?(uintptr_t)tsf_iid:(uintptr_t)bad_guid,(uintptr_t)&out[1],0,0,0,0,0x5a17);need(out[1]==UINT64_MAX,"unknown COM profile untouched");
 }
 static const char *names[]={"imm32.dll","IMM32.DLL","C:\\Windows\\System32\\imm32.dll"};
 for(unsigned i=0;i<3;++i){uint16_t wide[64]={0};for(unsigned n=0;names[i][n];++n)wide[n]=(unsigned char)names[i][n];win64_region_add((uintptr_t)names[i],strlen(names[i])+1,0);win64_region_add((uintptr_t)wide,sizeof(wide),0);
  for(unsigned api=0;api<4;++api){const char *apis[]={"LoadLibraryA","LoadLibraryW","GetModuleHandleA","GetModuleHandleW"};size_t modules_before=module_count,exports_before=export_count;
   need(!invoke(api&2?"kernelbase.dll":"KERNEL32.dll",apis[api],api&1?(uintptr_t)wide:(uintptr_t)names[i],0,0,0,0,0,0,0,1,126),"IMM unavailable");need(module_count==modules_before&&export_count==exports_before,"no module/handle fabricated");}
  win64_region_remove((uintptr_t)wide);win64_region_remove((uintptr_t)names[i]);
 }
 wide_corpus(argv[1]);wide_corpus(argv[2]);win64_com_self_test();win64_error_message_self_test();
 need(out[0]==0x1122334455667788&&out[2]==0x8877665544332211,"final output canaries");
 printf("{\"success\":true,\"checked_calls\":%u,\"parallel_TSF_calls\":4096,\"native_W50_cases\":240,\"native_W126_cases\":240,\"IMM_loader_cases\":12,\"real_Core_COM_DInput_messages\":true,\"unrelated_providers_decline_doubles\":true,\"full_CPU_stack8192_errors_singlepop\":true,\"game_boot_proven\":false}\n",atomic_load(&checked_calls));return 0;
}
