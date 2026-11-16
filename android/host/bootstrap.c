/* Native bootstrap for statically translated Win64 PE and explicit HLE.
 * Original PE bytes are data only: RW and verified NX. Only generated C is
 * called. Unsupported imports, CPUID, TEB reads and instruction sites stop.
 * Build with native/runtime64 and generated/startup on the include path.
 */
#define _GNU_SOURCE
#include "cpu64.h"
#include "aot_cpu.h"
#include "extra_aot.h"
#if defined(FROSTPUNK_HAVE_TEXTURE_FORMATS)
#include "texture_formats.h"
#endif
#include "recomp_dispatch.h"
#include "win64_environment.h"
#include "win64_openal.h"
#include "win64_shell.h"
#if defined(FROSTPUNK_HAVE_SUPPLEMENTAL)
#include "recomp_supplemental.h"
#include "shader_lookup_aot.h"
#include "vorbis_native.h"
#if defined(FROSTPUNK_HAVE_STRING_RESIZE)
#include "string_resize_aot.h"
#endif
#endif
#include "win64.h"
#include "zip_files.h"
#include "win64_math.h"
#include "win64_rtti.h"
#include "win64_kernel.h"
#include "dependencies.h"
#include "process.h"
#include "../dxvk/startup_gpu.h"
#include "../steam/startup_services.h"
#include "win64_newsfeed.h"
#include "perf_counters.h"
#if defined(FROSTPUNK_HAVE_PAYLOAD)
#include "payload.h"
#endif
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

#define ORIGINAL_BASE UINT64_C(0x140000000)
#define ORIGINAL_ENTRY UINT64_C(0x144506310)
#define MAX_IMAGE (256u * 1024u * 1024u)
#define STACK_BYTES (2u * 1024u * 1024u)
#define TEB_BYTES 0x2000u

int64_t g_image_delta;
extern uint64_t es3_eh_image_base;
extern void frostpunk_call_next_test(CPU *c);
/* One thread-local block: a dlopen'ed library reaches TLS through the dynamic TLSDESC
 * resolver, so the TEB bytes and their known-map share one resolve per guest gs: access. */
static _Thread_local struct { unsigned char teb[TEB_BYTES], known[TEB_BYTES]; } teb_state __attribute__((aligned(16)));
#define guest_teb (teb_state.teb)
#define teb_known (teb_state.known)
static _Thread_local uint64_t stack_lo, stack_hi;
static _Atomic uint64_t dispatch_calls;
static uint64_t dispatch_budget=1000000;
static int trace_calls=1;
static _Thread_local uint64_t static_tls_vector[64];
static unsigned char guest_peb[0x1000], process_parameters[0x400];

static _Noreturn void fail(int code, const char *format, ...) {
    va_list ap;
    fputs("STOP ", stderr);
    va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
    /* Live guest workers cannot use C++ globals during host static teardown. */
    fputc('\n', stderr); fflush(stderr); _Exit(code);
}
#define stop(...) fail(73, __VA_ARGS__)

void frostpunk_unsupported(uint64_t va, const char *text) {
    fail(72, "unsupported instruction va=0x%" PRIx64 " detail=%s", va, text);
}

void frostpunk_cpuid(CPU *c) {
    uint32_t leaf=(uint32_t)c->rax;
    c->rax=c->rbx=c->rcx=c->rdx=0;
    /* This is the translated guest ISA contract, not the physical ARM CPU. */
    if (leaf==0) { c->rax=1; c->rbx=0x756e6547; c->rdx=0x49656e69; c->rcx=0x6c65746e; }
    else if (leaf==1) { c->rax=0x206a7; c->rdx=(1u<<15)|(1u<<25)|(1u<<26); }
    else if (leaf==0x80000000) c->rax=0x80000001;
    else if (leaf==0x80000001) c->rdx=(1u<<20)|(1u<<29);
}

static int fits(uint64_t offset, uint64_t bytes, uint64_t limit) {
    return offset <= limit && bytes <= limit - offset;
}

static int teb_accessible(uint64_t off, unsigned bytes) {
    return (bytes == 4 || bytes == 8) && fits(off, bytes, TEB_BYTES);
}

uint64_t frostpunk_teb_read(uint64_t off, unsigned bytes) {
    uint64_t value = 0;
    /* Hot path (gs:[0x58] TLS array, gs:[0x30] self): every byte known, one wide check. */
    if (__builtin_expect(bytes == 8 && off <= TEB_BYTES - 8, 1)) {
        const unsigned char *t = (const unsigned char *)&teb_state;
        uint64_t known; memcpy(&known, t + TEB_BYTES + off, 8);
        if (__builtin_expect(known == UINT64_C(0x0101010101010101), 1)) { memcpy(&value, t + off, 8); return value; }
    } else if (bytes == 4 && off <= TEB_BYTES - 4) {
        const unsigned char *t = (const unsigned char *)&teb_state;
        uint32_t known; memcpy(&known, t + TEB_BYTES + off, 4);
        if (known == UINT32_C(0x01010101)) { memcpy(&value, t + off, 4); return value; }
    }
    if (!teb_accessible(off, bytes)) stop("TEB read bounds offset=0x%" PRIx64 " size=%u", off, bytes);
    for (unsigned i = 0; i < bytes; ++i)
        if (!teb_known[off + i]) stop("TEB read unimplemented offset=0x%" PRIx64 " size=%u", off, bytes);
    memcpy(&value, guest_teb + off, bytes);
    return value;
}

void frostpunk_teb_write(uint64_t off, uint64_t value, unsigned bytes) {
    if (!teb_accessible(off, bytes)) stop("TEB write bounds offset=0x%" PRIx64 " size=%u", off, bytes);
    memcpy(guest_teb + off, &value, bytes);
    memset(teb_known + off, 1, bytes);
}

/* The host stack is the shadow call stack: a longjmp to a catching frame
 * unwinds the native dispatch frames with it, so no separate depth counter is
 * kept on the dispatch hot path (it cost a dynamic TLS lookup per call). */
