/* Native Lua 5.1.1 in place of the lifted copy that Frostpunk.exe links statically.
 *
 * The game embeds Lua 5.1.1 ($Lua: Lua 5.1.1 ... $, core 0x141C097A0-0x141C22410) and tolua++.
 * Here the unmodified 5.1.1 sources (reference/lua-5.1.1, MIT) run natively; the 58 C API
 * functions that the engine and tolua++ call (tools/native_libs.py, found in IDA as every function
 * of the core range called from outside it) are strong L_<VA> symbols that take the Win64
 * arguments from the guest registers.  The lifted core loses its dispatch entries and its bodies
 * (tools/native_libs.py).  Checked in research/ida/Frostpunk.exe.i64 on 2026-11-18.
 *
 * Build options of the game's copy, matched here (luaconf.h of 5.1.1 is included unchanged and
 * the differences are redefined right after it):
 *   - lua_Number is float: lmathlib calls sinf/cosf/logf..., lua_pushnumber takes the number in
 *     xmm1 as a float and luaV_tostring prints *(float *) with "%.14g" after the promotion to
 *     double;
 *   - lua_number2int truncates (the x64 MSVC branch of luaconf.h);
 *   - LUAL_BUFFERSIZE is MSVC's BUFSIZ (512);
 *   - luaL_openlibs opens base, table, string, math and debug only (its table at 0x1421C29A0;
 *     io, os and package are not in the image);
 *   - errors use setjmp/longjmp (C, not C++ exceptions).
 * The structures keep their Win64 layout because 5.1.1 uses no `long`: static asserts below pin
 * the offsets that the lifted callers were compiled against.
 *
 * Guest code inside native Lua:
 *   - lua_CFunction values from the guest (lua_pushcclosure, luaL_register) become closures of
 *     one native trampoline with the guest address as an extra, last upvalue; lua_upvalueindex(i)
 *     of the guest function still finds its own upvalues 1..n;
 *   - the allocator of lua_newstate: the engine's (0x140FE9890) is realloc/free of its heap and
 *     is replaced by the native heap; any other one is called as guest code;
 *   - the panic function and debug hooks are called as guest code.
 * A guest call runs on the guest stack of the CPU that entered the outermost API call (thread
 * local).  LUAI_TRY saves that CPU and restores it when an error unwinds with longjmp, so the
 * guest registers and stack pointer are those of the API call that catches it.  The lifted copy
 * could not do this at all: the runtime's longjmp is unsupported (win64_jump.c), so any Lua error
 * stopped the game.
 */
#include "cpu64.h"
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ configuration */
#define LUA_CORE   /* the luai_num* block of luaconf.h; the sources define it again */
#include "luaconf.h"
#undef LUAI_FUNC
#undef LUAI_DATA
#define LUAI_FUNC static
#define LUAI_DATA /* empty */
#undef LUA_NUMBER_DOUBLE
#undef LUA_NUMBER
#define LUA_NUMBER float
#undef LUAI_UACNUMBER
#define LUAI_UACNUMBER double
#undef LUA_NUMBER_SCAN
#define LUA_NUMBER_SCAN "%f"
#undef LUA_NUMBER_FMT
#define LUA_NUMBER_FMT "%.14g"
#undef lua_number2str
#define lua_number2str(s, n) sprintf((s), LUA_NUMBER_FMT, (double)(n))
#undef lua_str2number
#define lua_str2number(s, p) ((lua_Number)strtod((s), (p)))
#undef lua_number2int
#undef lua_number2integer
#define lua_number2int(i, d) ((i) = (int)(d))
#define lua_number2integer(i, d) ((i) = (lua_Integer)(d))
#undef LUAL_BUFFERSIZE
#define LUAL_BUFFERSIZE 512

struct fp_lua_guard { CPU *cpu; CPU saved; };
static void fp_lua_guard_enter(struct fp_lua_guard *g);
static void fp_lua_guard_restore(struct fp_lua_guard *g);
#undef LUAI_THROW
#undef LUAI_TRY
#undef luai_jmpbuf
#define LUAI_THROW(L, c) _longjmp((c)->b, 1)
#define LUAI_TRY(L, c, a) { struct fp_lua_guard fp_guard_; fp_lua_guard_enter(&fp_guard_); \
    if (_setjmp((c)->b) == 0) { a } else fp_lua_guard_restore(&fp_guard_); }
