#define _GNU_SOURCE
#include "dependencies.h"
#include "win64.h"
#include "../../generated/dependencies/msvcp140/dependency_image.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct { uint64_t va; frostpunk_dependency_fn fn; } dependency_entry;
typedef struct { uint64_t slot; const char *dll; const char *name; } dependency_import;
extern const dependency_entry msvcp140_recomp_dispatch_table[];
extern const uint32_t msvcp140_recomp_dispatch_count;
extern const dependency_import msvcp140_recomp_imports[];
static atomic_int state; /* 0 uninitialized; 1 mapping; 2 mapped; 3 DllMain succeeded */

static _Noreturn void dependency_stop(int code, const char *detail, uint64_t address) {
    fprintf(stderr, "STOP msvcp140 %s address=0x%" PRIx64 "\n", detail, address);
    exit(code);
}

static int fits(uint64_t offset, uint64_t size, uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}
static uint16_t read16(const void *p) { uint16_t value; memcpy(&value,p,2); return value; }
static uint32_t read32(const void *p) { uint32_t value; memcpy(&value,p,4); return value; }
static uint64_t read64(const void *p) { uint64_t value; memcpy(&value,p,8); return value; }

static void *image_at(uint32_t rva, size_t bytes) {
    if (!fits(rva,bytes,DEPENDENCY_SIZE)) dependency_stop(73,"image bounds",rva);
    return (void *)(uintptr_t)(DEPENDENCY_BASE+rva);
}
static const char *image_string(uint32_t rva) {
    if (rva >= DEPENDENCY_SIZE) dependency_stop(73,"string bounds",rva);
    const char *p=image_at(rva,1);
    if (!memchr(p,0,DEPENDENCY_SIZE-rva)) dependency_stop(73,"unterminated image string",rva);
    return p;
}

static int mapped_nx(void) {
    FILE *maps=fopen("/proc/self/maps","r");
    if (!maps) return 0;
    uint64_t cursor=DEPENDENCY_BASE,end=DEPENDENCY_BASE+DEPENDENCY_SIZE;
    char line[1024],mode[5];
    while (fgets(line,sizeof(line),maps)) {
        uint64_t low,high;
        if (sscanf(line,"%" SCNx64 "-%" SCNx64 " %4s",&low,&high,mode)!=3) continue;
        if (low<=cursor && cursor<high) {
            if (mode[0]!='r' || mode[2]=='x') break;
            cursor=high<end?high:end;
            if (cursor==end) { fclose(maps); return 1; }
        }
    }
    fclose(maps); return 0;
}

static void patch_imports(unsigned char *image, uint32_t pe) {
    unsigned char seen[DEPENDENCY_IMPORTS]={0};
    uint32_t directory=read32(image+pe+24+112+8), bytes=read32(image+pe+24+112+12);
    if (!fits(directory,bytes,DEPENDENCY_SIZE) || bytes<20) dependency_stop(73,"import directory bounds",directory);
    unsigned patched=0;
    int terminated=0;
    for (uint32_t offset=0; fits(offset,20,bytes); offset+=20) {
        const unsigned char *descriptor=image_at(directory+offset,20);
        uint32_t original=read32(descriptor),name=read32(descriptor+12),first=read32(descriptor+16);
        if (!original && !name && !first) { terminated=1; break; }
        const char *dll=image_string(name);
        uint32_t lookup=original?original:first;
        int thunk_end=0;
        for (uint32_t index=0; index<=DEPENDENCY_IMPORTS; ++index) {
            uint64_t lookup_rva=(uint64_t)lookup+index*8ull,slot_rva=(uint64_t)first+index*8ull;
            if (!fits(lookup_rva,8,DEPENDENCY_SIZE) || !fits(slot_rva,8,DEPENDENCY_SIZE))
                dependency_stop(73,"import thunk bounds",slot_rva);
            uint64_t value=read64(image+(size_t)lookup_rva);
            if (!value) { thunk_end=1; break; }
            char ordinal[24];
            const char *import_name;
            if (value & UINT64_C(0x8000000000000000)) {
                snprintf(ordinal,sizeof(ordinal),"#%u",(unsigned)(value&0xffff)); import_name=ordinal;
            } else {
                if (!fits(value,3,DEPENDENCY_SIZE)) dependency_stop(73,"import name bounds",value);
                import_name=image_string((uint32_t)value+2);
            }
            uint64_t slot=DEPENDENCY_BASE+slot_rva;
            unsigned low=0,high=DEPENDENCY_IMPORTS;
            while (low<high) {
                unsigned mid=low+(high-low)/2;
                if (msvcp140_recomp_imports[mid].slot<slot) low=mid+1; else high=mid;
            }
            if (low==DEPENDENCY_IMPORTS || msvcp140_recomp_imports[low].slot!=slot || seen[low] ||
                strcmp(msvcp140_recomp_imports[low].dll,dll) || strcmp(msvcp140_recomp_imports[low].name,import_name))
                dependency_stop(73,"IAT differs from compiled dependency metadata",slot);
            uint64_t sentinel=DEPENDENCY_HLE_BASE+(uint64_t)low*8;
            memcpy(image+(size_t)slot_rva,&sentinel,8); seen[low]=1; ++patched;
        }
        if (!thunk_end) dependency_stop(73,"unterminated import thunk",lookup);
    }
    if (!terminated || patched!=DEPENDENCY_IMPORTS) dependency_stop(73,"incomplete IAT verification",patched);
}