int es3_cs_depth_get(void) { return 0; }
void es3_cs_depth_set(int depth) {
    if (depth < 0) stop("invalid EH dispatch depth=%d", depth);
}

static uint64_t return_pc(const CPU *c) {
    return c->rsp >= stack_lo && fits(c->rsp, 8, stack_hi) ? rd64(c->rsp) : 0;
}

/* Isolated candidate: immutable after check_dispatch_table, before guest threads.
 * No runtime allocator, lock or thread-local access on a main-table hit. */
#define MAIN_BUCKET_BASE UINT64_C(0x140000000)
#define MAIN_BUCKET_BYTES UINT64_C(0x4539000)
#define MAIN_BUCKET_SHIFT 6u
#define MAIN_BUCKET_COUNT ((MAIN_BUCKET_BYTES+((UINT64_C(1)<<MAIN_BUCKET_SHIFT)-1u))>>MAIN_BUCKET_SHIFT)
static uint32_t main_bucket_index[MAIN_BUCKET_COUNT+1];
static int main_bucket_ready;
static void main_bucket_init(void){
    if(main_bucket_ready)return;
    uint32_t cursor=0;
    for(size_t bucket=0;bucket<=MAIN_BUCKET_COUNT;++bucket){
        uint64_t limit=MAIN_BUCKET_BASE+((uint64_t)bucket<<MAIN_BUCKET_SHIFT);
        while(cursor<recomp_dispatch_count&&recomp_dispatch_table[cursor].va<limit)++cursor;
        main_bucket_index[bucket]=cursor;
    }
    if(cursor!=recomp_dispatch_count)stop("main lookup entry outside bucket namespace");
    main_bucket_ready=1;
}

#define LOOKUP_CACHE_SLOTS 32768u
/* Shared with the fastlift units (fl_icall in fastlift.h reads it inline): one atomic word per
 * entry, only immutable main-table functions that the dispatcher does not treat specially. */
__attribute__((visibility("hidden"))) _Atomic uint64_t fp_lookup_cache[LOOKUP_CACHE_SLOTS];
__attribute__((visibility("hidden"))) void fp_lookup_anchor(void) {}
#define lookup_cache fp_lookup_cache
static intptr_t lookup_anchor(void) { return (intptr_t)(uintptr_t)&fp_lookup_anchor; }
/* Fast dispatch for AOT calls with a producer-verified linked canonical function. The full
 * resolver keeps every special target (adapters, ring fences, replacements, the WinMain note);
 * this 1024-bit filter only decides whether a target may be one of them. */
static uint64_t dispatch_special_filter[16];
static int dispatch_fast_ready;
static unsigned dispatch_special_hash(uint64_t target) { return (unsigned)((target>>2)^(target>>12))&1023u; }
static const uint64_t dispatch_specials[]={UINT64_C(0x140e222e0),UINT64_C(0x140d95650),UINT64_C(0x140f499a0),
    UINT64_C(0x1410596a9),UINT64_C(0x1410597e9),UINT64_C(0x141250610),UINT64_C(0x14124c7b0),
    UINT64_C(0x140ec6200),UINT64_C(0x140ec7240),UINT64_C(0x140e74b30),UINT64_C(0x140e26e40),UINT64_C(0x140e26f30),
    UINT64_C(0x140e26f90),UINT64_C(0x140f99f00),UINT64_C(0x140f9d240),UINT64_C(0x140e02420),
    UINT64_C(0x141c5c490),UINT64_C(0x141c54e60),UINT64_C(0x141c54af0),UINT64_C(0x141c551d0)};
static void dispatch_fast_init(void) {
    for(size_t i=0;i<sizeof(dispatch_specials)/sizeof(*dispatch_specials);++i){
        unsigned h=dispatch_special_hash(dispatch_specials[i]);dispatch_special_filter[h>>6]|=UINT64_C(1)<<(h&63u);
    }
}
/* Exact, order-independent (lifted code runs before dispatch_fast_init): never cache these. */
static int dispatch_is_special(uint64_t target) {
    for(size_t i=0;i<sizeof(dispatch_specials)/sizeof(*dispatch_specials);++i)if(dispatch_specials[i]==target)return 1;
    return 0;
}
/* Only immutable main/supplemental AOT entries are cached. Exact main
 * namespace hits may skip disjoint provider declines; the entire
 * OpenAL envelope retains its existing provider precedence. */