#define luai_jmpbuf jmp_buf

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "lapi.c"
#include "lcode.c"
#include "ldebug.c"
#include "ldo.c"
#include "ldump.c"
#include "lfunc.c"
#include "lgc.c"
#include "llex.c"
#include "lmem.c"
#include "lobject.c"
#include "lopcodes.c"
#include "lparser.c"
#include "lstate.c"
#include "lstring.c"
#include "ltable.c"
#include "ltm.c"
#include "lundump.c"
#include "lvm.c"
#include "lzio.c"
#include "lauxlib.c"
#include "lbaselib.c"
#include "ldblib.c"
#include "lmathlib.c"
#include "lstrlib.c"
#include "ltablib.c"
#pragma clang diagnostic pop

/* The offsets the lifted engine and tolua++ were compiled against (MSVC x64). */
_Static_assert(sizeof(lua_Number) == 4, "float lua_Number");
_Static_assert(sizeof(TValue) == 16, "TValue");
_Static_assert(offsetof(lua_State, top) == 16 && offsetof(lua_State, base) == 24 && offsetof(lua_State, l_G) == 32, "lua_State head");
_Static_assert(offsetof(lua_State, ci) == 40 && offsetof(lua_State, stack) == 64 && offsetof(lua_State, base_ci) == 80, "lua_State stack");
_Static_assert(offsetof(lua_State, nCcalls) == 96 && offsetof(lua_State, hookmask) == 98 && offsetof(lua_State, basehookcount) == 100, "lua_State hooks");
_Static_assert(offsetof(lua_State, hookcount) == 104 && offsetof(lua_State, hook) == 112 && offsetof(lua_State, l_gt) == 120, "lua_State hook/gt");
_Static_assert(sizeof(lua_State) == 184, "lua_State size");
_Static_assert(sizeof(LG) == 616, "lua_newstate block");
_Static_assert(offsetof(global_State, frealloc) == 16 && offsetof(global_State, GCthreshold) == 112, "global_State alloc/threshold");
_Static_assert(offsetof(global_State, totalbytes) == 120 && offsetof(global_State, gcpause) == 144, "global_State gc");
_Static_assert(offsetof(global_State, gcstepmul) == 148 && offsetof(global_State, panic) == 152, "global_State panic");
_Static_assert(offsetof(global_State, mt) == 224, "global_State mt");
_Static_assert(sizeof(lua_Debug) == 120 && offsetof(lua_Debug, i_ci) == 116, "lua_Debug");
_Static_assert(sizeof(Udata) == 40 && sizeof(CallInfo) == 40, "Udata/CallInfo");

/* ------------------------------------------------------------------ guest bridge */
#define FP_LUA_RETURN UINT64_C(0xE5EE0300)     /* fake return address of guest calls */
#define FP_LUA_ENGINE_ALLOC UINT64_C(0x140FE9890)

static _Thread_local CPU *fp_lua_cpu;
static void fp_lua_stop(const char *why) {
    fprintf(stderr, "STOP native Lua: %s\n", why);
    fflush(stderr);
    _Exit(73);
}

static void fp_lua_guard_enter(struct fp_lua_guard *g) {
    g->cpu = fp_lua_cpu;
    if (g->cpu) memcpy(&g->saved, g->cpu, sizeof(CPU));
}
static void fp_lua_guard_restore(struct fp_lua_guard *g) {
    fp_lua_cpu = g->cpu;
    if (g->cpu) memcpy(g->cpu, &g->saved, sizeof(CPU));
}

