/* Synthetic CPU/mocked-parent tests only. No original x64 bytes execute. */
#include "recomp_supplemental.h"
#include "supplemental_addresses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include "gc_parent_epilog.h"
int64_t g_image_delta;
static uint64_t stack_words[16],stack_before[16],expected_target;
static unsigned dispatch_calls,cases,checks;
static CPU dispatch_state;
static int import_free_mode;
static int gc_mode;
static unsigned gc_cases;
static unsigned native_free_calls,import_cases;
static unsigned raise_mode,raise_cases,raise_revision,native_raise_calls;
static uint64_t native_raise_hwnd;
static uint32_t mock_last_error;
static void *last_freed;
static void native_free(void *pointer){last_freed=pointer;++native_free_calls;free(pointer);}
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void native_free_adapter(void *opaque,void *pointer){(void)opaque;native_free(pointer);}
static void check(int ok,const char*what){++checks;if(!ok){fprintf(stderr,"SUPPLEMENTAL_FIXTURE_FAIL %s cases%u\n",what,cases);exit(1);}}
/* Typed native boundary mock only: never call Windows SetForegroundWindow. */
static int native_foreground_mock(uint64_t hwnd){native_raise_hwnd=hwnd;++native_raise_calls;errno=raise_revision?EIO:ERANGE;mock_last_error=UINT32_C(0x2468ace0)+raise_revision;return raise_revision?0:1;}
void dispatch(CPU*c,uint64_t target){check(target==expected_target,"parent tail target");check(c->rsp==(uintptr_t)&stack_words[4],"tail JMP must preserve existing frame");dispatch_state=*c;++dispatch_calls;if(gc_mode){L_000141C10188(c);return;}check(stack_words[4]==UINT64_C(0x123456789abcdef0),"tail original return address");if(import_free_mode)native_free((void*)(uintptr_t)c->rcx);if(raise_mode)c->rax=(uint32_t)native_foreground_mock(c->rcx);c->rsp+=8;}
void dispatch_jmp(CPU*c,uint64_t target){dispatch(c,target);}
static CPU initial(unsigned flags){CPU c;memset(&c,0xa5,sizeof(c));c.cf=(flags>>0)&1;c.zf=(flags>>1)&1;c.sf=(flags>>2)&1;c.of=(flags>>3)&1;c.pf=(flags>>4)&1;c.af=(flags>>5)&1;c.mxcsr=0x1f80;
    for(unsigned i=0;i<16;++i)stack_words[i]=UINT64_C(0x13579bdf01234567)+i;stack_words[4]=UINT64_C(0x123456789abcdef0);memcpy(stack_before,stack_words,sizeof(stack_words));c.rsp=(uintptr_t)&stack_words[4];dispatch_calls=0;return c;}
static uint64_t arithmetic(CPU*c,uint64_t a,uint64_t b,unsigned bits,int subtract){uint64_t mask=bits==64?UINT64_MAX:UINT64_C(0xffffffff),sign=UINT64_C(1)<<(bits-1);a&=mask;b&=mask;uint64_t r=(subtract?a-b:a+b)&mask;
    c->cf=subtract?a<b:r<a;c->zf=!r;c->sf=!!(r&sign);c->of=!!(((subtract?(a^b):~(a^b))&(a^r))&sign);c->af=((a^b^r)>>4)&1;unsigned ones=0;for(unsigned i=0;i<8;++i)ones+=(unsigned)(r>>i)&1;c->pf=!(ones&1);return r;}