static void (*main_static_lookup(uint64_t target))(CPU *) {
#if defined(FROSTPUNK_HAVE_STRING_RESIZE)
    if(target==UINT64_C(0x140e222e0)) {
        void (*resize_function)(CPU *)=fp_android_string_resize_lookup(target);
        if(resize_function)return resize_function;
    }
#endif
#if defined(FROSTPUNK_HAVE_TEXTURE_FORMATS)
    if(target==UINT64_C(0x140d95650)||target==UINT64_C(0x140f499a0)) {
        void (*texture_function)(CPU *)=fp_android_texture_formats_lookup(target);
        if(texture_function)return texture_function;
    }
#endif
#if defined(FROSTPUNK_HAVE_SUPPLEMENTAL)
    /* Exact static equivalents of the two profiled original scan fragments. */
    if(target==UINT64_C(0x1410596a9))return frostpunk_shader_lookup_1410596a9;
    if(target==UINT64_C(0x1410597e9))return frostpunk_shader_lookup_1410597e9;
    /* libvorbis mdct_backward, vorbis_book_decodev_set, vorbis_book_decodevv_add: native,
     * self-checked against the translated originals. */
    if(target==UINT64_C(0x141c5c490)||target==UINT64_C(0x141c54e60)||target==UINT64_C(0x141c54af0)||target==UINT64_C(0x141c551d0)) {
        void (*vorbis_function)(CPU *)=fp_vorbis_native_lookup(target);
        if(vorbis_function)return vorbis_function;
    }
#endif
    void (*function)(CPU *)=NULL;
    uint32_t lo=0,hi=recomp_dispatch_count;
    unsigned cache_slot=0;uint64_t cache_tag=0;
    if(main_bucket_ready&&target>=MAIN_BUCKET_BASE&&target-MAIN_BUCKET_BASE<MAIN_BUCKET_BYTES){
        /* Immutable main-table hits only: entry = (target offset + 1) << 32 | function offset,
         * one atomic word, so a racing reader sees an empty slot or a complete pair. */
        cache_tag=(target-MAIN_BUCKET_BASE+1)<<32;
        cache_slot=(unsigned)((target>>2)^(target>>14))&(LOOKUP_CACHE_SLOTS-1u);
        uint64_t entry=atomic_load_explicit(&lookup_cache[cache_slot],memory_order_relaxed);
        if((entry&~UINT64_C(0xffffffff))==cache_tag)
            return (void (*)(CPU *))(uintptr_t)(lookup_anchor()+(intptr_t)(int32_t)(uint32_t)entry);
        size_t bucket=(size_t)((target-MAIN_BUCKET_BASE)>>MAIN_BUCKET_SHIFT);
        lo=main_bucket_index[bucket];hi=main_bucket_index[bucket+1];
    }
    while(lo<hi){
        uint32_t mid=lo+(hi-lo)/2;
        if(recomp_dispatch_table[mid].va<target)lo=mid+1;else hi=mid;
    }
    if(lo<recomp_dispatch_count&&recomp_dispatch_table[lo].va==target)
        function=recomp_dispatch_table[lo].fn;
    if(function){
        intptr_t delta=(intptr_t)(uintptr_t)function-lookup_anchor();
        if(cache_tag&&delta>=INT32_MIN&&delta<=INT32_MAX&&!dispatch_is_special(target))
            atomic_store_explicit(&lookup_cache[cache_slot],cache_tag|(uint32_t)(int32_t)delta,memory_order_relaxed);
        return function;
    }
    /* Preserve the existing immutable supplemental/extra cache. Main-table
     * hits above need neither a TLS descriptor nor per-thread cache storage. */
    static _Thread_local struct { uint64_t target; void (*fn)(CPU *); } cache[256];
    size_t slot=(size_t)((target>>3)^(target>>13))&255;
    if(cache[slot].fn&&cache[slot].target==target)return cache[slot].fn;
#if defined(FROSTPUNK_HAVE_SUPPLEMENTAL)
    function=frostpunk_supplemental_lookup(target);
#endif
    if(!function)function=fp_android_extra_aot_lookup(target);
    if(function){cache[slot].target=target;cache[slot].fn=function;}
    return function;
}

int win64_d3d11_dispatch(CPU *cpu, uint64_t target); /* android/dxvk/bridge.h */
/* Direct handlers of resolved imports, indexed like recomp_imports; read inline by fl_import. */
int (*win64_import_fast(const char *dll, const char *name))(CPU *);
__attribute__((visibility("hidden"))) int (*_Atomic fp_import_fast[HLE_COUNT])(CPU *);
static unsigned char fp_import_known[HLE_COUNT];

static void (*main_static_lookup(uint64_t target))(CPU *);
/* For host helpers that call one guest callback many times (qsort): the function dispatch() would
 * reach for an ordinary main-image target, or NULL when the target needs the full dispatcher
 * (imports, COM sentinels, payload code, call tracing or a dispatch budget). */