/* Win64 call of guest code with up to four integer arguments, below the current guest frame. */
static uint64_t fp_guest_call(uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    CPU *c = fp_lua_cpu;
    if (!c) fp_lua_stop("guest call outside an API call");
    uint64_t rsp0 = c->rsp;
    uint64_t sp = ((rsp0 - 128) & ~UINT64_C(15)) - 32;   /* 16-aligned, with the shadow space */
    c->rsp = sp;
    c->rcx = a0; c->rdx = a1; c->r8 = a2; c->r9 = a3;
    push64(c, FP_LUA_RETURN);
    dispatch(c, fn);
    if (c->rsp != sp) fp_lua_stop("guest call stack imbalance");
    uint64_t result = c->rax;
    c->rsp = rsp0;
    fp_lua_cpu = c;
    return result;
}

/* Diagnostics: API and guest-function call counts at powers of two from 1024, and every failed
 * lua_pcall with its message (the first 64), so a behaviour change of the native copy shows. */
static _Atomic uint64_t fp_lua_api_calls, fp_lua_guest_calls;
static _Atomic unsigned fp_lua_errors;
static void fp_lua_count(_Atomic uint64_t *counter) {
    uint64_t n = ++*counter;
    if (n >= 1024 && !(n & (n - 1)))
        fprintf(stderr, "LUA_NATIVE calls api=%llu guest_functions=%llu pcall_errors=%u\n",
                (unsigned long long)fp_lua_api_calls, (unsigned long long)fp_lua_guest_calls, (unsigned)fp_lua_errors);
}

/* Guest lua_CFunction: the trampoline's last upvalue is the guest address. */
static int fp_lua_guest_cfunction(lua_State *L) {
    fp_lua_count(&fp_lua_guest_calls);
    Closure *cl = clvalue(L->base - 1);
    uint64_t fn = (uint64_t)(uintptr_t)pvalue(&cl->c.upvalue[cl->c.nupvalues - 1]);
    return (int)(int32_t)(uint32_t)fp_guest_call(fn, (uint64_t)(uintptr_t)L, 0, 0, 0);
}
static int fp_lua_is_guest(lua_State *L, int idx, uint64_t *fn) {
    StkId o = index2adr(L, idx);
    if (!iscfunction(o) || clvalue(o)->c.f != fp_lua_guest_cfunction) return 0;
    if (fn) *fn = (uint64_t)(uintptr_t)pvalue(&clvalue(o)->c.upvalue[clvalue(o)->c.nupvalues - 1]);
    return 1;
}
static void fp_lua_push_guest_closure(lua_State *L, uint64_t fn, int n) {
    lua_pushlightuserdata(L, (void *)(uintptr_t)fn);
    lua_pushcclosure(L, fp_lua_guest_cfunction, n + 1);
}

/* Allocators: the engine's realloc/free of its heap becomes the native heap. */
static void *fp_lua_native_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) { free(ptr); return NULL; }
    return realloc(ptr, nsize);
}
struct fp_lua_guest_alloc { uint64_t fn, ud; };
static void *fp_lua_guest_allocator(void *ud, void *ptr, size_t osize, size_t nsize) {
    const struct fp_lua_guest_alloc *a = ud;
    return (void *)(uintptr_t)fp_guest_call(a->fn, a->ud, (uint64_t)(uintptr_t)ptr, osize, nsize);
}

/* Panic functions and hooks are guest code, one per state (the game has one). */
#define FP_LUA_STATES 8
static struct { global_State *g; uint64_t panic; } fp_lua_panics[FP_LUA_STATES];
static struct { lua_State *L; uint64_t hook; } fp_lua_hooks[64];
static int fp_lua_panic(lua_State *L) {
    for (unsigned i = 0; i < FP_LUA_STATES; ++i)
        if (fp_lua_panics[i].g == G(L) && fp_lua_panics[i].panic)
            return (int)(int32_t)(uint32_t)fp_guest_call(fp_lua_panics[i].panic, (uint64_t)(uintptr_t)L, 0, 0, 0);
    return 0;
}
static void fp_lua_hook(lua_State *L, lua_Debug *ar) {
    for (unsigned i = 0; i < sizeof(fp_lua_hooks) / sizeof(*fp_lua_hooks); ++i)
        if (fp_lua_hooks[i].L == L && fp_lua_hooks[i].hook) {
            fp_guest_call(fp_lua_hooks[i].hook, (uint64_t)(uintptr_t)L, (uint64_t)(uintptr_t)ar, 0, 0);
            return;
        }
}