uint64_t frostpunk_dependencies_export(const char *name) {
    if (!name) return 0;
    size_t low=0,high=sizeof(dependency_exports)/sizeof(dependency_exports[0]);
    while (low<high) {
        size_t mid=low+(high-low)/2;
        if (strcmp(dependency_exports[mid].name,name)<0) low=mid+1; else high=mid;
    }
    return low<sizeof(dependency_exports)/sizeof(dependency_exports[0]) && !strcmp(dependency_exports[low].name,name)
           ? dependency_exports[low].va : 0;
}

frostpunk_dependency_fn frostpunk_dependencies_lookup(uint64_t target) {
    if (target<DEPENDENCY_BASE || target-DEPENDENCY_BASE>=DEPENDENCY_SIZE) return NULL;
    if (atomic_load_explicit(&state,memory_order_acquire)<2) dependency_stop(73,"static call before image initialization",target);
    uint32_t low=0,high=msvcp140_recomp_dispatch_count;
    while (low<high) {
        uint32_t mid=low+(high-low)/2;
        if (msvcp140_recomp_dispatch_table[mid].va<target) low=mid+1; else high=mid;
    }
    if (low==msvcp140_recomp_dispatch_count || msvcp140_recomp_dispatch_table[low].va!=target)
        dependency_stop(71,"missing static target",target);
    return msvcp140_recomp_dispatch_table[low].fn;
}

int frostpunk_dependencies_import(CPU *c, uint64_t target) {
    if (target<DEPENDENCY_HLE_BASE || target-DEPENDENCY_HLE_BASE>=DEPENDENCY_IMPORTS*8ull) return 0;
    if ((target-DEPENDENCY_HLE_BASE)%8) dependency_stop(73,"unaligned import sentinel",target);
    const dependency_import *import=&msvcp140_recomp_imports[(target-DEPENDENCY_HLE_BASE)/8];
    if (win64_dispatch_import(c,import->dll,import->name)) return 1;
    fprintf(stderr,"STOP unimplemented dependency import %s!%s slot=0x%" PRIx64 "\n",import->dll,import->name,import->slot);
    exit(70);
}

static int invoke_dllmain(CPU *c,unsigned reason) {
    CPU saved=*c;
    if(c->rsp<0x80)dependency_stop(73,"DllMain stack underflow",c->rsp);
    uint64_t call_top=(c->rsp-0x40)&~UINT64_C(15);
    win64_pointer(call_top-8,0x30,1);
    c->rsp=call_top;push64(c,0);
    c->rcx=DEPENDENCY_BASE;c->rdx=reason;c->r8=0;
    dispatch(c,DEPENDENCY_ENTRY);
    int success=(uint32_t)c->rax!=0;
    if(c->rsp!=call_top)dependency_stop(73,"DllMain returned with wrong guest stack",c->rsp);
    *c=saved;
    return success;
}

