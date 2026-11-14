#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int (__cdecl *callback)(void);
typedef struct {uintptr_t first,last,end;} table_type;
typedef int (__cdecl *init_fn)(table_type *);
typedef int (__cdecl *register_fn)(table_type *,callback);
typedef int (__cdecl *execute_fn)(table_type *);
typedef int (__cdecl *atexit_fn)(void (__cdecl *)(void));
typedef void (__cdecl *cleanup_fn)(void);
typedef void (__cdecl *tls_register_fn)(void (NTAPI *)(void *,DWORD,void *));
static init_fn initialize;static register_fn append;static execute_fn drain;
static table_type table;static int events[32],event_count,counts[4],checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"ONEXIT_ORACLE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static int cb1(void){events[event_count++]=1;return 0;}
static int cb3(void){events[event_count++]=3;return 0;}
static int cb4(void){events[event_count++]=4;return 0;}
static int cb2(void){events[event_count++]=2;if(append(&table,cb3)||append(&table,cb4))abort();return 0;}
static int c0(void){++counts[0];return 0;}static int c1(void){++counts[1];return 0;}
static int c2(void){++counts[2];return 0;}static int c3(void){++counts[3];return 0;}
static DWORD WINAPI worker(void *opaque){callback f[]={c0,c1,c2,c3};unsigned id=(unsigned)(uintptr_t)opaque;
    for(unsigned i=0;i<2000;++i)if(append(&table,f[id]))return 1;return 0;}
static void global1(void){events[event_count++]=1;}static void global2(void){events[event_count++]=2;}
static int tls_valid=1;
static void NTAPI tls_callback(void *module,DWORD reason,void *reserved){if(module||reason!=DLL_PROCESS_DETACH||reserved)tls_valid=0;events[event_count++]=9;}
int main(int argc,char **argv){HMODULE u=GetModuleHandleW(L"ucrtbase.dll");CHECK(u!=NULL);
    initialize=(init_fn)GetProcAddress(u,"_initialize_onexit_table");append=(register_fn)GetProcAddress(u,"_register_onexit_function");drain=(execute_fn)GetProcAddress(u,"_execute_onexit_table");CHECK(initialize&&append&&drain);
    if(argc>1&&!strcmp(argv[1],"--lifecycle")){atexit_fn add=(atexit_fn)GetProcAddress(u,"_crt_atexit");cleanup_fn cexit=(cleanup_fn)GetProcAddress(u,"_cexit"),c_exit=(cleanup_fn)GetProcAddress(u,"_c_exit");tls_register_fn tls=(tls_register_fn)GetProcAddress(u,"_register_thread_local_exe_atexit_callback");CHECK(add&&cexit&&c_exit&&tls);
        tls(tls_callback);CHECK(!add(NULL)&&!add(global1)&&!add(global2));c_exit();CHECK(!event_count);cexit();CHECK(tls_valid&&event_count==3&&events[0]==9&&events[1]==2&&events[2]==1);cexit();CHECK(tls_valid&&event_count==4&&events[3]==9);
        printf("{\"success\":true,\"checks\":%d,\"c_exit_skips_callbacks\":true,\"cexit_order\":[9,2,1],\"repeat_cexit_TLS_only\":true}\n",checks);return 0;}
    CHECK(initialize(NULL)==-1&&append(NULL,cb1)==-1&&drain(NULL)==-1);
    CHECK(!initialize(&table)&&table.first==table.last&&table.first==table.end);
    CHECK(!append(&table,NULL)&&!append(&table,cb1)&&!append(&table,cb2));table_type saved=table;
    CHECK(!initialize(&table)&&!memcmp(&saved,&table,sizeof(table)));CHECK(!drain(&table));
    CHECK(event_count==4&&events[0]==2&&events[1]==4&&events[2]==3&&events[3]==1);
    CHECK(table.first==table.last&&table.first==table.end);CHECK(!drain(&table));
    table=(table_type){0x30,0x20,0x10};saved=table;CHECK(!initialize(&table)&&!memcmp(&saved,&table,sizeof(table)));
    table=(table_type){0x10,0x20,0x10};CHECK(!initialize(&table)&&table.first==table.last&&table.first==table.end);
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,(void *)(uintptr_t)i,0,NULL);CHECK(threads[i]!=NULL);}
    CHECK(WaitForMultipleObjects(4,threads,TRUE,INFINITE)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i){DWORD code=1;CHECK(GetExitCodeThread(threads[i],&code)&&!code);CloseHandle(threads[i]);}
    CHECK(!drain(&table));for(unsigned i=0;i<4;++i)CHECK(counts[i]==2000);
    printf("{\"success\":true,\"checks\":%d,\"threads\":4,\"concurrent_registrations\":8000,\"NULL_callback_allowed\":true,\"NULL_table_returns_minus1\":true,\"preserve_populated\":true,\"reentrant_registration_LIFO\":[2,4,3,1]}\n",checks);return 0;
}