void (*frostpunk_static_target(uint64_t target))(CPU *) {
    if (dispatch_budget || trace_calls) return NULL;
    if (target < UINT64_C(0x140000000) || target >= UINT64_C(0x144539000) ||
        (target >= UINT64_C(0x141babd20) && target <= UINT64_C(0x141bc2d40))) return NULL;
    return main_static_lookup(target);
}
/* switch_performance.c: graphics defaults of a new profile (absent from host-only test builds). */
extern void fp_graphics_first_run_defaults(void) __attribute__((weak));
extern void fp_graphics_budget_note(uint64_t self, uint64_t target, int after, uint64_t result) __attribute__((weak));
static int fp_budget_target(uint64_t target) {
    return target == UINT64_C(0x140E5DA20) || target == UINT64_C(0x140E65A50);
}
static void dispatch_resolved(CPU *c, uint64_t target, void (*linked)(CPU *)) {
    if (__builtin_expect(dispatch_budget || trace_calls,0)) {
        uint64_t calls=atomic_fetch_add_explicit(&dispatch_calls,1,memory_order_relaxed)+1;
        if (dispatch_budget && calls > dispatch_budget) stop("dispatch budget exceeded target=0x%" PRIx64, target);
    }
    FP_PERF_TICK();
    if (target >= HLE_BASE && target - HLE_BASE < (uint64_t)HLE_COUNT * HLE_STRIDE) {
        FP_PERF(FP_PERF_IMPORT);
        if ((target - HLE_BASE) % HLE_STRIDE) stop("unaligned import sentinel=0x%" PRIx64, target);
        size_t i = (size_t)((target - HLE_BASE) / HLE_STRIDE);
        if (win64_dispatch_import(c,recomp_imports[i].dll,recomp_imports[i].name)) {
            /* Publish the import's direct handler once for fl_import (fastlift.h). */
            if (__builtin_expect(!fp_import_known[i],0)) {
                int (*fast)(CPU *)=win64_import_fast(recomp_imports[i].dll,recomp_imports[i].name);
                if (fast) atomic_store_explicit(&fp_import_fast[i],fast,memory_order_release);
                fp_import_known[i]=1;
            }
            return;
        }
        fail(70, "unimplemented import %s!%s slot=0x%" PRIx64 " return=0x%" PRIx64
             " dispatch_calls=%" PRIu64, recomp_imports[i].dll, recomp_imports[i].name,
             recomp_imports[i].slot, return_pc(c), dispatch_calls);
    }
    void (*function)(CPU *) = NULL;
    if (target >= UINT64_C(0x140000000) && target < UINT64_C(0x144539000) &&
        (target < UINT64_C(0x141babd20) || target > UINT64_C(0x141bc2d40))) {
        function=linked;
        if(!function || target==UINT64_C(0x140e222e0) || target==UINT64_C(0x140d95650) ||
           target==UINT64_C(0x140f499a0) || target==UINT64_C(0x1410596a9) || target==UINT64_C(0x1410597e9) ||
           target==UINT64_C(0x141250610) || target==UINT64_C(0x14124c7b0) ||
           target==UINT64_C(0x140ec6200) || target==UINT64_C(0x140ec7240) || target==UINT64_C(0x140e74b30) ||
           target==UINT64_C(0x140e26e40) || target==UINT64_C(0x140e26f30) || target==UINT64_C(0x140e26f90) ||
           target==UINT64_C(0x140f99f00) || target==UINT64_C(0x140f9d240) || target==UINT64_C(0x141c5c490) ||
           target==UINT64_C(0x141c54e60) || target==UINT64_C(0x141c54af0)||target==UINT64_C(0x141c551d0))
            function=main_static_lookup(target);
        if(function){FP_PERF(FP_PERF_MAIN);goto static_invoke;}
    }
#if defined(FROSTPUNK_ANDROID_LIBRARY)
    /* D3D11 COM methods are about 97% of the dynamic targets (15k per frame in a developed city)
     * and their sentinel range is disjoint from every other provider's: try them first. */
    if (win64_d3d11_dispatch(c,target)) {FP_PERF(FP_PERF_DYNAMIC);FP_PERF(FP_PERF_D3D11);c->rsp+=8;return;}
#endif
#if defined(FROSTPUNK_HAVE_PAYLOAD)
    if (frostpunk_payload_import(c,target)) {FP_PERF(FP_PERF_PAYLOAD);return;}
#endif
    if (frostpunk_dependencies_import(c,target)) {FP_PERF(FP_PERF_DEPENDENCY);return;}
    if (win64_dispatch_dynamic(c,target)) {FP_PERF(FP_PERF_DYNAMIC);return;}
    FP_PERF(FP_PERF_MISS);
#if defined(FROSTPUNK_HAVE_PAYLOAD)
    function=frostpunk_payload_lookup(target);
#endif
    if (!function) function=frostpunk_dependencies_lookup(target);
    if (!function) {
        function=main_static_lookup(target);
        if (!function) fail(71, "missing static target=0x%" PRIx64 " return=0x%" PRIx64, target, return_pc(c));
    }
static_invoke:
    c->rip = target;
    if (__builtin_expect(target==UINT64_C(0x140e02420),0)) {
        fprintf(stderr,"WINMAIN_ENTERED target=0x%" PRIx64 " return=0x%" PRIx64 "\n",target,return_pc(c));
        if (fp_graphics_first_run_defaults) fp_graphics_first_run_defaults();
    }
    if (__builtin_expect(trace_calls,0)) fprintf(stderr, "STATIC_CALL target=0x%" PRIx64 "\n", target);
    uint64_t budget_self = 0;
    int budget = fp_budget_target(target);
    if (budget) budget_self = c->rcx;
    if (budget && target == UINT64_C(0x140E65A50) && fp_graphics_budget_note)
        fp_graphics_budget_note(budget_self, target, 0, 0);
    function(c);
    if (budget && fp_graphics_budget_note)
        fp_graphics_budget_note(budget_self, target, 1, c->rax);
}

void dispatch(CPU *c, uint64_t target) { dispatch_resolved(c,target,NULL); }
__attribute__((visibility("hidden"))) void fp_dispatch_main(CPU *c, uint64_t target, void (*linked)(CPU *)) {
    if (__builtin_expect(fp_budget_target(target), 0)) { dispatch_resolved(c, target, linked); return; }
#if !defined(FP_PERF_COUNTERS)
    unsigned h=dispatch_special_hash(target);
    if(__builtin_expect(dispatch_fast_ready&&!((dispatch_special_filter[h>>6]>>(h&63u))&1u),1)){
        c->rip=target; linked(c); return;
    }
#endif
    dispatch_resolved(c,target,linked);
}
void dispatch_jmp(CPU *c, uint64_t target) { dispatch(c, target); }

typedef struct {
    const unsigned char *bytes;
    size_t size;
    uint32_t image_size, headers, entry, section_table;
    uint16_t sections;
    uint64_t base;
    uint32_t directories, directory_table;
} pe_t;

static uint16_t u16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t u32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t u64(const unsigned char *p) { uint64_t v; memcpy(&v,p,8); return v; }
static uint32_t max32(uint32_t a, uint32_t b) { return a > b ? a : b; }