/* lua_pushvfstring with guest Win64 variadic arguments: slot j is r8, r9, then the stack. */
static uint64_t fp_lua_vararg(const CPU *c, unsigned first, unsigned j) {
    unsigned slot = first + j;   /* argument position, 0-based */
    if (slot == 0) return c->rcx;
    if (slot == 1) return c->rdx;
    if (slot == 2) return c->r8;
    if (slot == 3) return c->r9;
    return rd64(c->rsp + 8 + 8 * slot);
}
static const char *fp_lua_pushgfstring(lua_State *L, const char *fmt, const CPU *c, unsigned first) {
    int n = 1;
    unsigned j = 0;
    pushstr(L, "");
    for (;;) {
        const char *e = strchr(fmt, '%');
        if (e == NULL) break;
        setsvalue2s(L, L->top, luaS_newlstr(L, fmt, e - fmt));
        incr_top(L);
        switch (*(e + 1)) {
        case 's': { const char *s = (const char *)(uintptr_t)fp_lua_vararg(c, first, j++); pushstr(L, s ? s : "(null)"); break; }
        case 'c': { char buff[2]; buff[0] = (char)fp_lua_vararg(c, first, j++); buff[1] = '\0'; pushstr(L, buff); break; }
        case 'd': setnvalue(L->top, cast_num((int)(uint32_t)fp_lua_vararg(c, first, j++))); incr_top(L); break;
        case 'f': { uint64_t bits = fp_lua_vararg(c, first, j++); double d; memcpy(&d, &bits, 8); setnvalue(L->top, cast_num(d)); incr_top(L); break; }
        case 'p': { char buff[4 * sizeof(void *) + 8]; sprintf(buff, "%p", (void *)(uintptr_t)fp_lua_vararg(c, first, j++)); pushstr(L, buff); break; }
        case '%': pushstr(L, "%"); break;
        default: { char buff[3]; buff[0] = '%'; buff[1] = *(e + 1); buff[2] = '\0'; pushstr(L, buff); break; }
        }
        n += 2;
        fmt = e + 2;
    }
    pushstr(L, fmt);
    luaV_concat(L, n + 1, cast_int(L->top - L->base) - 1);
    L->top -= n;
    return svalue(L->top - 1);
}

/* luaI_openlib of lauxlib.c with a guest luaL_Reg array {char *name; guest lua_CFunction}. */
static void fp_lua_register_guest(lua_State *L, const char *libname, uint64_t reg, int nup) {
    if (libname) {
        int size = 0;
        for (uint64_t r = reg; rd64(r); r += 16) size++;
        luaL_findtable(L, LUA_REGISTRYINDEX, "_LOADED", size);
        lua_getfield(L, -1, libname);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            if (luaL_findtable(L, LUA_GLOBALSINDEX, libname, size) != NULL)
                luaL_error(L, "name conflict for module " LUA_QS, libname);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, libname);
        }
        lua_remove(L, -2);
        lua_insert(L, -(nup + 1));
    }
    for (uint64_t r = reg; rd64(r); r += 16) {
        for (int i = 0; i < nup; i++) lua_pushvalue(L, -nup);
        fp_lua_push_guest_closure(L, rd64(r + 8), nup);
        lua_setfield(L, -(nup + 2), (const char *)(uintptr_t)rd64(r));
    }
    lua_pop(L, nup);
}

static void fp_lua_openlibs(lua_State *L) {
    static const luaL_Reg libs[] = {{"", luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                                    {LUA_MATHLIBNAME, luaopen_math}, {LUA_DBLIBNAME, luaopen_debug}, {NULL, NULL}};
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        lua_pushcfunction(L, lib->func);
        lua_pushstring(L, lib->name);
        lua_call(L, 1, 0);
    }
}

