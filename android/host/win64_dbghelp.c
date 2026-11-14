/* The original engine tolerates a failed optional symbol handler. Android AOT
 * has no DbgHelp/PDB session, so initialization fails explicitly with error50.
 * shortcut: no PDB symbol provider; implement one if gameplay requires symbols. */
#include "win64_dbghelp.h"
#include "win64.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_flag unavailable_logged=ATOMIC_FLAG_INIT;
static void unavailable(CPU *cpu,int announce) {
    if(announce&&!atomic_flag_test_and_set(&unavailable_logged))
        fputs("DBGHELP_UNAVAILABLE symbol_handler=0 native_AOT=1 error=50\n",stderr);
    win64_set_error(50);cpu->rax=0;
}
int win64_dbghelp_call(CPU *cpu,const char *name) {
    if(!strcmp(name,"SymCleanup")){
        /* Every handle is uninitialized because SymInitialize never succeeds.
         * Native Windows returns FALSE/ERROR_INVALID_HANDLE in this state. */
        win64_set_error(6);cpu->rax=0;return 1;
    }
    if(!strcmp(name,"SymInitialize")){
        if(cpu->rdx)(void)win64_string_a(cpu->rdx);
        unavailable(cpu,1);return 1;
    }
    return 0;
}
void win64_dbghelp_self_test(void) {
    uint32_t saved_error=win64_get_error();CPU cpu,before;
    memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=UINT64_MAX;before=cpu;
    if(!win64_dbghelp_call(&cpu,"SymCleanup")||cpu.rax||win64_get_error()!=6)abort();
    cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();
    unavailable(&cpu,0);
    if(cpu.rax||win64_get_error()!=50)abort();
    cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();
    if(win64_dbghelp_call(&cpu,"SymFromAddr")||memcmp(&cpu,&before,sizeof(cpu)))abort();
    win64_set_error(saved_error);
    fputs("WIN64_DBGHELP_SELF_TEST no_symbol_session=1 cleanup_uninitialized_error6=1 initialization_unavailable_error50=1 no_rsp_pop=1 passed\n",stderr);
}