static const char *parse_pe(const unsigned char *b, size_t size, pe_t *out) {
    if (size < 64 || b[0] != 'M' || b[1] != 'Z') return "missing DOS header";
    uint32_t pe = u32(b + 0x3c);
    if (!fits(pe, 24, size) || memcmp(b + pe, "PE\0\0", 4)) return "missing PE header";
    if (u16(b + pe + 4) != 0x8664) return "not AMD64";
    uint16_t sections = u16(b + pe + 6), opt_size = u16(b + pe + 20);
    uint64_t opt = (uint64_t)pe + 24;
    if (!sections || sections > 96 || opt_size < 112 || !fits(opt, opt_size, size)) return "optional header bounds";
    if (u16(b + opt) != 0x20b) return "not PE32+";
    uint32_t dirs = u32(b + opt + 108);
    if (dirs > 16 || !fits(112, (uint64_t)dirs * 8, opt_size)) return "directory table bounds";
    uint32_t image = u32(b + opt + 56), headers = u32(b + opt + 60), entry = u32(b + opt + 16);
    uint64_t base = u64(b + opt + 24), table = opt + opt_size;
    long page = sysconf(_SC_PAGESIZE);
    /* SizeOfImage follows the 4 KB PE section alignment; one anonymous RW mapping of it is
     * rounded up by the kernel on 16 KB-page hosts. Only the base must be host-page aligned. */
    if (page <= 0 || !image || image > MAX_IMAGE || image % 0x1000u ||
        base < 0x10000 || base % (uint64_t)page || base > UINT64_MAX - image) return "image geometry";
    if (!headers || headers > image || headers > size ||
        !fits(table, (uint64_t)sections * 40, headers)) return "section table bounds";
    if (entry >= image) return "entry outside image";
    int entry_executable = 0;
    for (unsigned i = 0; i < sections; ++i) {
        const unsigned char *s = b + table + i * 40;
        uint32_t vs = u32(s+8), va = u32(s+12), raw_size = u32(s+16), raw = u32(s+20);
        uint32_t span = max32(vs, raw_size);
        if (span && (va < headers || !fits(va, span, image))) return "section virtual bounds";
        if (raw_size && (raw < headers || !fits(raw, raw_size, size))) return "section file bounds";
        if (span && entry >= va && entry - va < span && (u32(s+36) & 0x20000000u)) entry_executable = 1;
        for (unsigned j = 0; j < i; ++j) {
            const unsigned char *p = b + table + j * 40;
            uint32_t pva = u32(p+12), plen = max32(u32(p+8), u32(p+16));
            if (span && plen && (uint64_t)va < (uint64_t)pva + plen && (uint64_t)pva < (uint64_t)va + span)
                return "overlapping virtual sections";
        }
    }
    if (!entry_executable) return "entry outside executable section metadata";
    *out = (pe_t){b, size, image, headers, entry, (uint32_t)table, sections, base, dirs, (uint32_t)opt + 112};
    return NULL;
}

static int mapped_nx(const void *mapping, size_t length) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    uintptr_t cursor = (uintptr_t)mapping, end = cursor + length;
    char line[1024], perms[5];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t a, b;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &a, &b, perms) != 3) continue;
        if (a <= cursor && cursor < b) {
            if (perms[2] == 'x') { fclose(f); return 0; }
            cursor = b < end ? b : end;
            if (cursor == end) { fclose(f); return 1; }
        }
    }
    fclose(f); return 0;
}

static const char *image_string(const unsigned char *image, size_t size, uint32_t rva) {
    if (rva >= size || !memchr(image + rva, 0, size - rva)) stop("import string bounds rva=0x%x", rva);
    return (const char *)image + rva;
}

static void patch_imports(const pe_t *pe, unsigned char *image) {
    if (pe->directories < 2) { if (HLE_COUNT) stop("missing import directory"); return; }
    uint32_t rva = u32(pe->bytes + pe->directory_table + 8);
    uint32_t size = u32(pe->bytes + pe->directory_table + 12);
    if (!rva && !size) { if (HLE_COUNT) stop("missing import directory"); return; }
    if (!fits(rva, size, pe->image_size) || size < 20) stop("import directory bounds");
    unsigned char *seen = calloc(HLE_COUNT ? HLE_COUNT : 1, 1);
    if (!seen) stop("import allocation");
    size_t imports = 0;
    int terminated = 0;
    for (uint64_t off = rva; fits(off, 20, (uint64_t)rva + size); off += 20) {
        const unsigned char *d = image + off;
        uint32_t names = u32(d), name = u32(d+12), slots = u32(d+16);
        if (!u32(d) && !u32(d+4) && !u32(d+8) && !name && !slots) { terminated = 1; break; }
        if (!name || !slots) stop("invalid import descriptor");
        const char *dll = image_string(image, pe->image_size, name);
        if (!names) names = slots;
        for (uint64_t n = 0;; ++n) {
            uint64_t nt = (uint64_t)names + n * 8, slot = (uint64_t)slots + n * 8;
            if (!fits(nt, 8, pe->image_size) || !fits(slot, 8, pe->image_size)) stop("import thunk bounds");
            uint64_t thunk = u64(image + nt);
            if (!thunk) break;
            char ordinal[24];
            const char *symbol;
            if (thunk >> 63) {
                if (thunk & UINT64_C(0x7fffffffffff0000)) stop("malformed ordinal import");
                snprintf(ordinal, sizeof(ordinal), "#%u", (unsigned)(uint16_t)thunk); symbol = ordinal;
            } else {
                if (thunk > UINT32_MAX || !fits(thunk, 3, pe->image_size)) stop("import hint bounds");
                symbol = image_string(image, pe->image_size, (uint32_t)thunk + 2);
            }
            size_t i;
            for (i = 0; i < HLE_COUNT; ++i) if (recomp_imports[i].slot == pe->base + slot) break;
            if (i == HLE_COUNT || seen[i] || strcmp(dll, recomp_imports[i].dll) || strcmp(symbol, recomp_imports[i].name))
                stop("generated import mismatch %s!%s slot=0x%" PRIx64, dll, symbol, pe->base + slot);
            uint64_t sentinel = HLE_SENTINEL(i);
            if (!strcasecmp(dll,"msvcp140.dll")) {
                sentinel=frostpunk_dependencies_export(symbol);
                if (!sentinel) stop("unbound MSVCP export %s",symbol);
            }
            memcpy(image + slot, &sentinel, 8); seen[i] = 1; ++imports;
        }
    }
    free(seen);
    if (!terminated || imports != HLE_COUNT) stop("incomplete import table parsed=%zu expected=%u", imports, HLE_COUNT);
    fprintf(stderr, "IMPORTS verified=%zu fail_closed=1\n", imports);
}

static void check_dispatch_table(void) {
    for (uint32_t i = 0; i < recomp_dispatch_count; ++i)
        if (!recomp_dispatch_table[i].fn || recomp_dispatch_table[i].va<MAIN_BUCKET_BASE ||
            recomp_dispatch_table[i].va>=MAIN_BUCKET_BASE+MAIN_BUCKET_BYTES || (i && recomp_dispatch_table[i-1].va >= recomp_dispatch_table[i].va))
            stop("invalid generated dispatch order");
    main_bucket_init();
}