static void invoke(uint64_t va,CPU*c,CPU expected,unsigned tails){frostpunk_supplemental_fn fn=frostpunk_supplemental_lookup(va);check(fn!=NULL,"known function lookup");fn(c);expected.rip=va;expected.rsp+=8;check(!memcmp(c,&expected,sizeof(*c)),"full CPU including preserved XMM/MXCSR/flags/GPR");check(dispatch_calls==tails,"tail dispatch count");check(!memcmp(stack_before,stack_words,sizeof(stack_words)),"no return-address push/pop corruption");if(tails){expected.rsp-=8;check(!memcmp(&dispatch_state,&expected,sizeof(expected)),"CPU state at mocked parent entry");}++cases;}
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static uint64_t native_gc_link(unsigned char *global,unsigned char *object,unsigned link_offset){
    /* Independent typed list insertion oracle; no original x64 bytes execute. */
    uint64_t old,object_address=(uintptr_t)object;memcpy(&old,global+0x38,8);memcpy(object+link_offset,&old,8);memcpy(global+0x38,&object_address,8);return old;
}
typedef struct {uint64_t guard;uint64_t hwnd;uint64_t tail[2];} raise_driver;
typedef struct {unsigned char prefix[0xc0];raise_driver *driver;} raise_window;
_Static_assert(offsetof(raise_window,driver)==0xc0,"SDL_Window driverdata offset");
_Static_assert(offsetof(raise_driver,hwnd)==8,"SDL_WindowData HWND offset");
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static uint64_t native_raise_chain(const raise_window *window){return window->driver->hwnd;}
int main(void){
    frostpunk_supplemental_fn saved[SUPPLEMENTAL_ADDRESS_COUNT];
    for(unsigned i=0;i<SUPPLEMENTAL_ADDRESS_COUNT;++i){saved[i]=frostpunk_supplemental_lookup(supplemental_addresses[i]);check(saved[i]!=NULL,"all exact entries");check(!frostpunk_supplemental_lookup(supplemental_addresses[i]-1),"preceding address miss");check(!frostpunk_supplemental_lookup(supplemental_addresses[i]+1),"following address miss");}
    check(!frostpunk_supplemental_lookup(0)&&!frostpunk_supplemental_lookup(UINT64_MAX)&&!frostpunk_supplemental_lookup(UINT64_C(0x140e52950)),"outside/parent-only address misses");
    uint32_t rng=323190;for(unsigned n=0;n<100000;++n){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;unsigned i=rng%SUPPLEMENTAL_ADDRESS_COUNT;check(frostpunk_supplemental_lookup(supplemental_addresses[i])==saved[i],"deterministic shuffled lookup");}
    static const uint64_t values[]={0,1,7,8,15,UINT64_MAX,UINT64_MAX-7,UINT64_C(0x7ffffffffffffff8),UINT64_C(0x8000000000000000)};
    for(unsigned flags=0;flags<64;++flags){
        for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i){CPU c=initial(flags);c.rcx=values[i];CPU e=c;e.rcx=arithmetic(&e,c.rcx,8,64,0);expected_target=UINT64_C(0x140e52950);invoke(UINT64_C(0x140de8740),&c,e,1);}
        CPU c=initial(flags),e=c;e.rax=c.rcx+0x140;invoke(UINT64_C(0x140dabe10),&c,e,0);
        c=initial(flags);e=c;e.rdx=2;expected_target=UINT64_C(0x1419c9550);invoke(UINT64_C(0x1419cb930),&c,e,1);
        c=initial(flags);e=c;e.rdx=(c.rdx&~UINT64_C(255))|1;expected_target=UINT64_C(0x141a489b0);invoke(UINT64_C(0x141a47dc0),&c,e,1);
        c=initial(flags);e=c;e.rdx=0;e.cf=e.of=e.sf=0;e.zf=e.pf=1;expected_target=UINT64_C(0x141aefb40);invoke(UINT64_C(0x141aef430),&c,e,1);
        unsigned char object[0x200],wanted[0x200];memset(object,0x5a,sizeof(object));memcpy(wanted,object,sizeof(object));memset(wanted,0,8);c=initial(flags);c.rcx=(uintptr_t)object;e=c;invoke(UINT64_C(0x140dc16b0),&c,e,0);check(!memcmp(object,wanted,sizeof(object)),"RET memory write and adjacent canaries");
        static const uint32_t counts[]={0,1,UINT32_MAX,0x7fffffff,0x80000000};
        for(unsigned decrement=0;decrement<2;++decrement)for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i){memset(object,0x5a,sizeof(object));memcpy(object+0x190,&counts[i],4);memcpy(wanted,object,sizeof(object));c=initial(flags);c.rcx=(uintptr_t)object;e=c;uint64_t oldcf=e.cf;uint32_t result=(uint32_t)arithmetic(&e,counts[i],1,32,(int)decrement);e.cf=oldcf;memcpy(wanted+0x190,&result,4);expected_target=UINT64_C(0x141a84000);invoke(decrement?UINT64_C(0x141a83120):UINT64_C(0x141a83110),&c,e,1);check(!memcmp(object,wanted,sizeof(object)),"INC/DEC DWORD width/CF/canaries");}
    }
    /* Native typed MSVC/AAPCS adapter is an independent ABI oracle. Its free
     * takes the second argument, including NULL, and never reads opaque.
     * The AOT boundary checks full CPU BEFORE import free; volatile machine
     * registers after the native CRT call are intentionally not compared. */
    uint64_t iat_slot=0;
    g_image_delta=(int64_t)(uintptr_t)&iat_slot-(int64_t)UINT64_C(0x141cc7c58);
    frostpunk_supplemental_fn adapter=frostpunk_supplemental_lookup(UINT64_C(0x141c04ec0));check(adapter!=NULL,"observed import-tail lookup");
    for(unsigned flags=0;flags<64;++flags)for(unsigned kind=0;kind<4;++kind)for(unsigned revision=0;revision<2;++revision){
        size_t bytes=kind?kind*31:0;void *oracle=bytes?malloc(bytes):NULL;check(!bytes||oracle!=NULL,"native oracle allocation");
        uintptr_t opaque=(flags&1)?UINTPTR_MAX:0;unsigned old=native_free_calls;
        native_free_adapter((void*)opaque,oracle);check(native_free_calls==old+1&&last_freed==oracle,"native typed second argument/NULL free oracle");
        void *pointer=bytes?malloc(bytes):NULL;check(!bytes||pointer!=NULL,"AOT fixture allocation");CPU c=initial(flags);c.rcx=opaque;c.rdx=(uintptr_t)pointer;CPU e=c;e.rcx=c.rdx;e.rip=UINT64_C(0x141c04ec3);
        iat_slot=UINT64_C(0xe5e20000)+revision*8;expected_target=iat_slot;import_free_mode=1;old=native_free_calls;adapter(&c);import_free_mode=0;
        check(native_free_calls==old+1&&last_freed==pointer,"actual native free receives moved RDX/NULL");check(dispatch_calls==1,"one dynamic import tail dispatch");
        check(!memcmp(&dispatch_state,&e,sizeof(e)),"MOV/import-JMP preserve complete CPU until native callee");e.rsp+=8;check(!memcmp(&c,&e,sizeof(c)),"single common import pop only");
        check(!memcmp(stack_words,stack_before,sizeof(stack_words))&&iat_slot==expected_target,"stack/IAT data unchanged");++import_cases;++cases;
    }
    /* Interior GC switch entries have a partial frame, not a fresh return slot.
     * Compare the exact AOT prefix then execute the unchanged generated parent
     * epilog copied into the prepared header, against independent CPU math. */
    static const uint64_t gc_entries[]={UINT64_C(0x141c10160),UINT64_C(0x141c1016a),UINT64_C(0x141c10177)};
    static const unsigned link_offsets[]={0x30,0xa0,0x68};
    for(unsigned flags=0;flags<64;++flags)for(unsigned entry=0;entry<3;++entry)for(unsigned value=0;value<4;++value)for(unsigned alias=0;alias<2;++alias){
        unsigned char global[0x80],object[0xd0],wanted_global[0x80],wanted_object[0xd0];
        memset(global,0x3a,sizeof(global));memset(object,0xc5,sizeof(object));
        unsigned char *g=alias?object:global;uint64_t old=value==0?0:value==1?UINT64_MAX:value==2?UINT64_C(0x8000000000000000):UINT64_C(0x0123456789abcdef);
        memcpy(g+0x38,&old,8);memcpy(wanted_global,global,sizeof(global));memcpy(wanted_object,object,sizeof(object));
        uint64_t got=native_gc_link(g,object,link_offsets[entry]);check(got==old,"typed native old GC head");
        memcpy(wanted_global,global,sizeof(global));memcpy(wanted_object,object,sizeof(object));
        /* Restore only the inputs changed by the native oracle. */
        memset(global,0x3a,sizeof(global));memset(object,0xc5,sizeof(object));memcpy(g+0x38,&old,8);
        CPU c=initial(flags);c.rdi=(uintptr_t)g;c.rbx=(uintptr_t)object;
        stack_words[4]=UINT64_C(0x142985960);stack_words[8]=UINT64_C(0xdead001122334455);
        stack_words[9]=UINT64_C(0x141c0e3cb);stack_words[10]=UINT64_C(0xbadadabacafef00d);stack_words[11]=UINT64_C(0x12340000ffffffff);
        memcpy(stack_before,stack_words,sizeof(stack_words));CPU boundary=c;boundary.rax=old;boundary.rsi=stack_words[10];boundary.rip=entry==2?gc_entries[entry]:UINT64_C(0x141c1017f);
        CPU expected=boundary;expected.rip=UINT64_C(0x141c10188);expected.rbx=stack_words[11];expected.rsp=arithmetic(&expected,c.rsp,0x20,64,0);expected.rdi=stack_words[8];expected.rsp+=16;
        expected_target=UINT64_C(0x141c10188);gc_mode=1;frostpunk_supplemental_lookup(gc_entries[entry])(&c);gc_mode=0;
        check(dispatch_calls==1,"GC one tail to exact parent epilog");check(!memcmp(&dispatch_state,&boundary,sizeof(boundary)),"GC full CPU at parent epilog including original flags/partialRSP");
        check(!memcmp(&c,&expected,sizeof(c)),"GC full CPU after actual generated parent epilog");
        check(!memcmp(global,wanted_global,sizeof(global))&&!memcmp(object,wanted_object,sizeof(object)),"GC typed native insertion and whole-object canaries/alias");
        check(!memcmp(stack_before,stack_words,sizeof(stack_words)),"GC partial frame/actual return/savedNV slots unchanged");++gc_cases;++cases;
    }
    /* The new leaf reads two typed pointer fields then JMPs to the current IAT
     * value. Compare BEFORE import side effects and after its single common pop. */
    frostpunk_supplemental_fn raise=frostpunk_supplemental_lookup(UINT64_C(0x141b1ef20));check(raise!=NULL,"observed RaiseWindow leaf lookup");
    g_image_delta=(int64_t)(uintptr_t)&iat_slot-(int64_t)UINT64_C(0x141cc7768);
    for(unsigned flags=0;flags<64;++flags)for(unsigned value=0;value<64;++value)for(unsigned revision=0;revision<2;++revision){
        struct {uint64_t before[2];raise_window window;uint64_t after[2];} storage,wanted;
        raise_driver driver,wanted_driver;memset(&storage,0x6a,sizeof(storage));memset(&driver,0xb3,sizeof(driver));
        uint64_t hwnd=value==0?0:value==1?UINT64_MAX:value==2?UINT64_C(0x8000000000000000):
            (UINT64_C(0x9e3779b97f4a7c15)*value)^UINT64_C(0xa5a5ffff12340000);
        driver.hwnd=hwnd;storage.window.driver=&driver;memcpy(&wanted,&storage,sizeof(storage));memcpy(&wanted_driver,&driver,sizeof(driver));
        uint64_t native_hwnd=native_raise_chain(&storage.window);check(native_hwnd==hwnd,"independent typed two-field HWND model");
        raise_revision=revision;unsigned before=native_raise_calls;int result=native_foreground_mock(native_hwnd);
        int expected_errno=errno;uint32_t expected_error=mock_last_error;check(native_raise_calls==before+1&&native_raise_hwnd==hwnd,"typed native HWND preserves all64bits includingNULL");
        CPU c=initial(flags);c.rdx=(uintptr_t)&storage.window;CPU boundary=c;boundary.rcx=native_hwnd;boundary.rip=UINT64_C(0x141b1ef2b);
        iat_slot=UINT64_C(0xe5e21000)+revision*8;expected_target=iat_slot;errno=E2BIG;mock_last_error=UINT32_C(0x1357bace);
        before=native_raise_calls;raise_mode=1;raise(&c);raise_mode=0;
        check(dispatch_calls==1&&native_raise_calls==before+1&&native_raise_hwnd==hwnd,"one dynamic IAT tail to typed HWND import mock");
        check(!memcmp(&dispatch_state,&boundary,sizeof(boundary)),"RaiseWindow full CPU at import RIP/no extra stack push");
        CPU expected=boundary;expected.rax=(uint32_t)result;expected.rsp+=8;
        check(!memcmp(&c,&expected,sizeof(c)),"RaiseWindow callee result and single common pop/full CPU");
        check(errno==expected_errno&&mock_last_error==expected_error,"native mock errno/error side effects preserved");
        check(!memcmp(&storage,&wanted,sizeof(storage))&&!memcmp(&driver,&wanted_driver,sizeof(driver)),"RaiseWindow whole object/field/adjacent guards untouched");
        check(!memcmp(stack_words,stack_before,sizeof(stack_words))&&iat_slot==expected_target,"RaiseWindow stack and dynamic IAT untouched");++raise_cases;++cases;
    }
    printf("{\"success\":true,\"static_entries\":%u,\"lookup_checks\":%u,\"CPU_state_cases\":%u,\"import_tail_cases\":%u,\"GC_jump_cases\":%u,\"raise_window_cases\":%u,\"GC_actual_parent_epilog\":true,\"native_typed_GC_oracle\":true,\"native_typed_free_oracle\":true,\"native_typed_raise_pointer_chain\":true,\"raise_import_boundary_mocked\":true,\"native_SetForegroundWindow_called\":false,\"dynamic_IAT_read\":true,\"NULL_free\":true,\"original_x64_executed\":false,\"parent_dispatch_mocked\":true,\"game_boot_proven\":false}\n",SUPPLEMENTAL_ADDRESS_COUNT,checks,cases,import_cases,gc_cases,raise_cases);return 0;
}