void frostpunk_dependencies_thread_notify(CPU *c,unsigned reason) {
    if(reason!=2&&reason!=3)dependency_stop(73,"invalid thread notification reason",reason);
    if(atomic_load_explicit(&state,memory_order_acquire)!=3)
        dependency_stop(73,"thread notification before successful DLL_PROCESS_ATTACH",reason);
    /* Windows ignores the BOOL returned for thread notifications. */
    (void)invoke_dllmain(c,reason);
    fprintf(stderr,"STATIC_DEPENDENCY DllMain_%s name=msvcp140.dll\n",reason==2?"THREAD_ATTACH":"THREAD_DETACH");
}

void frostpunk_dependencies_init(CPU *c) {
    int expected=0;
    if (!atomic_compare_exchange_strong(&state,&expected,1)) {
        if (expected==3) return;
        dependency_stop(73,"recursive or concurrent initialization",expected);
    }
    uint32_t pe=read32(dependency_file+0x3c);
    if (memcmp(dependency_file,"MZ",2) || !fits(pe,24+112+16,sizeof(dependency_file)) ||
        memcmp(dependency_file+pe,"PE\0\0",4) || read16(dependency_file+pe+4)!=0x8664 ||
        read16(dependency_file+pe+24)!=0x20b || read64(dependency_file+pe+24+24)!=DEPENDENCY_BASE ||
        read32(dependency_file+pe+24+56)!=DEPENDENCY_SIZE || read32(dependency_file+pe+24+60)!=DEPENDENCY_HEADERS ||
        DEPENDENCY_BASE+read32(dependency_file+pe+24+16)!=DEPENDENCY_ENTRY ||
        !fits(0,DEPENDENCY_HEADERS,sizeof(dependency_file)) || DEPENDENCY_HEADERS>DEPENDENCY_SIZE)
        dependency_stop(73,"compiled private PE identity/geometry mismatch",pe);
    unsigned char *image=mmap((void *)(uintptr_t)DEPENDENCY_BASE,DEPENDENCY_SIZE,PROT_READ|PROT_WRITE,
                              MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (image==MAP_FAILED) dependency_stop(73,"mmap failed",DEPENDENCY_BASE);
    if ((uintptr_t)image!=DEPENDENCY_BASE) { munmap(image,DEPENDENCY_SIZE); dependency_stop(73,"preferred mapping unavailable",DEPENDENCY_BASE); }
    memcpy(image,dependency_file,DEPENDENCY_HEADERS);
    for (size_t i=0;i<sizeof(dependency_sections)/sizeof(dependency_sections[0]);++i) {
        const dependency_section *s=&dependency_sections[i];
        size_t span=s->raw_size>s->virtual_size?s->raw_size:s->virtual_size;
        if (!fits(s->raw,s->raw_size,sizeof(dependency_file)) || !fits(s->va,span,DEPENDENCY_SIZE) || s->va<DEPENDENCY_HEADERS)
            dependency_stop(73,"section copy bounds",s->va);
        for (size_t j=0;j<i;++j) {
            const dependency_section *p=&dependency_sections[j];
            uint32_t previous=p->raw_size>p->virtual_size?p->raw_size:p->virtual_size;
            if (span && previous && (uint64_t)s->va<p->va+(uint64_t)previous && (uint64_t)p->va<s->va+(uint64_t)span)
                dependency_stop(73,"overlapping sections",s->va);
        }
        memcpy(image+s->va,dependency_file+s->raw,s->raw_size);
    }
    patch_imports(image,pe);
    if (!mapped_nx()) dependency_stop(73,"image NX validation failed",DEPENDENCY_BASE);
    win64_region_add(DEPENDENCY_BASE,DEPENDENCY_SIZE,1);
    win64_image_add(DEPENDENCY_BASE,DEPENDENCY_SIZE);
    atomic_store_explicit(&state,2,memory_order_release);
    fprintf(stderr,"STATIC_DEPENDENCY mapped=msvcp140.dll base=0x%" PRIx64 " bytes=%u IAT=%u NX=1 file_sha256=%s original_bytes_executed=0\n",
            DEPENDENCY_BASE,DEPENDENCY_SIZE,DEPENDENCY_IMPORTS,DEPENDENCY_FILE_SHA256);
    int success=invoke_dllmain(c,1);
    if (!success) dependency_stop(73,"original DllMain DLL_PROCESS_ATTACH returned FALSE",DEPENDENCY_ENTRY);
    atomic_store_explicit(&state,3,memory_order_release);
    fprintf(stderr,"STATIC_DEPENDENCY DllMain_PROCESS_ATTACH_TRUE name=msvcp140.dll\n");
}