static void require(int condition, const char *detail) { if (!condition) stop("self-test %s", detail); }
static void put16(unsigned char *b, size_t off, uint16_t v) { memcpy(b+off,&v,2); }
static void put32(unsigned char *b, size_t off, uint32_t v) { memcpy(b+off,&v,4); }
static void put64(unsigned char *b, size_t off, uint64_t v) { memcpy(b+off,&v,8); }

static void process_environment(uint64_t image) {
    memset(guest_peb,0,sizeof(guest_peb)); memset(process_parameters,0,sizeof(process_parameters));
    put64(guest_peb,0x10,image); put64(guest_peb,0x20,(uintptr_t)process_parameters);
    put32(guest_peb,0xb8,(uint32_t)sysconf(_SC_NPROCESSORS_ONLN));
    frostpunk_teb_write(0x40,(uint32_t)getpid(),8);
    frostpunk_teb_write(0x48,(uint32_t)gettid(),8);
    frostpunk_teb_write(0x60,(uintptr_t)guest_peb,8);
    win64_region_add((uintptr_t)guest_teb,sizeof(guest_teb),1);
    win64_region_add((uintptr_t)guest_peb,sizeof(guest_peb),1);
    win64_region_add((uintptr_t)process_parameters,sizeof(process_parameters),1);
}

static void initial_tls_callbacks(const pe_t *pe,CPU *c,uint64_t list) {
    if (!list) return;
    if (list<pe->base || !fits(list-pe->base,8,pe->image_size)) stop("TLS callback list bounds");
    for (unsigned i=0;i<64;++i) {
        if (!fits(list-pe->base+i*8,8,pe->image_size)) stop("TLS callback entry bounds");
        uint64_t target=rd64(list+i*8); if (!target) return;
        if (target<pe->base || target-pe->base>=pe->image_size) stop("TLS callback target outside image");
        CPU saved=*c; c->rsp=(c->rsp-64)&~UINT64_C(15); uint64_t before=c->rsp;
        c->rcx=pe->base; c->rdx=1; c->r8=0; push64(c,UINT64_C(0xe5ef0010));
        fprintf(stderr,"TLS_CALLBACK process_attach=0x%" PRIx64 "\n",target); dispatch(c,target);
        if (c->rsp!=before) stop("TLS callback stack imbalance");
        *c=saved;
    }
    stop("unterminated TLS callback list");
}

static void initialize_static_tls(const pe_t *pe,unsigned char *image) {
    if (pe->directories<10) return;
    uint32_t rva=u32(pe->bytes+pe->directory_table+9*8), bytes=u32(pe->bytes+pe->directory_table+9*8+4);
    if (!rva && !bytes) return;
    if (bytes<40 || !fits(rva,40,pe->image_size)) stop("static TLS directory bounds");
    const unsigned char *tls=image+rva;
    uint64_t first=u64(tls),last=u64(tls+8),index=u64(tls+16);
    uint32_t zero=u32(tls+32),alignment_code=(u32(tls+36)>>20)&15;
    if (first<pe->base || last<first || last-pe->base>pe->image_size ||
        index<pe->base || !fits(index-pe->base,4,pe->image_size) ||
        last-first>16*1024*1024 || zero>16*1024*1024 || alignment_code>5) stop("static TLS geometry");
    size_t total=(size_t)(last-first)+zero;
    uint64_t storage=win64_alloc(total?total:1,1);
    if (!storage) stop("static TLS allocation");
    memcpy((void *)(uintptr_t)storage,(void *)(uintptr_t)first,(size_t)(last-first));
    memset(static_tls_vector,0,sizeof(static_tls_vector)); static_tls_vector[0]=storage;
    wr32(index,0); frostpunk_teb_write(0x58,(uintptr_t)static_tls_vector,8);
    win64_region_add((uintptr_t)static_tls_vector,sizeof(static_tls_vector),1);
    fprintf(stderr,"STATIC_TLS index=0 template=%" PRIu64 " zero=%u alignment=%u\n",last-first,zero,alignment_code?1u<<(alignment_code-1):1);
}

#include "bootstrap_threads.inc"