/* ------------------------------------------------------------------ the C API entries */
#define LS ((lua_State *)(uintptr_t)c->rcx)
#define A2 (c->rdx)
#define A3 (c->r8)
#define A4 (c->r9)
#define I2 ((int)(int32_t)(uint32_t)c->rdx)
#define I3 ((int)(int32_t)(uint32_t)c->r8)
#define I4 ((int)(int32_t)(uint32_t)c->r9)
#define P2 ((const char *)(uintptr_t)c->rdx)
#define P3 ((const char *)(uintptr_t)c->r8)
#define ENTER CPU *fp_prev = fp_lua_cpu; fp_lua_cpu = c; fp_lua_count(&fp_lua_api_calls)
#define LEAVE fp_lua_cpu = fp_prev; c->rip = pop64(c)
#define RET(v) do { c->rax = (uint64_t)(v); } while (0)
#define RETI(v) do { c->rax = (uint64_t)(uint32_t)(int)(v); } while (0)
#define RETP(v) do { c->rax = (uint64_t)(uintptr_t)(v); } while (0)
#define RETF(v) do { c->xmm[0].u64[0] = 0; c->xmm[0].u64[1] = 0; c->xmm[0].f32[0] = (float)(v); } while (0)
#define API(va, body) void L_##va(CPU *c) { ENTER; body; LEAVE; }

