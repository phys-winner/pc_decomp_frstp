#define SDL_DINPUT_FIXTURE_ENTRY staged_profile_main
/* The unchanged A-profile fixture runs first against the real DI provider. */
#define main keyboard_profile_main
#include "dinput_keyboard_fixture.c"
#undef main
#include "sdl_native.h"
static unsigned char clsid_w[16]={0xe4,0x09,0xe6,0x25,0x59,0xb2,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static unsigned char iid_w[16]={0x31,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static unsigned w_native_checks;
static void w_native(const char *label,uint32_t value){for(unsigned i=0;i<sizeof(sdl_native_results)/sizeof(sdl_native_results[0]);++i)if(!strcmp(label,sdl_native_results[i].label)){require(value==sdl_native_results[i].result,label);++w_native_checks;return;}require(0,"missing SDL native result");}
static uint32_t create_w(uint64_t cls,uint64_t requested,uint64_t *out,int ready,int expected_handled){
 uint64_t stack[8]={UINT64_C(0x141b647ff),0,0,0,0,(uintptr_t)out,0,0};CPU cpu;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=cls;cpu.rdx=0;cpu.r8=1;cpu.r9=requested;cpu.rsp=(uintptr_t)stack;CPU before=cpu;win64_region_add((uintptr_t)stack,sizeof(stack),0);win64_set_error(0x5a17);errno=1927;
 int handled=win64_dinput_com_call(&cpu,"CoCreateInstance",ready);require(handled==expected_handled,"exact class routing");uint32_t result=(uint32_t)cpu.rax;if(handled)cpu.rax=before.rax;require(!memcmp(&cpu,&before,sizeof(cpu)),"COM fullCPU/noRSPpop");require(errno==1927&&win64_get_error()==0x5a17,"COM adapter nativeerrno/LastError policy");require(stack[0]==UINT64_C(0x141b647ff)&&stack[5]==(uintptr_t)out,"COM stack unchanged");win64_region_remove((uintptr_t)stack);return result;
}
static uint32_t enumerate(uint64_t object,uint32_t kind,uint32_t flags){return invoke(method(object,4),NULL,object,kind,UINT64_C(0x141b645b0),UINT64_C(0x12345678),flags);}
static void null_callback(uint64_t object,const char *phase){
 uint64_t guard[3]={UINT64_C(0x1122334455667788),UINT64_C(0xa5a5a5a5a5a5a5a5),UINT64_C(0x8877665544332211)},before[3];memcpy(before,guard,sizeof(guard));win64_region_add((uintptr_t)guard,sizeof(guard),1);
 char label[96];snprintf(label,sizeof(label),"%s:GAMECTRL-NULL-callback",phase);w_native(label,invoke(method(object,4),NULL,object,4,0,(uintptr_t)&guard[1],1));require(!memcmp(guard,before,sizeof(guard)),"NULL callback GAMECTRL context untouched");
 snprintf(label,sizeof(label),"%s:ALL-forcefeedback-NULL-callback",phase);w_native(label,invoke(method(object,4),NULL,object,0,0,0,0x101));require(!memcmp(guard,before,sizeof(guard)),"NULL callback ALL context untouched");win64_region_remove((uintptr_t)guard);
}
#ifndef SDL_DINPUT_FIXTURE_ENTRY
#define SDL_DINPUT_FIXTURE_ENTRY main
#endif
int SDL_DINPUT_FIXTURE_ENTRY(int argc,char **argv){
 require(!keyboard_profile_main(1,argv),"existing keyboardA regression");
 win64_region_add((uintptr_t)clsid_w,16,0);win64_region_add((uintptr_t)iid_w,16,0);uint64_t outputs[3]={0x11223344,0x5555,0x55667788};win64_region_add((uintptr_t)outputs,sizeof(outputs),1);
 create_w((uintptr_t)bad_guid,(uintptr_t)iid_w,&outputs[1],1,0);require(outputs[1]==0x5555,"unknown class left for normal COM handler");
 w_native("CoCreate:without-apartment",create_w((uintptr_t)clsid_w,(uintptr_t)iid_w,&outputs[1],0,1));require(!outputs[1],"no apartment NULLout");
 w_native("CoCreate:unsupported-IID",create_w((uintptr_t)clsid_w,(uintptr_t)bad_guid,&outputs[1],1,1));require(!outputs[1],"unsupportedIIDNULLout");
 w_native("CoCreate:DirectInput8W",create_w((uintptr_t)clsid_w,(uintptr_t)iid_w,&outputs[1],1,1));uint64_t object=outputs[1];require(object&&outputs[0]==0x11223344&&outputs[2]==0x55667788,"CoCreate realobject/outcanaries");
 if(argc==3&&!strcmp(argv[1],"--negative")){
  if(!strcmp(argv[2],"enum-unscoped")){call(object,7,0x140000000,0x800,0);enumerate(object,0,1);}
  else if(!strcmp(argv[2],"wide-createdevice"))call(object,3,(uintptr_t)keyboard,(uintptr_t)&outputs[1],0);
  else return 2;return 99;
 }
 w_native("uninitialized:GAMECTRL",enumerate(object,4,1));w_native("uninitialized:ALL-forcefeedback",enumerate(object,0,0x101));
 null_callback(object,"uninitialized");
 w_native("QI:IUnknown",call(object,0,(uintptr_t)unknown,(uintptr_t)&outputs[1],0));require(outputs[1]==object,"canonical W IUnknown identity");require(call(object,2,0,0,0)==1,"releaseIUnknown");
 w_native("QI:IUnknown-to-Input8W",call(object,0,(uintptr_t)iid_w,(uintptr_t)&outputs[1],0));require(outputs[1]==object,"W interface roundtrip identity");require(call(object,2,0,0,0)==1,"releaseQI W");
 const uint32_t versions[]={0,0x700,0x900};const char *labels[]={"Initialize:version0","Initialize:version700","Initialize:version900"};
 for(unsigned i=0;i<3;++i){w_native(labels[i],call(object,7,0x140000000,versions[i],0));w_native("uninitialized:GAMECTRL",enumerate(object,4,1));}
 w_native("Initialize:module-NULL",call(object,7,0,0x800,0));w_native("uninitialized:GAMECTRL",enumerate(object,4,1));
 w_native("Initialize:main-module-800",call(object,7,0x140000000,0x800,0));w_native("Initialize:repeat",call(object,7,0x140000000,0x800,0));
 w_native("initialized:GAMECTRL-attached",enumerate(object,4,1));w_native("initialized:ALL-forcefeedback-attached",enumerate(object,0,0x101));
 null_callback(object,"initialized");
 for(unsigned i=0;i<512;++i){require(enumerate(object,4,1)==0,"stable emptycontrollerenumeration");require(enumerate(object,0,0x101)==0,"stable emptyhapticenumeration");}
 w_native("Release:final",call(object,2,0,0,0));win64_region_remove((uintptr_t)outputs);
 printf("{\"success\":true,\"staged_only\":false,\"A_profile_regression_included\":true,\"W_native_HRESULT_comparisons\":%u,\"empty_enumerations\":1026,\"no_callback_invocation\":true,\"COM_requires_real_apartment_state\":true,\"production_changes\":false,\"game_boot_proven\":false}\n",w_native_checks);return 0;
}

#include "win64_com.h"
#include "com_route.inc"

static uint32_t com_invoke(int route,const char *dll,const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e,uint64_t fifth,int expected_handled) {
 uint64_t stack[8]={UINT64_C(0x141b647ff),0,0,0,0,fifth,0,0};CPU cpu;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=a;cpu.rdx=b;cpu.r8=d;cpu.r9=e;cpu.rsp=(uintptr_t)stack;CPU before=cpu;win64_region_add((uintptr_t)stack,sizeof(stack),0);win64_set_error(0x5a17);errno=1927;
 int handled=route?staged_com_import(&cpu,dll,name):win64_com_call(&cpu,name);require(handled==expected_handled,"COM real TLS route handled");uint32_t result=(uint32_t)cpu.rax;
 if(handled){if(strcmp(name,"CoUninitialize")){require(cpu.rax==result,"COM HRESULT zeroextend");cpu.rax=before.rax;}if(route)before.rsp+=8;}
 require(!memcmp(&cpu,&before,sizeof(cpu)),"COM lifecycle/factory fullCPU and common single pop");require(errno==1927&&win64_get_error()==0x5a17,"COM lifecycle/factory errno/LastError");require(stack[0]==UINT64_C(0x141b647ff)&&stack[5]==fifth,"COM shadow stack unchanged");win64_region_remove((uintptr_t)stack);return result;
}
static uint32_t real_factory(uint64_t *out){return com_invoke(1,"OLE32.DLL","CoCreateInstance",(uintptr_t)clsid_w,0,1,(uintptr_t)iid_w,(uintptr_t)out,1);}
static uint32_t apartment(uint32_t flags){return com_invoke(1,"ole32.dll","CoInitializeEx",0,flags,0,0,0,1);}
static void uninitialize(void){(void)com_invoke(1,"ole32.dll","CoUninitialize",0,0,0,0,0,1);}
static void release_created(uint64_t object){require(object!=0,"COM returned real W object");require(call(object,7,0x140000000,0x800,0)==0,"COM W Initialize");require(enumerate(object,4,1)==0&&enumerate(object,0,0x101)==0,"COM exact SDL empty controller/haptic profiles");require(call(object,2,0,0,0)==0,"COM W final release");}
static void *apartment_worker(void *unused){
 (void)unused;uint64_t out=0x5555;win64_region_add((uintptr_t)&out,8,1);
 w_native("CoCreate:without-apartment",real_factory(&out));require(!out,"worker does not inherit parent apartment");require(apartment(2)==0,"worker independent STA");w_native("CoCreate:DirectInput8W",real_factory(&out));release_created(out);uninitialize();out=0x5555;w_native("CoCreate:without-apartment",real_factory(&out));require(!out,"worker apartment teardown");win64_region_remove((uintptr_t)&out);return NULL;
}
int main(int argc,char **argv){
 require(!staged_profile_main(argc,argv),"existing A and staged W regression");
 uint64_t out[3]={0x11223344,0x5555,0x55667788};win64_region_add((uintptr_t)out,sizeof(out),1);
 /* This is the first real COM call, so it covers pthread_once/TLS creation. */
 w_native("CoCreate:without-apartment",real_factory(&out[1]));require(!out[1]&&out[0]==0x11223344&&out[2]==0x55667788,"uninitialized COM output/canaries");
 win64_com_self_test();out[1]=0x5555;
 com_invoke(0,NULL,"CoGetClassObject",0,0,0,0,0,0);
 com_invoke(1,"user32.dll","CoCreateInstance",(uintptr_t)clsid_w,0,1,(uintptr_t)iid_w,(uintptr_t)&out[1],0);require(out[1]==0x5555,"wrong DLL untouched");
 com_invoke(1,"ole32.dll","CoCreateInstance",(uintptr_t)bad_guid,0,1,(uintptr_t)iid_w,(uintptr_t)&out[1],0);require(out[1]==0x5555,"unknown class remains unhandled");
 w_native("CoInitializeEx:STA",apartment(2));require(apartment(6)==1,"repeat STA balanced S_FALSE");require(apartment(0)==UINT32_C(0x80010106),"changed-mode does not initialize");
 w_native("CoCreate:DirectInput8W",real_factory(&out[1]));release_created(out[1]);
 pthread_t thread;require(!pthread_create(&thread,NULL,apartment_worker,NULL)&&!pthread_join(thread,NULL),"real COM thread-local factory state");
 uninitialize();w_native("CoCreate:DirectInput8W",real_factory(&out[1]));release_created(out[1]);
 uninitialize();out[1]=0x5555;w_native("CoCreate:without-apartment",real_factory(&out[1]));require(!out[1],"all successful initializations require balancing");
 require(apartment(0)==0,"MTA after fully balanced STA");require(apartment(2)==UINT32_C(0x80010106),"STA conflicts with MTA");uninitialize();
 require(com_invoke(0,NULL,"CoInitialize",0,0,0,0,0,1)==0,"CoInitialize aliases STA");uninitialize();uninitialize();
 out[1]=0x5555;w_native("CoCreate:without-apartment",real_factory(&out[1]));require(!out[1]&&out[0]==0x11223344&&out[2]==0x55667788,"final teardown/no-op preserves guards");win64_region_remove((uintptr_t)out);
 printf("{\"success\":true,\"staged_only\":false,\"real_COM_TLS\":true,\"existing_OLE32_route\":true,\"full_CPU_single_pop_errno_LastError\":true,\"thread_isolation_and_balancing\":true,\"A_and_W_regression_included\":true,\"no_general_class_success\":true,\"game_boot_proven\":false}\n");return 0;
}