static void self_test(void) {
    win64_kernel_roots_self_test();
    bootstrap_threads_install(NULL,NULL);
    win64_self_test();
    CPU feature={0}; feature.rax=1; feature.r8=0x12345678; feature.zf=1;
    frostpunk_cpuid(&feature);
    require(feature.rcx==0 && feature.rdx==((1u<<15)|(1u<<25)|(1u<<26)) && feature.r8==0x12345678 && feature.zf==1,"guest SSE2 CPU contract");
    unsigned char sample[0x400] = {0};
    pe_t pe;
    sample[0]='M'; sample[1]='Z'; put32(sample,0x3c,0x80); memcpy(sample+0x80,"PE\0\0",4);
    put16(sample,0x84,0x8664); put16(sample,0x86,1); put16(sample,0x94,240);
    put16(sample,0x98,0x20b); put32(sample,0xa8,0x1000); put64(sample,0xb0,ORIGINAL_BASE);
    put32(sample,0xd0,0x3000); put32(sample,0xd4,0x200); put32(sample,0x104,16);
    put32(sample,0x190,0x100); put32(sample,0x194,0x1000); put32(sample,0x198,0x200);
    put32(sample,0x19c,0x200); put32(sample,0x1ac,0x60000020);
    require(parse_pe(sample,sizeof(sample),&pe)==NULL,"valid synthetic PE rejected");
    require(parse_pe(sample,63,&pe)!=NULL,"short DOS header accepted");
    require(parse_pe(sample,sizeof(sample)-1,&pe)!=NULL,"truncated section accepted");
    put32(sample,0x194,0x2f00); require(parse_pe(sample,sizeof(sample),&pe)!=NULL,"section overflow accepted");
    put32(sample,0x194,0x1000); put32(sample,0xa8,0x3000);
    require(parse_pe(sample,sizeof(sample),&pe)!=NULL,"outside entry accepted");
    put32(sample,0xa8,0x1000); put16(sample,0x86,2);
    memcpy(sample+0x1b0,sample+0x188,40);
    require(parse_pe(sample,sizeof(sample),&pe)!=NULL,"overlapping sections accepted");
    require(!fits(UINT64_MAX,2,UINT64_MAX) && fits(8,8,16),"overflow-safe bounds");
    require(!teb_accessible(TEB_BYTES-3,4) && !teb_accessible(UINT64_MAX,8),"TEB bounds");
    frostpunk_teb_write(0x30,UINT64_C(0x123456789abcdef0),8);
    require(frostpunk_teb_read(0x30,8)==UINT64_C(0x123456789abcdef0),"TEB round trip");
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    void *nx=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    require(nx!=MAP_FAILED,"NX mmap failed"); require(mapped_nx(nx,page),"NX mapping verification failed");
    munmap(nx,page);
    for (unsigned flags=0; flags<64; ++flags) {
        uint64_t stack[8]={0}; CPU c, expected;
        memset(&c,0x5a,sizeof(c));
        c.cf=(flags>>0)&1; c.pf=(flags>>1)&1; c.af=(flags>>2)&1;
        c.zf=(flags>>3)&1; c.sf=(flags>>4)&1; c.of=(flags>>5)&1;
        c.rsp=(uint64_t)(uintptr_t)&stack[4]; stack[4]=UINT64_C(0xfeedbeef12345678);
        expected=c; expected.rax=ORIGINAL_ENTRY+5; expected.rsp+=8;
        frostpunk_call_next_test(&c); expected.rip=c.rip;
        require(!memcmp(&c,&expected,sizeof(c)),"call-next CPU state mismatch");
        require(stack[3]==ORIGINAL_ENTRY+5 && stack[4]==UINT64_C(0xfeedbeef12345678),"call-next guest stack mismatch");
    }
    fprintf(stderr,"SELF_TEST passed PE_bounds=7 NX=1 TEB_bounds=1 call_next_flag_cases=64\n");
}