API(000141C09A40, lua_close(LS))
API(000141C09AC0, {
    lua_State *L;
    if (c->rcx == FP_LUA_ENGINE_ALLOC) L = lua_newstate(fp_lua_native_alloc, NULL);
    else {
        struct fp_lua_guest_alloc *a = malloc(sizeof(*a));   /* lives as long as the process */
        if (!a) fp_lua_stop("allocator record");
        a->fn = c->rcx; a->ud = c->rdx;
        L = lua_newstate(fp_lua_guest_allocator, a);
    }
    if (L) {
        for (unsigned i = 0; i < FP_LUA_STATES; ++i)
            if (!fp_lua_panics[i].g) { fp_lua_panics[i].g = G(L); fp_lua_panics[i].panic = 0; break; }
        fprintf(stderr, "LUA_NATIVE state=%p allocator=%s lua_Number=float\n", (void *)L,
                c->rcx == FP_LUA_ENGINE_ALLOC ? "native" : "guest");
    }
    RETP(L);
})
API(000141C09E10, {
    uint64_t previous = 0;
    unsigned i;
    for (i = 0; i < FP_LUA_STATES; ++i) if (fp_lua_panics[i].g == G(LS)) break;
    if (i == FP_LUA_STATES) fp_lua_stop("lua_atpanic on an unknown state");
    previous = fp_lua_panics[i].panic;
    fp_lua_panics[i].panic = A2;
    lua_atpanic(LS, A2 ? fp_lua_panic : NULL);
    RET(previous);
})
API(000141C09E30, lua_call(LS, I2, I3))
API(000141C09F00, lua_concat(LS, I2))
API(000141C09F90, lua_createtable(LS, I2, I3))
API(000141C0A050, lua_error(LS))
API(000141C0A060, RETI(lua_gc(LS, I2, I3)))
API(000141C0A1F0, lua_getfenv(LS, I2))
API(000141C0A270, lua_getfield(LS, I2, P3))
API(000141C0A2E0, RETI(lua_getmetatable(LS, I2)))
API(000141C0A340, lua_gettable(LS, I2))
API(000141C0A370, RETI(lua_gettop(LS)))
API(000141C0A420, lua_insert(LS, I2))
API(000141C0A470, RETI(lua_iscfunction(LS, I2)))
API(000141C0A4A0, RETI(lua_isnumber(LS, I2)))
API(000141C0A4D0, RETI(lua_isstring(LS, I2)))
API(000141C0A510, RETI(lua_isuserdata(LS, I2)))
API(000141C0A640, RETP(lua_newuserdata(LS, (size_t)A2)))
API(000141C0A6B0, RETI(lua_next(LS, I2)))
API(000141C0A790, {
    lua_State *L = LS;
    int status = lua_pcall(L, I2, I3, I4);
    if (status && ++fp_lua_errors <= 64) {
        const char *message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)";
        fprintf(stderr, "LUA_NATIVE pcall status=%d: %s\n", status, message);
    }
    RETI(status);
})
API(000141C0A820, lua_pushboolean(LS, I2))
API(000141C0A840, fp_lua_push_guest_closure(LS, A2, I3))
API(000141C0A960, lua_pushinteger(LS, (lua_Integer)(int64_t)A2))
API(000141C0A980, lua_pushlightuserdata(LS, (void *)(uintptr_t)A2))
API(000141C0A9A0, lua_pushstring(LS, P2))   /* an LTCG copy of lua_pushstring without the NULL test */
API(000141C0AA10, lua_pushnil(LS))
API(000141C0AA30, lua_pushnumber(LS, c->xmm[1].f32[0]))
API(000141C0AA50, lua_pushstring(LS, P2))
API(000141C0AB20, lua_pushvalue(LS, I2))
API(000141C0ABA0, RETI(lua_rawequal(LS, I2, I3)))
API(000141C0AC10, lua_rawget(LS, I2))
API(000141C0AC90, lua_rawset(LS, I2))
API(000141C0ADB0, lua_remove(LS, I2))
API(000141C0AE00, lua_replace(LS, I2))
API(000141C0AED0, RETI(lua_setfenv(LS, I2)))
API(000141C0AF70, lua_setfield(LS, I2, P3))
API(000141C0AFF0, RETI(lua_setmetatable(LS, I2)))
API(000141C0B0B0, lua_settable(LS, I2))
API(000141C0B0E0, lua_settop(LS, I2))
API(000141C0B230, RETI(lua_toboolean(LS, I2)))
API(000141C0B260, {
    uint64_t fn = 0;
    if (fp_lua_is_guest(LS, I2, &fn)) RET(fn);
    else {
        lua_CFunction f = lua_tocfunction(LS, I2);
        if (f) fprintf(stderr, "LUA_NATIVE lua_tocfunction of a native library function %p\n", (void *)f);
        RETP(f);
    }
})
API(000141C0B2C0, {
    size_t len = 0;
    const char *s = lua_tolstring(LS, I2, &len);
    if (A3) wr64(A3, len);
    RETP(s);
})
API(000141C0B360, RETF(lua_tonumber(LS, I2)))
API(000141C0B440, RETP(lua_touserdata(LS, I2)))
API(000141C0B480, RETI(lua_type(LS, I2)))
API(000141C0B4B0, RETP(lua_typename(LS, I2)))
API(000141C0BF80, RETI(lua_getinfo(LS, P2, (lua_Debug *)(uintptr_t)A3)))
API(000141C0C110, RETP(lua_getlocal(LS, (const lua_Debug *)(uintptr_t)A2, I3)))
API(000141C0C180, RETI(lua_getstack(LS, I2, (lua_Debug *)(uintptr_t)A3)))
API(000141C0C200, {
    lua_State *L = LS;
    unsigned i;
    unsigned free_slot = ~0u;
    for (i = 0; i < sizeof(fp_lua_hooks) / sizeof(*fp_lua_hooks); ++i) {
        if (fp_lua_hooks[i].L == L) break;
        if (!fp_lua_hooks[i].L && free_slot == ~0u) free_slot = i;
    }
    if (i == sizeof(fp_lua_hooks) / sizeof(*fp_lua_hooks)) {
        if (free_slot == ~0u) fp_lua_stop("too many hooked Lua threads");
        i = free_slot;
    }
    fp_lua_hooks[i].L = A2 ? L : NULL;
    fp_lua_hooks[i].hook = A2;
    RETI(lua_sethook(L, A2 ? fp_lua_hook : NULL, I3, I4));
})
API(000141C0C760, fp_lua_openlibs(LS))
API(000141C0CCD0, RETF(luaL_checknumber(LS, I2)))
API(000141C0CE70, {
    lua_State *L = LS;
    luaL_where(L, 1);
    fp_lua_pushgfstring(L, P2, c, 2);
    lua_concat(L, 2);
    lua_error(L);
})
API(000141C0D0F0, RETI(luaL_loadbuffer(LS, P2, (size_t)A3, (const char *)(uintptr_t)A4)))
API(000141C0D120, RETI(luaL_loadstring(LS, P2)))
API(000141C0D160, RETI(luaL_newmetatable(LS, P2)))
API(000141C0D390, fp_lua_register_guest(LS, P2, A3, 0))
