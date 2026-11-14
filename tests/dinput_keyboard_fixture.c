/* Production COM provider and production guest-memory validation; the snapshot
 * fixture supplies a deterministic physical keyboard state without a second
 * Android input consumer. Root's window fixture tests the real event producer. */
#include "../android/host/win64.c"
#include "win64_dinput.h"
#include "dinput_native.h"
#include <stdatomic.h>
static _Thread_local unsigned char test_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned n){uint64_t value=0;if(offset+n>sizeof(test_teb)||n>8)abort();memcpy(&value,test_teb+offset,n);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned n){if(offset+n>sizeof(test_teb)||n>8)abort();memcpy(test_teb+offset,&value,n);}
static const uint64_t HWND=UINT64_C(0x701);
static _Atomic int focused=1;static unsigned char physical_keys[256];
int win64_window_input_snapshot(uint64_t hwnd,uint8_t keys[256]){if(hwnd!=HWND)return -1;if(!atomic_load(&focused)){memset(keys,0,256);return 0;}memcpy(keys,physical_keys,256);return 1;}
static _Atomic unsigned checks,native_checks;
static void require(int condition,const char *why){++checks;if(!condition){fprintf(stderr,"DINPUT_FIXTURE_FAIL %s\n",why);abort();}}
static void native_equal(const char *label,uint32_t value){for(unsigned i=0;i<sizeof(native_results)/sizeof(native_results[0]);++i)if(!strcmp(label,native_results[i].label)){require(value==native_results[i].result,label);++native_checks;return;}require(0,"missing native row");}
static unsigned char unknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static unsigned char iid[16]={0x30,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static unsigned char device_iid[16]={0x80,0x10,0xd4,0x54,0x15,0xdc,0x33,0x48,0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79};
static unsigned char keyboard[16]={0x61,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static unsigned char key_guid[16]={0x20,0x82,0x72,0x55,0x3c,0xd3,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static unsigned char bad_guid[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
typedef struct{uint64_t guid;uint32_t offset,type,flags,pad;} key_format;
typedef struct{uint32_t size,object_size,flags,bytes,count,pad;uint64_t keys;} keyboard_format;
static key_format keys[256];static keyboard_format format;
static uint32_t invoke(uint64_t target,const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e,uint64_t f){
 uint64_t stack[8]={UINT64_C(0x141234567),0,0,0,0,f,0,0};CPU cpu;memset(&cpu,0xa5,sizeof(cpu));cpu.rsp=(uintptr_t)stack;cpu.rcx=a;cpu.rdx=b;cpu.r8=d;cpu.r9=e;CPU before=cpu;
 win64_region_add((uintptr_t)stack,sizeof(stack),0);win64_set_error(0x5a17);errno=1927;
 int handled=name?win64_dinput_call(&cpu,name):win64_dinput_dispatch(&cpu,target);require(handled,"known call handled");uint32_t result=(uint32_t)cpu.rax;require(cpu.rax==result,"HRESULT/ULONG zero-extended");cpu.rax=before.rax;
 require(!memcmp(&cpu,&before,sizeof(cpu)),"CPU including pre-pop RSP preserved");require(errno==1927&&win64_get_error()==0x5a17,"adapter errno/LastError policy");require(stack[0]==UINT64_C(0x141234567)&&stack[5]==f,"guest shadow stack unchanged");win64_region_remove((uintptr_t)stack);return result;
}
static uint64_t method(uint64_t object,unsigned slot){uint64_t table,target;memcpy(&table,win64_pointer(object,8,0),8);memcpy(&target,win64_pointer(table+slot*8,8,0),8);return target;}
static uint32_t call(uint64_t object,unsigned slot,uint64_t a,uint64_t b,uint64_t d){return invoke(method(object,slot),NULL,object,a,b,d,0);}
static uint64_t new_root(void){uint64_t output[3]={0x11223344,0xa5a5a5a5,0x55667788};win64_region_add((uintptr_t)output,sizeof(output),1);uint32_t result=invoke(0,"DirectInput8Create",0x140000000,0x800,(uintptr_t)iid,(uintptr_t)&output[1],0);native_equal("create:valid",result);require(output[0]==0x11223344&&output[2]==0x55667788&&output[1],"Create8 output/canaries");uint64_t object=output[1];win64_region_remove((uintptr_t)output);return object;}
static uint64_t new_device(uint64_t root){uint64_t output=0;win64_region_add((uintptr_t)&output,8,1);native_equal("device:keyboard",call(root,3,(uintptr_t)keyboard,(uintptr_t)&output,0));require(output!=0,"real keyboard object");win64_region_remove((uintptr_t)&output);return output;}
static void configure(uint64_t device){native_equal("configured:SetCooperativeLevel",call(device,13,HWND,0x16,0));native_equal("configured:SetDataFormat",call(device,11,(uintptr_t)&format,0,0));}
static void setup(void){
 unsigned char *guids[]={unknown,iid,device_iid,keyboard,key_guid,bad_guid};for(unsigned i=0;i<6;++i)win64_region_add((uintptr_t)guids[i],16,0);
 for(unsigned i=0;i<256;++i)keys[i]=(key_format){(uintptr_t)key_guid,i,UINT32_C(0x8000000c)|(i<<8),0,0};format=(keyboard_format){32,24,2,256,256,0,(uintptr_t)keys};win64_region_add((uintptr_t)keys,sizeof(keys),0);win64_region_add((uintptr_t)&format,sizeof(format),0);physical_keys[0x1e]=physical_keys[0x9d]=physical_keys[0xc8]=0x80;
}
static void state_checks(uint64_t device){
 unsigned char output[272];memset(output,0xa5,sizeof(output));win64_region_add((uintptr_t)output,sizeof(output),1);
 native_equal("configured:State-before-Acquire",call(device,9,256,(uintptr_t)output+8,0));require(output[8]==0xa5,"unacquired output untouched");
 atomic_store(&focused,0);native_equal("configured:Acquire",call(device,7,0,0,0));atomic_store(&focused,1);require(call(device,7,0,0,0)==0,"focused physical keyboard acquire");require(call(device,7,0,0,0)==1,"repeated acquisition no effect");
 require(call(device,9,256,(uintptr_t)output+8,0)==0,"GetDeviceState success");require(!memcmp(output+8,physical_keys,256),"exact physical DIK state copied");for(unsigned i=0;i<8;++i)require(output[i]==0xa5&&output[i+264]==0xa5,"state boundary canaries");
 require(call(device,9,255,(uintptr_t)output+8,0)==0x80070057,"state size invalid");require(call(device,13,HWND,0x16,0)==0x800700aa,"cannot reconfigure acquired keyboard");require(call(device,11,(uintptr_t)&format,0,0)==0x800700aa,"cannot reformat acquired keyboard");
 atomic_store(&focused,0);require(call(device,9,256,(uintptr_t)output+8,0)==0x8007001e,"real focus loss deacquires");native_equal("configured:State-before-Acquire",call(device,9,256,(uintptr_t)output+8,0));atomic_store(&focused,1);require(call(device,7,0,0,0)==0,"reacquire after focus gain");require(call(device,8,0,0,0)==0,"Unacquire success");native_equal("configured:Unacquire-repeat",call(device,8,0,0,0));win64_region_remove((uintptr_t)output);
}
typedef struct{uint64_t device;unsigned calls;} parallel_input;
static _Atomic int stderr_locked;
static void *hold_stderr(void *opaque){(void)opaque;flockfile(stderr);atomic_store(&stderr_locked,1);for(;;)pause();return NULL;}
static void *parallel(void *opaque){parallel_input *input=opaque;unsigned char output[256];win64_region_add((uintptr_t)output,sizeof(output),1);for(unsigned i=0;i<512;++i){uint32_t rc=call(input->device,9,256,(uintptr_t)output,0);if(rc||memcmp(output,physical_keys,256))abort();++input->calls;}win64_region_remove((uintptr_t)output);return NULL;}
int main(int argc,char **argv){setup();win64_dinput_self_test();uint64_t root=new_root(),device=new_device(root);configure(device);
 if(argc==3&&!strcmp(argv[1],"--negative")){
  if(!strcmp(argv[2],"stale")){call(device,2,0,0,0);call(device,7,0,0,0);}
  else if(!strcmp(argv[2],"bad-this"))invoke(WIN64_DINPUT_SENTINEL_BASE+8,NULL,1,0,0,0,0);
  else if(!strcmp(argv[2],"unaligned"))invoke(WIN64_DINPUT_SENTINEL_BASE+1,NULL,root,0,0,0,0);
  else if(!strcmp(argv[2],"unsupported"))call(device,10,0,0,0);
  else if(!strcmp(argv[2],"short-output")){uint8_t out[255];win64_region_add((uintptr_t)out,sizeof(out),1);call(device,7,0,0,0);call(device,9,256,(uintptr_t)out,0);}
  else if(!strcmp(argv[2],"readonly-output")){uint8_t out[256];win64_region_add((uintptr_t)out,sizeof(out),0);call(device,7,0,0,0);call(device,9,256,(uintptr_t)out,0);}
  else if(!strcmp(argv[2],"format-overflow")){keyboard_format f=format;f.keys=UINT64_MAX-10;win64_region_add((uintptr_t)&f,sizeof(f),0);call(device,11,(uintptr_t)&f,0,0);}
  else if(!strcmp(argv[2],"null-output")){call(device,7,0,0,0);call(device,9,256,0,0);}
  else if(!strcmp(argv[2],"locked-stderr")){pthread_t t;if(pthread_create(&t,NULL,hold_stderr,NULL))return 2;while(!atomic_load(&stderr_locked))sched_yield();invoke(WIN64_DINPUT_SENTINEL_BASE+8,NULL,1,0,0,0,0);}
  else return 2;return 99;
 }
 state_checks(device);uint64_t out=0;win64_region_add((uintptr_t)&out,8,1);
 native_equal("QI-root:Input8A",call(root,0,(uintptr_t)iid,(uintptr_t)&out,0));require(out==root,"same Input8A identity");require(call(root,2,0,0,0)==1,"release queried root ref");
 native_equal("QI-device:Device8A",call(device,0,(uintptr_t)device_iid,(uintptr_t)&out,0));require(out==device,"same device identity");require(call(device,2,0,0,0)==1,"release queried device ref");
 native_equal("QI-root:unsupported",call(root,0,(uintptr_t)bad_guid,(uintptr_t)&out,0));require(!out,"unknown IID NULLoutput");native_equal("device:unknown",call(root,3,(uintptr_t)bad_guid,(uintptr_t)&out,0));require(!out,"unknown GUID NULLoutput");
 native_equal("QI-root:NULL-output",call(root,0,(uintptr_t)iid,0,0));native_equal("device:NULL-output",call(root,3,(uintptr_t)keyboard,0,0));native_equal("format:NULL",call(device,11,0,0,0));
 native_equal("cooperative:0",call(device,13,HWND,0,0));native_equal("cooperative:ffffffff",call(device,13,HWND,UINT32_MAX,0));native_equal("cooperative:invalid-hwnd-1",call(device,13,1,0x16,0));
 uint32_t versions[]={0,0x700,0x900};const char *version_labels[]={"create:version0","create:version700","create:version900"};
 for(unsigned i=0;i<3;++i){out=0x5555;native_equal(version_labels[i],invoke(0,"DirectInput8Create",0x140000000,versions[i],(uintptr_t)iid,(uintptr_t)&out,0));require(!out,"invalid version NULLout");}
 native_equal("create:NULL-output",invoke(0,"DirectInput8Create",0x140000000,0x800,(uintptr_t)iid,0,0));
 win64_region_remove((uintptr_t)&out);require(call(device,7,0,0,0)==0,"parallel acquire");pthread_t threads[4];parallel_input inputs[4];unsigned parallel_calls=0;
 for(unsigned i=0;i<4;++i){inputs[i]=(parallel_input){device,0};require(!pthread_create(threads+i,NULL,parallel,inputs+i),"worker create");}
 for(unsigned i=0;i<4;++i){require(!pthread_join(threads[i],NULL),"worker join");parallel_calls+=inputs[i].calls;}require(call(device,8,0,0,0)==0,"final unacquire");require(call(device,2,0,0,0)==0,"device ref reaches zero");require(call(root,2,0,0,0)==0,"root ref reaches zero");
 printf("{\"success\":true,\"checks\":%u,\"native_HRESULT_comparisons\":%u,\"parallel_calls\":%u,\"keyboard_state_genuine_snapshot\":true,\"full_CPU_and_RSP_preserved\":true,\"physical_event_producer_mocked\":true,\"game_boot_proven\":false}\n",checks,native_checks,parallel_calls);return 0;
}