void fp_thread_placement_main(void) __attribute__((weak)); /* android/host/thread_placement.c */
int frostpunk_process_main(int argc,char **argv,PFN_vkGetInstanceProcAddr supplied_provider,void *native_window) {
    if (argc==2 && !strcmp(argv[1],"--self-test")) { self_test(); return 0; }
    if (fp_thread_placement_main) fp_thread_placement_main();
    if (argc!=3 || (strcmp(argv[1],"--run") && strcmp(argv[1],"--run-crt") && strcmp(argv[1],"--run-local") && strcmp(argv[1],"--inspect"))) {
        fprintf(stderr,"Usage: %s --self-test | --inspect <Frostpunk.exe> | --run <Frostpunk.exe> | --run-crt <Frostpunk.exe> | --run-local <Frostpunk.exe>\n",argv[0]); return 64;
    }
    const win64_environment_entry environment[]={
        {"TEMP","C:\\Users\\Android\\AppData\\Roaming\\11bitstudios\\Frostpunk\\"},
        {"TMP","C:\\Users\\Android\\AppData\\Roaming\\11bitstudios\\Frostpunk\\"},
        {"SDL_AUDIODRIVER","dummy"},
        {"SDL_XINPUT_ENABLED","0"}
    };
    if (!win64_environment_configure(environment,sizeof(environment)/sizeof(*environment)))
        stop("guest process environment configuration failed");
    check_dispatch_table();
    FILE *f=fp_data_fopen(argv[2],"rb"); if (!f) stop("open original PE errno=%d",errno);
    if (fseek(f,0,SEEK_END)) stop("seek original PE");
    long length=ftell(f); if (length<=0 || length>MAX_IMAGE) stop("original PE file size");
    rewind(f); unsigned char *bytes=malloc((size_t)length); if (!bytes) stop("file allocation");
    if (fread(bytes,1,(size_t)length,f)!=(size_t)length || ferror(f)) stop("read original PE"); fclose(f);
    pe_t pe; const char *error=parse_pe(bytes,(size_t)length,&pe); if (error) stop("PE validation: %s",error);
    if (pe.base!=ORIGINAL_BASE || pe.base+pe.entry!=ORIGINAL_ENTRY) stop("unexpected PE image base or entry");
    unsigned char *image=mmap((void *)(uintptr_t)pe.base,pe.image_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (image==MAP_FAILED) stop("image mmap errno=%d",errno);
    if ((uint64_t)(uintptr_t)image!=pe.base) { munmap(image,pe.image_size); stop("preferred image base occupied"); }
    memcpy(image,bytes,pe.headers);
    for (unsigned i=0;i<pe.sections;++i) {
        const unsigned char *s=bytes+pe.section_table+i*40;
        uint32_t raw_size=u32(s+16); if (raw_size) memcpy(image+u32(s+12),bytes+u32(s+20),raw_size);
    }
    patch_imports(&pe,image);
    if (!mapped_nx(image,pe.image_size)) stop("image NX verification failed");
    if (!win64_math_init(image,pe.image_size)) stop("math error handler original opcode mismatch");
    if (!win64_rtti_init(image,pe.image_size)) stop("RTTI main image initialization failed");
    if (!fp_aot_cpu_init(image,pe.image_size)) stop("portable CPU original opcode mismatch");
    if (!fp_android_extra_aot_init(image,pe.image_size)) stop("extra AOT original opcode mismatch");
#if defined(FROSTPUNK_HAVE_STRING_RESIZE)
    if (!fp_android_string_resize_init(image,pe.image_size)) stop("string resize original opcode mismatch");
#endif
#if defined(FROSTPUNK_HAVE_TEXTURE_FORMATS)
    if (!fp_android_texture_formats_init(image,pe.image_size)) stop("texture format original opcode mismatch");
#endif
    size_t page=(size_t)sysconf(_SC_PAGESIZE), reserve=STACK_BYTES+2*page;
    unsigned char *stack=mmap(NULL,reserve,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (stack==MAP_FAILED || mprotect(stack+page,STACK_BYTES,PROT_READ|PROT_WRITE)) stop("guest stack mapping");
    stack_lo=(uint64_t)(uintptr_t)(stack+page); stack_hi=stack_lo+STACK_BYTES;
    if (!mapped_nx(stack+page,STACK_BYTES)) stop("stack NX verification failed");
    memset(guest_teb,0,sizeof(guest_teb)); memset(teb_known,0,sizeof(teb_known));
    frostpunk_teb_write(0x08,stack_hi,8); frostpunk_teb_write(0x10,stack_lo,8);
    frostpunk_teb_write(0x30,(uint64_t)(uintptr_t)guest_teb,8);
    CPU c={0}; c.rsp=(stack_hi-0x40)&~UINT64_C(15); c.rsp-=8; c.mxcsr=0x1f80;
    wr64(c.rsp,UINT64_C(0xfffffffffffffff0));
    g_image_delta=0; es3_eh_image_base=pe.base;
    win64_init(pe.base,pe.image_size,stack_lo,STACK_BYTES);
    process_environment(pe.base);
    bootstrap_threads_install(&pe,image);
    fprintf(stderr,"PE_READY base=0x%" PRIx64 " entry=0x%" PRIx64 " bytes=%u image_NX=1 stack_NX=1 original_bytes_executed=0\n",pe.base,pe.base+pe.entry,pe.image_size);
    if (!strcmp(argv[1],"--inspect")) {
        fprintf(stderr,"INSPECT_ONLY entry_dispatched=0\n");
        return 0;
    }
    const char *gpu_runtime=getenv("FROSTPUNK_GPU_RUNTIME"), *gpu_driver=getenv("FROSTPUNK_GPU_DRIVER");
    if (!(supplied_provider?frostpunk_startup_gpu_configure(supplied_provider,native_window):
          gpu_runtime && gpu_driver && frostpunk_startup_gpu_init(gpu_runtime,gpu_driver)))
        stop("native Vulkan provider initialization failed; explicit runtime/driver paths required");
    uint64_t selected_entry=pe.base+pe.entry;
    if (!strcmp(argv[1],"--run-crt")) {
        selected_entry=UINT64_C(0x141ae13e0);
        fprintf(stderr,"DIAGNOSTIC_CRT_ENTRY target=0x%" PRIx64 " supplied_wrapper_entry=0x%" PRIx64 "\n",selected_entry,pe.base+pe.entry);
    }
    if (!strcmp(argv[1],"--run-local")) {
        const char *services=getenv("FROSTPUNK_SERVICES_RUNTIME"),*profile=getenv("FROSTPUNK_SERVICES_PROFILE"),*game=getenv("FROSTPUNK_GAME_ROOT");
        if (!win64_kernel_configure_roots(game,profile)) stop("explicit disjoint game/profile anchors must be configured before kernel use");
        if (!win64_shell_provision()) stop("private known-folder provisioning failed error=%u",win64_get_error());
        fputs("SHELL_FOLDERS_READY current_redirects=2 private_profile_anchor=1 filesystem_verified=1\n",stderr);
        if (!frostpunk_startup_services_init(services,profile,game)) stop("explicit native local-services configuration failed");
        char audio_library[PATH_MAX];int audio_length=snprintf(audio_library,sizeof(audio_library),"%s/libopenal.so",services);
        if (audio_length<0 || (size_t)audio_length>=sizeof(audio_library) || !win64_openal_configure(audio_library,profile))
            stop("explicit native AAudio provider configuration failed");
        fprintf(stderr,"OPENAL_PROVIDER_READY native_aarch64=1 typed_routes=30 guarded_entries=157 AAudio_backend_only=1 playback_started=0\n");
        win64_newsfeed_enable_offline();
        selected_entry=UINT64_C(0x141ae13e0);
        dispatch_budget=0;
        const char *trace=getenv("FROSTPUNK_TRACE_DISPATCH");trace_calls=trace && !strcmp(trace,"1");
        fprintf(stderr,"LOCAL_OFFLINE_ENTRY target=0x%" PRIx64 " supplied_wrapper_entry=0x%" PRIx64 " original_entry_selection_modified=1\n",selected_entry,pe.base+pe.entry);
    }
    initialize_static_tls(&pe,image);
    /* Loader/thread metadata now refers to the mapped image, not file bytes. */
    free(bytes);pe.bytes=NULL;pe.size=0;
    fprintf(stderr,"PE_SOURCE_RELEASED file_bytes=%ld\n",length);
    frostpunk_dependencies_init(&c);
    initial_tls_callbacks(&pe,&c,bootstrap_thread_image.callbacks);
    bootstrap_threads_process_attached(&c);
    fprintf(stderr,"STATIC_ENTRY target=0x%" PRIx64 " tracing=%d dispatch_budget=%" PRIu64 "\n",selected_entry,trace_calls,dispatch_budget);
    /* Guest threads start below; the fast path is fixed before any of them runs. */
    dispatch_fast_init();
    dispatch_fast_ready=!dispatch_budget&&!trace_calls;
    fprintf(stderr,"DISPATCH_FAST_PATH enabled=%d lookup_cache_slots=%u\n",dispatch_fast_ready,LOOKUP_CACHE_SLOTS);
    dispatch(&c,selected_entry);
    stop("PE entry returned; Windows process lifecycle is not implemented rax=0x%" PRIx64,c.rax);
}
#if !defined(FROSTPUNK_ANDROID_LIBRARY)
int main(int argc,char **argv) { return frostpunk_process_main(argc,argv,NULL,NULL); }
#endif
