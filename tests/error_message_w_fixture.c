/* Reuse actual production allocator/validator and owned ANSI message fixture. */
#define win64_dispatch_import unused_full_core_dispatch
#define main ansi_fixture_main
#include "error_message_fixture.c"
#undef main
#undef win64_dispatch_import
#include <errno.h>
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;return 0;}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;return 0;}
static int unrelated_notification(CPU*c){(void)c;return 0;}
#include "wide_message_router.inc"
typedef struct{uint32_t identifier,language,capacity,null_output,initial,result,error,crt;uint16_t buffer[1040];} wide_record;
_Static_assert(sizeof(wide_record)==2112,"native wide record ABI");
static _Atomic unsigned wide_checks,wide_calls;
static void verify(int ok,const char*why){if(!ok){fprintf(stderr,"FORMAT_MESSAGE_W_FIXTURE_FAIL %s\n",why);_Exit(1);}++wide_checks;}
static uint64_t wide_call(workspace*w,uint32_t identifier,uint32_t language,uint64_t destination,uint32_t capacity,uint32_t initial,uint32_t expected,int routed){
 CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=UINT64_C(0xfeca123400001000);c.rdx=0;c.r8=UINT64_C(0xaabbccdd00000000)|identifier;c.r9=UINT64_C(0xfedcba9800000000)|language;c.rsp=(uintptr_t)w->stack;w->stack[5]=destination;w->stack[6]=UINT64_C(0xfeca123400000000)|capacity;w->stack[7]=0;b=c;
 win64_set_error(initial);win64_stdio_set_errno(1234);errno=2718;verify(routed?win64_dispatch_import(&c,routed==1?"KERNEL32.DLL":"kernelbase.dll","FormatMessageW"):win64_error_message_call(&c,"FormatMessageW"),"handled");
 uint64_t result=c.rax;verify(result<=UINT32_MAX,"DWORD zeroextension");verify(win64_get_error()==expected&&crt_errno==1234&&errno==2718,"LastError/native and guest errno");c.rax=b.rax;if(routed)b.rsp+=8;verify(!memcmp(&c,&b,sizeof(c)),"fullCPU and solecommonpop");++wide_calls;return result;
}
static void *wide_worker(void*unused){(void)unused;workspace w;unsigned char buffer[65];win64_region_add((uintptr_t)&w,sizeof(w),1);win64_region_add((uintptr_t)buffer,sizeof(buffer),1);
 for(unsigned i=0;i<1024;++i){memset(buffer,0xa5,sizeof(buffer));if(wide_call(&w,50,0,(uintptr_t)buffer+1,1024,1927,1927,i%3)!=31)abort();if(buffer[0]!=0xa5)abort();for(unsigned n=0;n<32;++n){uint16_t unit;memcpy(&unit,buffer+1+n*2,2);if(unit!=(unsigned char)unsupported_request[n])abort();}}
 win64_region_remove((uintptr_t)buffer);win64_region_remove((uintptr_t)&w);return NULL;
}
int main(int argc,char**argv){
 win64_error_message_self_test();workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
 if(argc==2&&argv[1][0]=='-'){
  uint16_t buffer[32];win64_region_add((uintptr_t)buffer,sizeof(buffer),1);CPU c={0};c.rcx=0x1000;c.r8=50;c.rsp=(uintptr_t)w.stack;w.stack[5]=(uintptr_t)buffer;w.stack[6]=32;w.stack[7]=0;
  if(!strcmp(argv[1],"--flags"))c.rcx=0x1300;else if(!strcmp(argv[1],"--source"))c.rdx=1;else if(!strcmp(argv[1],"--inserts"))w.stack[7]=1;else if(!strcmp(argv[1],"--language"))c.r9=0x419;
  else if(!strcmp(argv[1],"--invalid-output"))w.stack[5]=1;else if(!strcmp(argv[1],"--short-output")){unsigned char bytes[63];win64_region_add((uintptr_t)bytes,63,1);w.stack[5]=(uintptr_t)bytes;}
  else if(!strcmp(argv[1],"--readonly")){unsigned char bytes[64];win64_region_add((uintptr_t)bytes,64,0);w.stack[5]=(uintptr_t)bytes;}else return 2;
  win64_error_message_call(&c,"FormatMessageW");return 99;
 }
 verify(argc==2,"nativecorpusarg");FILE*f=fopen(argv[1],"rb");verify(f!=NULL,"nativefile");uint32_t header[2];verify(fread(header,sizeof(header),1,f)==1&&header[0]==0x47534d57&&header[1]==240,"nativeheader");
 uint16_t buffer[1040];win64_region_add((uintptr_t)buffer,sizeof(buffer),1);for(unsigned i=0;i<header[1];++i){wide_record row;verify(fread(&row,sizeof(row),1,f)==1,"row");memset(buffer,0xa5,sizeof(buffer));verify(wide_call(&w,row.identifier,row.language,row.null_output?0:(uintptr_t)&buffer[8],row.capacity,row.initial,row.error,i%3)==row.result,"nativereturn");verify(!memcmp(buffer,row.buffer,sizeof(buffer)),"fullnativeUTF16/prefix/NUL/guards");}verify(fgetc(f)==EOF&&!fclose(f),"nativeEOF");
 verify(wide_call(&w,50,0,1,0,17,122,1)==0,"zero capacity doesnotdereference output");
 unsigned char exact[65];win64_region_add((uintptr_t)exact,sizeof(exact),1);memset(exact,0xa5,sizeof(exact));verify(wide_call(&w,50,0,(uintptr_t)exact+1,UINT32_MAX,17,17,2)==31&&exact[0]==0xa5,"actual64write/oddalignment/DWORDcapacity");
 CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(17);verify(!win64_dispatch_import(&c,"USER32.dll","FormatMessageW")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==17,"DLLgate");verify(!win64_error_message_call(&c,"FormatMessageUnexpected")&&!memcmp(&c,&b,sizeof(c)),"unknownAPI");
 pthread_t threads[4];for(unsigned i=0;i<4;++i)verify(!pthread_create(&threads[i],NULL,wide_worker,NULL),"threadcreate");for(unsigned i=0;i<4;++i)verify(!pthread_join(threads[i],NULL),"threadjoin");
 verify(!atomic_load(&record_count)&&!error_allocations,"no owned allocation leak");printf("{\"success\":true,\"checks\":%u,\"calls\":%u,\"native_cases\":240,\"parallel_calls\":4096,\"full_CPU_single_pop\":true,\"LastError_errno\":true,\"direct_UTF16_ID50\":true,\"game_boot_proven\":false}\n",wide_checks,wide_calls);return 0;
}
