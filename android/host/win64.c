/* Native Win64 HLE. Guest pointers are data; every CPU call uses static dispatch. */
#define _GNU_SOURCE
#include "perf_counters.h"
#include "win64.h"
#include "port_adpf.h"
#include "../dxvk/bridge.h"
#include "win64_kernel.h"
#include "win64_threads.h"
#include "win64_sync.h"
#include "win64_stdio.h"
#include "win64_wide_stdio.h"
#include "win64_locale.h"
#include "win64_string.h"
#include "win64_system.h"
#include "win64_scan.h"
#include "win64_utility.h"
#include "win64_newsfeed.h"
#include "win64_display.h"
#include "win64_math.h"
#include "win64_rtti.h"
#include "win64_shader.h"
#include "win64_environment.h"
#include "win64_com.h"
#include "win64_time.h"
#include "win64_dbghelp.h"
#include "win64_error_message.h"
#include "win64_encoding.h"
#include "win64_openal.h"
#include "win64_shell.h"
#include "win64_path.h"
#include "win64_jump.h"
#include "win64_window.h"
#include "win64_dinput.h"
#include "win64_accessibility.h"
#include "../steam/bridge.h"
#include "recomp_dispatch.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>
#include <fenv.h>
#include "exports.h"

#define EXPORT_BASE UINT64_C(0xe5e40000)
#define EXPORT_CAPACITY 2048
#define MODULE_CAPACITY 64
#define REGION_CACHE_CAPACITY 64

typedef struct region { uint64_t base,owner; size_t size; struct region *left,*right; uint64_t max_end; uint32_t protection,state; unsigned height; struct region *owner_next,*next,*previous; } region;
typedef struct { char name[128]; uint64_t handle; unsigned refs; int pinned; } module;
typedef struct { module *owner; char *name; } export_entry;
static region *regions;
typedef struct { uint64_t base; size_t size; uint32_t protection,state; } region_snapshot;
typedef struct { region_snapshot entries[REGION_CACHE_CAPACITY]; size_t count; uint64_t epoch; } region_cache_state;
static _Thread_local region_cache_state region_tls;
#define region_cache (region_tls.entries)
#define region_cache_count (region_tls.count)
#define region_cache_epoch (region_tls.epoch)
static _Thread_local unsigned region_mutation_depth;
#define REGION_JOURNAL_CAPACITY 16
typedef struct { uint64_t epoch,low,high; unsigned known; } region_change;
static region_change region_journal[REGION_JOURNAL_CAPACITY];
static _Thread_local region_change region_pending;
static void region_cache_sync(uint64_t current);
static _Atomic uint64_t region_epoch=2;
#ifdef WIN64_REGION_CACHE_TEST
static _Thread_local uint64_t region_fast_hits,region_slow_calls;
static _Thread_local int region_force_slow;
static _Thread_local void (*region_fast_probe)(void);
#endif
static region *region_index,**region_owners;
static size_t region_index_count,region_owner_capacity;
static module modules[MODULE_CAPACITY];
static size_t module_count, export_count;
static export_entry exports[EXPORT_CAPACITY];
static uint64_t image_base;
static struct { uint64_t base; size_t bytes; } images[MODULE_CAPACITY];
static size_t image_count;
static pthread_mutex_t metadata_mutex;
static pthread_once_t metadata_once=PTHREAD_ONCE_INIT;
static void metadata_initialize(void) {
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) || pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&metadata_mutex,&attributes)) abort();
    pthread_mutexattr_destroy(&attributes);
}
static void metadata_lock(void) { pthread_once(&metadata_once,metadata_initialize); pthread_mutex_lock(&metadata_mutex); }
static void metadata_unlock(void) { pthread_mutex_unlock(&metadata_mutex); }
static pthread_once_t pointer_cookie_once=PTHREAD_ONCE_INIT;
static uint64_t pointer_cookie;
static void initialize_pointer_cookie(void) {
    do {
        size_t done=0;
        while (done<sizeof(pointer_cookie)) {
            ssize_t count=getrandom((char *)&pointer_cookie+done,sizeof(pointer_cookie)-done,0);
            if (count<0 && errno==EINTR) continue;
            if (count<=0) abort();
            done+=(size_t)count;
        }
    } while (!pointer_cookie);
}
static uint64_t rotate_pointer(uint64_t p,unsigned bits) { return bits?(p>>bits)|(p<<(64-bits)):p; }
static _Thread_local uint32_t last_error;
static _Thread_local int crt_errno;
void win64_stdio_set_errno(int value) { crt_errno=value; }
static _Thread_local int crt_region_registered;
void win64_set_error(uint32_t value) { last_error=value; frostpunk_teb_write(0x68,value,4); }
uint32_t win64_get_error(void) { last_error=(uint32_t)frostpunk_teb_read(0x68,4); return last_error; }
/* -novideo is removed again at startup (see win64_init): it would also disable cinematics. */
/* Room for engine switches appended at startup (debug.fp.args, diagnostics only). */
/* -ultralow is the engine's own low-spec switch (UltraLowSpec, otherwise set only for Intel Iris
 * GPUs): it unlocks the hidden "Ultra low" level in the Graphics menu, and at that level the snow
 * system also takes its simplified path.  The other levels render the same with or without it. */
int win64_skip_intro=1;
static char command_line[192]="-gfxconfig -novideo -ultralow";
static char executable_name[]="C:\\Program Files (x86)\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe";
static char full_command_line[288]="\"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Frostpunk\\Frostpunk.exe\" -gfxconfig -novideo -ultralow";
static uint16_t executable_name_w[sizeof(executable_name)], full_command_line_w[sizeof(full_command_line)];
static int application_type, new_mode, argument_mode;
static int default_file_mode=0x4000, commit_mode;
static _Atomic uint64_t user_math_error;
uint64_t win64_math_error_handler(void) { return atomic_load_explicit(&user_math_error,memory_order_acquire); }
static uint64_t unhandled_exception_filter;
static uint64_t *exit_functions, tls_exit_callback;
static size_t exit_count,exit_capacity;
extern void frostpunk_android_exiting(uint32_t code) __attribute__((weak));

void win64_thread_crt_attach(void) {
    crt_errno=0; win64_locale_thread_attach();
    if (!crt_region_registered) { win64_region_add((uintptr_t)&crt_errno,sizeof(crt_errno),1); crt_region_registered=1; }
}
void win64_thread_crt_detach(void) {
    if (crt_region_registered) { win64_region_remove((uintptr_t)&crt_errno); crt_region_registered=0; }
}

static _Noreturn void invalid(const char *format, ...) {
    va_list ap; fputs("STOP Win64 ", stderr); va_start(ap, format);
    vfprintf(stderr, format, ap); va_end(ap); fputc('\n', stderr); fflush(stderr); _Exit(73);
}

/* The list/index is authoritative under metadata_mutex. Cached entries contain
 * only clipped attribute snapshots, never pointers to removable region records.
 * A writer publishes an odd epoch BEFORE changing physical memory/metadata and
 * an even epoch afterwards. Every caller releasing registered memory must either
 * remove its region first, or hold this boundary across the OS release.
 */
/* Journal and pending spans are accessed only while metadata_mutex is held.
 * Only a controlled top-level add/remove can prove the complete changed span.
 * Any nested boundary deliberately poisons that proof, including known calls. */
static void region_mutation_start(unsigned known,uint64_t low,uint64_t high) {
    metadata_lock();
    if(!region_mutation_depth++) {
        uint64_t current=atomic_load_explicit(&region_epoch,memory_order_acquire);
        if(current&1 || current>UINT64_MAX-2)invalid("region mutation epoch exhausted or inconsistent");
        region_cache_sync(current);
        region_pending=(region_change){.low=low,.high=high,.known=known};
        atomic_store_explicit(&region_epoch,current+1,memory_order_release);
        /* Publish exclusion before the following mprotect/free/metadata stores. */
        atomic_thread_fence(memory_order_seq_cst);
    } else region_pending.known=0;
}
void win64_region_mutation_begin(void) { region_mutation_start(0,0,0); }
static void region_mutation_span(uint64_t low,uint64_t high) {
    if(!region_pending.known)return;
    if(low>=high){region_pending.known=0;return;}
    if(region_pending.low>=region_pending.high){region_pending.low=low;region_pending.high=high;}
    else {if(low<region_pending.low)region_pending.low=low;if(high>region_pending.high)region_pending.high=high;}
}
void win64_region_mutation_end(void) {
    if(!region_mutation_depth)invalid("unbalanced region mutation boundary");
    if(!--region_mutation_depth) {
        uint64_t current=atomic_load_explicit(&region_epoch,memory_order_relaxed);
        if(!(current&1) || current==UINT64_MAX)invalid("region mutation epoch exhausted or inconsistent");
        region_pending.epoch=current+1;
        if(region_pending.low>=region_pending.high)region_pending.known=0;
        region_journal[((current+1)>>1)&(REGION_JOURNAL_CAPACITY-1)]=region_pending;
        if(!region_pending.known)region_cache_count=0;
        region_cache_epoch=current+1;
        atomic_store_explicit(&region_epoch,current+1,memory_order_release);
    }
    metadata_unlock();
}

/* Optional one-run telemetry. All access occurs under metadata_mutex;
 * no guest API, new allocation, runtime environment variable or FILE lock. */
#ifdef WIN64_REGION_PROFILE
static struct {
    uint64_t queries,hits,misses,one_byte,one_byte_misses;
    uint64_t linear_calls,linear_segments,linear_nodes,linear_max,insertions,evictions;
    uint64_t invalidate_calls,invalidated,protect_calls,protect_cleared;
    uint64_t remove_calls,remove_nodes,remove_max,remove_records,live,peak;
    uint64_t allocation_calls,allocation_nodes,allocation_max,forget_calls,forget_nodes,forget_max;
    uint64_t index_fast,index_gap,index_fallback,index_search_steps,index_insert_moves,index_remove_scans,index_rebuilds;
    uint64_t hit_positions[REGION_CACHE_CAPACITY];
} region_profile;
static void region_profile_emit(void) {
    if(region_profile.misses&((UINT64_C(1)<<16)-1))return;
    int saved_errno=errno;
    uint64_t first8=0;for(unsigned i=0;i<8&&i<REGION_CACHE_CAPACITY;++i)first8+=region_profile.hit_positions[i];
    char message[2048];
    int n=snprintf(message,sizeof(message),
        "REGION_PROFILE {\"capacity\":%u,\"queries\":%llu,\"hits\":%llu,\"misses\":%llu,\"hits_first8\":%llu,"
        "\"one_byte_queries\":%llu,\"one_byte_misses\":%llu,\"linear_calls\":%llu,\"linear_segments\":%llu,\"linear_nodes\":%llu,\"linear_max\":%llu,"
        "\"insertions\":%llu,\"evictions\":%llu,\"invalidate_calls\":%llu,\"invalidated\":%llu,\"protect_calls\":%llu,\"protect_cleared\":%llu,"
        "\"remove_calls\":%llu,\"remove_nodes\":%llu,\"remove_max\":%llu,\"remove_records\":%llu,\"live_regions\":%llu,\"peak_regions\":%llu,"
        "\"allocation_at_calls\":%llu,\"allocation_at_nodes\":%llu,\"allocation_at_max\":%llu,\"forget_calls\":%llu,\"forget_nodes\":%llu,\"forget_max\":%llu,"
        "\"index_fast\":%llu,\"index_gap\":%llu,\"index_fallback\":%llu,\"index_search_steps\":%llu,\"index_insert_moves\":%llu,\"index_remove_scans\":%llu,\"index_rebuilds\":%llu}\n",
        (unsigned)REGION_CACHE_CAPACITY,
        (unsigned long long)region_profile.queries,(unsigned long long)region_profile.hits,(unsigned long long)region_profile.misses,(unsigned long long)first8,
        (unsigned long long)region_profile.one_byte,(unsigned long long)region_profile.one_byte_misses,
        (unsigned long long)region_profile.linear_calls,(unsigned long long)region_profile.linear_segments,(unsigned long long)region_profile.linear_nodes,(unsigned long long)region_profile.linear_max,
        (unsigned long long)region_profile.insertions,(unsigned long long)region_profile.evictions,
        (unsigned long long)region_profile.invalidate_calls,(unsigned long long)region_profile.invalidated,
        (unsigned long long)region_profile.protect_calls,(unsigned long long)region_profile.protect_cleared,
        (unsigned long long)region_profile.remove_calls,(unsigned long long)region_profile.remove_nodes,(unsigned long long)region_profile.remove_max,
        (unsigned long long)region_profile.remove_records,(unsigned long long)region_profile.live,(unsigned long long)region_profile.peak,
        (unsigned long long)region_profile.allocation_calls,(unsigned long long)region_profile.allocation_nodes,(unsigned long long)region_profile.allocation_max,
        (unsigned long long)region_profile.forget_calls,(unsigned long long)region_profile.forget_nodes,(unsigned long long)region_profile.forget_max,
        (unsigned long long)region_profile.index_fast,(unsigned long long)region_profile.index_gap,(unsigned long long)region_profile.index_fallback,
        (unsigned long long)region_profile.index_search_steps,(unsigned long long)region_profile.index_insert_moves,
        (unsigned long long)region_profile.index_remove_scans,(unsigned long long)region_profile.index_rebuilds);
    if(n<=0){errno=saved_errno;return;}size_t bytes=(size_t)n<sizeof(message)?(size_t)n:sizeof(message)-1;
    for(size_t at=0;at<bytes;){ssize_t wrote=write(STDERR_FILENO,message+at,bytes-at);if(wrote>0)at+=(size_t)wrote;else if(wrote<0&&errno==EINTR)continue;else break;}
    errno=saved_errno;
}
#define REGION_PROFILE(...) do { __VA_ARGS__; } while(0)
#else
#define REGION_PROFILE(...) ((void)0)
#endif

/* Intrusive AVL address index: subtree maxima retain the old unique-coverage
 * proof. Ambiguous overlaps still use the authoritative newest-first list.
 * Owner buckets and previous links avoid scanning every live region on free.
 * All mutations hold metadata_mutex; pointer lookups allocate no storage. */
static size_t metadata_bucket(uint64_t key,size_t capacity) {
    key^=key>>30;key*=UINT64_C(0xbf58476d1ce4e5b9);key^=key>>27;
    key*=UINT64_C(0x94d049bb133111eb);key^=key>>31;return (size_t)key&(capacity-1);
}
static unsigned region_height(region *r) { return r?r->height:0; }
static uint64_t region_max_end(region *r) { return r?r->max_end:0; }
static void region_refresh(region *r) {
    unsigned left=region_height(r->left),right=region_height(r->right);
    r->height=1+(left>right?left:right);r->max_end=r->base+r->size;
    uint64_t end=region_max_end(r->left);if(end>r->max_end)r->max_end=end;
    end=region_max_end(r->right);if(end>r->max_end)r->max_end=end;
}
static region *region_rotate(region *r,int left) {
    region *top=left?r->right:r->left;
    if(left){r->right=top->left;top->left=r;}else{r->left=top->right;top->right=r;}
    region_refresh(r);region_refresh(top);return top;
}
static region *region_balance(region *r) {
    region_refresh(r);
    if(region_height(r->left)>region_height(r->right)+1){
        if(region_height(r->left->right)>region_height(r->left->left))r->left=region_rotate(r->left,1);
        return region_rotate(r,0);
    }
    if(region_height(r->right)>region_height(r->left)+1){
        if(region_height(r->right->left)>region_height(r->right->right))r->right=region_rotate(r->right,0);
        return region_rotate(r,1);
    }
    return r;
}
static int region_before(region *a,region *b) {
    return a->base<b->base || (a->base==b->base && (uintptr_t)a<(uintptr_t)b);
}
static region *region_tree_insert(region *root,region *record) {
    if(!root)return record;
    if(region_before(record,root))root->left=region_tree_insert(root->left,record);
    else root->right=region_tree_insert(root->right,record);
    return region_balance(root);
}
static region *region_tree_remove(region *root,region *record) {
    if(!root)invalid("region address index missing record");
    if(root==record){
        if(!root->left)return root->right;
        if(!root->right)return root->left;
        region *next=root->right;while(next->left)next=next->left;
        region *right=region_tree_remove(root->right,next);
        next->left=root->left;next->right=right;return region_balance(next);
    }
    if(region_before(record,root))root->left=region_tree_remove(root->left,record);
    else root->right=region_tree_remove(root->right,record);
    return region_balance(root);
}
static void region_index_insert(region *r) {
    r->left=r->right=NULL;r->height=1;r->max_end=r->base+r->size;
    region_index=region_tree_insert(region_index,r);++region_index_count;
}
static void region_index_remove(region *r) {
    region_index=region_tree_remove(region_index,r);--region_index_count;
}
static void region_owner_insert(region *record) {
    if(region_index_count>=region_owner_capacity){
        size_t capacity=region_owner_capacity?region_owner_capacity*2:64;
        if(capacity<region_owner_capacity || capacity>SIZE_MAX/sizeof(*region_owners))invalid("region owner index size");
        region **grown=calloc(capacity,sizeof(*grown));if(!grown)invalid("region owner index allocation");
        for(region *r=regions;r;r=r->next){size_t slot=metadata_bucket(r->owner,capacity);r->owner_next=grown[slot];grown[slot]=r;}
        free(region_owners);region_owners=grown;region_owner_capacity=capacity;
    }
    size_t slot=metadata_bucket(record->owner,region_owner_capacity);
    record->owner_next=region_owners[slot];region_owners[slot]=record;
}
static void region_link(region *r,region *previous,region *next) {
    region_owner_insert(r);region_index_insert(r);r->previous=previous;r->next=next;
    if(previous)previous->next=r;else regions=r;if(next)next->previous=r;
}

/* All cache reads and mutations share metadata_mutex with the region list.
 * Copies never retain pointers to removable/splittable region records. */
static void region_cache_invalidate(uint64_t base,size_t bytes) {
    REGION_PROFILE(++region_profile.invalidate_calls);
    for (size_t i=0;i<region_cache_count;) {
        region_snapshot *s=&region_cache[i];
        if (base<s->base+s->size && s->base<base+bytes) {
            REGION_PROFILE(++region_profile.invalidated);
            memmove(s,s+1,(region_cache_count-i-1)*sizeof(*s)); --region_cache_count;
        } else ++i;
    }
}

/* The caller holds metadata_mutex. Missing, odd, future or overwritten history
 * retains the old complete invalidation. Snapshots never retain node pointers.
 * shortcut: retain at most 16 proven disjoint mutations; use a larger history
 * only after a measured workload justifies its locked replay cost. */
static void region_cache_sync(uint64_t current) {
    uint64_t previous=region_cache_epoch;
    if(previous==current && !(current&1))return;
    if(!region_cache_count){region_cache_epoch=current;return;}
    if((previous|current)&1 || previous>current || (current-previous)/2>REGION_JOURNAL_CAPACITY)goto clear;
    unsigned steps=(unsigned)((current-previous)/2);
    for(unsigned i=1;i<=steps;++i){
        uint64_t epoch=previous+2*(uint64_t)i;
        region_change change=region_journal[(epoch>>1)&(REGION_JOURNAL_CAPACITY-1)];
        if(change.epoch!=epoch || !change.known || change.low>=change.high)goto clear;
        region_cache_invalidate(change.low,(size_t)(change.high-change.low));
    }
    region_cache_epoch=current;return;
clear:
    region_cache_count=0;region_cache_epoch=current;
}

void win64_region_add(uint64_t address, size_t bytes, int writable) {
    if (!bytes || address > UINT64_MAX - bytes) invalid("region geometry");
    region *r = malloc(sizeof(*r)); if (!r) invalid("region allocation");
    region_mutation_start(1,address,address+bytes); region_cache_invalidate(address,bytes);
    *r=(region){.base=address,.owner=address,.size=bytes,.protection=writable?4u:2u,.state=0x1000};
    region_link(r,NULL,regions);
    REGION_PROFILE(if(++region_profile.live>region_profile.peak)region_profile.peak=region_profile.live);
    win64_region_mutation_end();
}

void win64_region_remove(uint64_t address) {
    region_mutation_start(1,0,0);
    region **link=region_owner_capacity?&region_owners[metadata_bucket(address,region_owner_capacity)]:NULL;
    int removed=0;size_t profile_depth=0;(void)profile_depth;
    REGION_PROFILE(++region_profile.remove_calls);
    while(link && *link){
        region *r=*link;REGION_PROFILE(++profile_depth;++region_profile.remove_nodes);
        if(r->owner!=address){link=&r->owner_next;continue;}
        *link=r->owner_next;region_index_remove(r);
        if(r->previous)r->previous->next=r->next;else regions=r->next;
        if(r->next)r->next->previous=r->previous;
        region_mutation_span(r->base,r->base+r->size);
        region_cache_invalidate(r->base,r->size);free(r);removed=1;
        REGION_PROFILE(++region_profile.remove_records;--region_profile.live);
    }
    REGION_PROFILE(if(profile_depth>region_profile.remove_max)region_profile.remove_max=profile_depth);
    if(!removed)invalid("remove unknown region address=0x%" PRIx64,address);
    win64_region_mutation_end();
}

void win64_image_add(uint64_t address,size_t bytes) {
    if (!address || !bytes || address>UINT64_MAX-bytes) invalid("loaded image geometry");
    metadata_lock();
    for (size_t i=0;i<image_count;++i) {
        if (images[i].base==address && images[i].bytes==bytes) { metadata_unlock(); return; }
        if (address<images[i].base+images[i].bytes && images[i].base<address+bytes) invalid("loaded images overlap");
    }
    if (image_count==MODULE_CAPACITY) invalid("loaded image table exhausted");
    images[image_count].base=address; images[image_count++].bytes=bytes;
    metadata_unlock();
}

static uint64_t image_containing(uint64_t pc) {
    for (size_t i=0;i<image_count;++i)
        if (pc>=images[i].base && pc-images[i].base<images[i].bytes) return images[i].base;
    return 0;
}

static int region_allows(uint32_t state,uint32_t protection,int writing) {
    return !(writing!=-1 && state!=0x1000) &&
        !(writing>=0 && (protection==1 || (writing && !(protection&(4|8|0x40|0x80)))));
}

/* Caller holds metadata_mutex. Before the first covering record, every newer
 * interval is entirely left or right of p. Their nearest endpoints delimit
 * the exact segment in which this first-match record keeps its priority. */
static int accessible_linear(uint64_t p,size_t n,int writing,int remember) {
    uint64_t begin=p,end=p+n;size_t profile_depth=0;(void)profile_depth;
    REGION_PROFILE(++region_profile.linear_calls);
    while (p<end) {
        region *found=NULL;uint64_t left=0,right=UINT64_MAX;
        REGION_PROFILE(++region_profile.linear_segments);
        for (region *r=regions;r;r=r->next) {
            REGION_PROFILE(++profile_depth;++region_profile.linear_nodes);
            if (p>=r->base && p-r->base<r->size) { found=r; break; }
            uint64_t limit=r->base+r->size;
            if (limit<=p && limit>left) left=limit;
            if (r->base>p && r->base<right) right=r->base;
        }
        REGION_PROFILE(if(profile_depth>region_profile.linear_max)region_profile.linear_max=profile_depth);
        if (!found) return 0;
        uint64_t next=found->base+found->size;
        if (right<next) next=right;
        if (remember && p==begin && end<=next) {
            uint64_t low=left>found->base?left:found->base;
            REGION_PROFILE(++region_profile.insertions;if(region_cache_count==REGION_CACHE_CAPACITY)++region_profile.evictions);
            size_t moved=region_cache_count<REGION_CACHE_CAPACITY?region_cache_count++:REGION_CACHE_CAPACITY-1;
            memmove(region_cache+1,region_cache,moved*sizeof(*region_cache));
            region_cache[0]=(region_snapshot){low,(size_t)(next-low),found->protection,found->state};
        }
        if (!region_allows(found->state,found->protection,writing)) return 0;
        p=next<end?next:end;
    }
    return 1;
}

static int accessible_indexed(uint64_t p,size_t n,int writing,int remember) {
    uint64_t begin=p,end=p+n;
    while(p<end){
        region *node=region_index,*r=NULL;uint64_t prefix=0,previous=0,next_base=UINT64_MAX;
        while(node){
            REGION_PROFILE(++region_profile.index_search_steps);
            if(node->base<=p){
                uint64_t left=region_max_end(node->left);if(left>prefix)prefix=left;
                r=node;previous=prefix;uint64_t limit=node->base+node->size;if(limit>prefix)prefix=limit;
                node=node->right;
            }else{next_base=node->base;node=node->left;}
        }
        if(!r){REGION_PROFILE(++region_profile.index_gap);return 0;}
        if(previous>p){REGION_PROFILE(++region_profile.index_fallback);return accessible_linear(p,(size_t)(end-p),writing,remember&&p==begin);}
        if(p-r->base>=r->size){REGION_PROFILE(++region_profile.index_gap);return 0;}
        uint64_t next=r->base+r->size;if(next_base<next)next=next_base;
        REGION_PROFILE(++region_profile.index_fast);
        if(remember&&p==begin&&end<=next){
            uint64_t left=previous>r->base?previous:r->base;
            REGION_PROFILE(++region_profile.insertions;if(region_cache_count==REGION_CACHE_CAPACITY)++region_profile.evictions);
            size_t moved=region_cache_count<REGION_CACHE_CAPACITY?region_cache_count++:REGION_CACHE_CAPACITY-1;
            memmove(region_cache+1,region_cache,moved*sizeof(*region_cache));
            region_cache[0]=(region_snapshot){left,(size_t)(next-left),r->protection,r->state};
        }
        if(!region_allows(r->state,r->protection,writing))return 0;
        p=next<end?next:end;
    }
    return 1;
}

static int accessible(uint64_t p, size_t n, int writing) {
    if (!n) return 1;
    if (!p || p > UINT64_MAX - n) return 0;
    /* Profile counters remain protected by the mutex. Release builds avoid the
     * shared lock on a live TLS hit; an epoch race repeats the old locked proof.
     * This validates access, it does not pin an allocation against a subsequent
     * guest data race (the old pointer-returning interface has the same limit).
     */
#ifndef WIN64_REGION_PROFILE
    uint64_t version=atomic_load_explicit(&region_epoch,memory_order_acquire);
    if(!(version&1) && region_cache_epoch==version
#ifdef WIN64_REGION_CACHE_TEST
       && !region_force_slow
#endif
    ) {
        for(size_t i=0;i<region_cache_count;++i) {
            region_snapshot s=region_cache[i];
            if(p>=s.base && p-s.base<s.size && n<=s.size-(size_t)(p-s.base)) {
                int allowed=region_allows(s.state,s.protection,writing);
#ifdef WIN64_REGION_CACHE_TEST
                if(region_fast_probe)region_fast_probe();
#endif
                if(atomic_load_explicit(&region_epoch,memory_order_acquire)==version) {
                    memmove(region_cache+1,region_cache,i*sizeof(s));region_cache[0]=s;
#ifdef WIN64_REGION_CACHE_TEST
                    ++region_fast_hits;
#endif
                    return allowed;
                }
                break;
            }
        }
    }
#endif
    metadata_lock();
    uint64_t current=atomic_load_explicit(&region_epoch,memory_order_acquire);
    region_cache_sync(current);
#ifdef WIN64_REGION_CACHE_TEST
    ++region_slow_calls;
#endif
    REGION_PROFILE(++region_profile.queries;region_profile.one_byte+=n==1);
    for (size_t i=0;i<region_cache_count;++i) {
        region_snapshot s=region_cache[i];
        if (p>=s.base && p-s.base<s.size && n<=s.size-(size_t)(p-s.base)) {
            memmove(region_cache+1,region_cache,i*sizeof(s)); region_cache[0]=s;
            REGION_PROFILE(++region_profile.hits;++region_profile.hit_positions[i]);
            int allowed=region_allows(s.state,s.protection,writing);
            metadata_unlock(); return allowed;
        }
    }
    REGION_PROFILE(++region_profile.misses;region_profile.one_byte_misses+=n==1);
    int allowed=accessible_indexed(p,n,writing,1);REGION_PROFILE(region_profile_emit());metadata_unlock();return allowed;
}

static void protection_set(uint64_t begin,size_t bytes,uint32_t value,uint32_t state) {
    /* Virtual* callers hold metadata_mutex across host and guest state changes. */
    win64_region_mutation_begin();
    REGION_PROFILE(++region_profile.protect_calls;region_profile.protect_cleared+=region_cache_count);
    region_cache_count=0;uint64_t end=begin+bytes;
    for(region *r=regions,*next;r;r=next){
        next=r->next;uint64_t old_end=r->base+r->size;
        if(begin>=old_end || end<=r->base)continue;
        uint64_t low=begin>r->base?begin:r->base,high=end<old_end?end:old_end;
        region_index_remove(r);
        if(low>r->base){
            region *left=malloc(sizeof(*left));if(!left)invalid("protection region split");
            *left=*r;left->size=(size_t)(low-r->base);region_link(left,r->previous,r);
            REGION_PROFILE(if(++region_profile.live>region_profile.peak)region_profile.peak=region_profile.live);
        }
        if(high<old_end){
            region *right=malloc(sizeof(*right));if(!right)invalid("protection region split");
            *right=*r;right->base=high;right->size=(size_t)(old_end-high);region_link(right,r,r->next);
            REGION_PROFILE(if(++region_profile.live>region_profile.peak)region_profile.peak=region_profile.live);
        }
        r->base=low;r->size=(size_t)(high-low);r->protection=value;if(state)r->state=state;
        region_index_insert(r);
    }
    win64_region_mutation_end();
}

static int host_protection(uint32_t guest) {
    switch (guest) { case 1:return PROT_NONE; case 2:case 0x10:case 0x20:return PROT_READ;
        case 4:case 8:case 0x40:case 0x80:return PROT_READ|PROT_WRITE; default:return -1; }
}

/* Guest pages are 4 KB. A larger host page (16 KB) that also holds guest pages outside
 * [low,high) is shared: it is never made less accessible than read/write data, and a
 * decommit zeroes only the guest bytes. With 4 KB host pages nothing is shared. */
#define GUEST_PAGE UINT64_C(4096)
static void host_split(uint64_t low,uint64_t high,uint64_t *inner,uint64_t *outer,uint64_t shared[2]) {
    uint64_t page=(uint64_t)sysconf(_SC_PAGESIZE);
    *inner=(low+page-1)&~(page-1); *outer=high&~(page-1);
    shared[0]=low<*inner?*inner-page:UINT64_MAX;
    shared[1]=*outer<high && *outer>=*inner?*outer:UINT64_MAX;
}
static int host_protect(uint64_t low,uint64_t high,int prot) {
    uint64_t inner,outer,shared[2],page=(uint64_t)sysconf(_SC_PAGESIZE); host_split(low,high,&inner,&outer,shared);
    if (inner<outer && mprotect((void *)(uintptr_t)inner,(size_t)(outer-inner),prot)) return 0;
    for (int i=0;i<2;++i) if (shared[i]!=UINT64_MAX && prot!=PROT_NONE &&
        mprotect((void *)(uintptr_t)shared[i],(size_t)page,PROT_READ|PROT_WRITE)) return 0;
    return 1;
}
static int host_decommit(uint64_t low,uint64_t high) {
    uint64_t inner,outer,shared[2],page=(uint64_t)sysconf(_SC_PAGESIZE); host_split(low,high,&inner,&outer,shared);
    if (inner<outer && (mprotect((void *)(uintptr_t)inner,(size_t)(outer-inner),PROT_NONE) ||
        madvise((void *)(uintptr_t)inner,(size_t)(outer-inner),MADV_DONTNEED))) return 0;
    for (int i=0;i<2;++i) {
        if (shared[i]==UINT64_MAX) continue;
        if (mprotect((void *)(uintptr_t)shared[i],(size_t)page,PROT_READ|PROT_WRITE)) return 0;
        uint64_t from=low>shared[i]?low:shared[i],to=high<shared[i]+page?high:shared[i]+page;
        memset((void *)(uintptr_t)from,0,(size_t)(to-from));
    }
    return 1;
}

static _Unwind_Reason_Code buffer_failure_frame(struct _Unwind_Context *context,void *argument) {
    unsigned *frames=argument;
    uintptr_t pc=(uintptr_t)_Unwind_GetIP(context);
    if (!pc || *frames==16) return _URC_END_OF_STACK;
    Dl_info info={0};int known=dladdr((void *)pc,&info);
    uintptr_t base=known?(uintptr_t)info.dli_fbase:0;
    fprintf(stderr,"WIN64_BUFFER_FRAME index=%u pc=0x%" PRIxPTR " module=%s base=0x%" PRIxPTR " offset=0x%" PRIxPTR "\n",
        (*frames)++,pc,known&&info.dli_fname?info.dli_fname:"?",base,known?pc-base:0);
    return _URC_NO_REASON;
}

void *win64_pointer(uint64_t p, size_t n, int writing) {
#ifndef FP_CHECKED_GUEST_POINTERS
    (void)writing;
    if (!p && n) invalid("NULL guest buffer bytes=%zu",n);
    return (void *)(uintptr_t)p;
#endif
    if (!accessible(p,n,writing)) {
        unsigned frames=0;
        _Unwind_Backtrace(buffer_failure_frame,&frames);
        invalid("invalid guest buffer address=0x%" PRIx64 " bytes=%zu write=%d",p,n,writing);
    }
    return (void *)(uintptr_t)p;
}

const char *win64_string_a(uint64_t p) {
#ifndef FP_CHECKED_GUEST_POINTERS
    if (!p) invalid("NULL guest string");
    return (const char *)(uintptr_t)p;
#endif
    for (size_t n=0;n<32768;++n) {
        if (!accessible(p+n,1,0)) invalid("unterminated guest string at 0x%" PRIx64,p);
        if (!*(const char *)(uintptr_t)(p+n)) return (const char *)(uintptr_t)p;
    }
    invalid("guest string exceeds 32767 bytes");
}

static void string_w_ascii(uint64_t p, char *out, size_t capacity) {
    size_t n=0;
    while (n+1<capacity) {
        uint16_t ch; memcpy(&ch,win64_pointer(p+n*2,2,0),2);
        if (ch>127) invalid("non-ASCII module name conversion is not implemented");
        out[n++]=(char)ch;
        if (!ch) return;
    }
    invalid("module name too long");
}

static void canonical(const char *name, char out[128]) {
    const char *base=name;
    for (const char *p=name;*p;++p) if (*p=='/' || *p=='\\') base=p+1;
    size_t n=strlen(base); if (!n || n>=123) invalid("invalid module basename");
    for (size_t i=0;i<=n;++i) out[i]=(char)tolower((unsigned char)base[i]);
    if (!strchr(out,'.')) strcat(out,".dll");
}

static module *module_name(const char *name) {
    char key[128]; canonical(name,key);
    for (size_t i=0;i<module_count;++i) if (!strcmp(key,modules[i].name)) return &modules[i];
    return NULL;
}

static module *module_handle(uint64_t handle) {
    for (size_t i=0;i<module_count;++i) if (modules[i].handle==handle && modules[i].refs) return &modules[i];
    return NULL;
}

static module *module_add(const char *name, int pinned) {
    module *m=module_name(name);
    if (m) { ++m->refs; return m; }
    if (module_count==MODULE_CAPACITY) invalid("module table exhausted");
    m=&modules[module_count++]; canonical(name,m->name);
    /* The HLE module handle identifies its actual provider record. */
    m->handle=(uint64_t)(uintptr_t)m; m->refs=1; m->pinned=pinned;
    return m;
}

static int export_known(module *m, const char *name) {
    /* Android motion already feeds the window queue; the separate Win32 touch
     * API is unavailable. SDL checks these optional exports before using it. */
    if (!strcmp(m->name,"user32.dll") && (!strcmp(name,"RegisterTouchWindow") ||
        !strcmp(name,"GetTouchInputInfo") || !strcmp(name,"CloseTouchInputHandle"))) return 0;
    for (size_t i=0;i<sizeof(win64_export_names)/sizeof(win64_export_names[0]);++i)
        if (!strcmp(m->name,win64_export_names[i].dll) && !strcmp(name,win64_export_names[i].name)) return 1;
    for (unsigned i=0;i<HLE_COUNT;++i)
        if (!strcasecmp(m->name,recomp_imports[i].dll) && !strcmp(name,recomp_imports[i].name)) return 1;
    return 0;
}

static uint64_t get_export(module *m, const char *name) {
    if (!export_known(m,name)) { win64_set_error(127); return 0; }
    for (size_t i=0;i<export_count;++i)
        if (exports[i].owner==m && !strcmp(exports[i].name,name)) return EXPORT_BASE+i*8;
    if (export_count==EXPORT_CAPACITY) invalid("export table exhausted");
    char *copy=strdup(name); if (!copy) invalid("export allocation");
    exports[export_count]=(export_entry){m,copy};
    return EXPORT_BASE+export_count++*8;
}

typedef struct allocation { void *p; size_t size; int mapped; uint32_t protection; uint64_t heap; size_t alignment; struct allocation *next,*previous,*hash_next; } allocation;
static allocation *allocations,**allocation_index;
static size_t allocation_count,allocation_capacity;
static const uint64_t process_heap_identity=UINT64_C(0x46524f5354484541);

static allocation *allocation_at(uint64_t p) {
    metadata_lock();size_t profile_depth=0;(void)profile_depth;REGION_PROFILE(++region_profile.allocation_calls);
    allocation *a=allocation_capacity?allocation_index[metadata_bucket(p,allocation_capacity)]:NULL;
    while(a){
        REGION_PROFILE(++profile_depth;++region_profile.allocation_nodes;if(profile_depth>region_profile.allocation_max)region_profile.allocation_max=profile_depth);
        if((uint64_t)(uintptr_t)a->p==p)break;a=a->hash_next;
    }
    metadata_unlock();return a;
}

static void allocation_index_insert(allocation *record) {
    if(allocation_count>=allocation_capacity){
        size_t capacity=allocation_capacity?allocation_capacity*2:64;
        if(capacity<allocation_capacity || capacity>SIZE_MAX/sizeof(*allocation_index))invalid("allocation index size");
        allocation **grown=calloc(capacity,sizeof(*grown));if(!grown)invalid("allocation index allocation");
        for(allocation *a=allocations;a;a=a->next){size_t slot=metadata_bucket((uintptr_t)a->p,capacity);a->hash_next=grown[slot];grown[slot]=a;}
        free(allocation_index);allocation_index=grown;allocation_capacity=capacity;
    }
    size_t slot=metadata_bucket((uintptr_t)record->p,allocation_capacity);
    record->hash_next=allocation_index[slot];allocation_index[slot]=record;++allocation_count;
}

static allocation *allocation_containing(uint64_t p,size_t bytes) {
    for (allocation *a=allocations;a;a=a->next) {
        uint64_t base=(uintptr_t)a->p;
        if (p>=base && p-base<=a->size && bytes<=a->size-(size_t)(p-base)) return a;
    }
    return NULL;
}

static void remember_allocation(void *p, size_t size, int mapped, uint32_t protection) {
    allocation *a=malloc(sizeof(*a)); if (!a) invalid("allocation metadata");
    metadata_lock();
    *a=(allocation){.p=p,.size=size,.mapped=mapped,.protection=protection,.next=allocations};
    allocation_index_insert(a);if(allocations)allocations->previous=a;allocations=a;
    win64_region_add((uint64_t)(uintptr_t)p,size,1);
    metadata_unlock();
}

static void forget_allocation(allocation *a) {
    size_t profile_depth=1;(void)profile_depth;
    allocation **link=&allocation_index[metadata_bucket((uintptr_t)a->p,allocation_capacity)];
    while(*link!=a){REGION_PROFILE(++profile_depth);link=&(*link)->hash_next;}*link=a->hash_next;--allocation_count;
    if(a->previous)a->previous->next=a->next;else allocations=a->next;
    if(a->next)a->next->previous=a->previous;
    REGION_PROFILE(++region_profile.forget_calls;region_profile.forget_nodes+=profile_depth;if(profile_depth>region_profile.forget_max)region_profile.forget_max=profile_depth);
    win64_region_remove((uint64_t)(uintptr_t)a->p);
    free(a);
}

/* Guest heap blocks carry a 16-byte header with the exact size, the CRT
 * alignment flag, the HeapAlloc owner flag and the offset back to the host
 * block. Lookups need no global index or lock; mapped Virtual* memory keeps
 * the allocation list above. */
#define HEAP_MAGIC UINT32_C(0x46504831)
typedef struct { uint64_t size; uint32_t magic; uint16_t offset; uint8_t aligned,heap; } heap_header;
_Static_assert(sizeof(heap_header)==16,"heap header size");

static heap_header *heap_block(uint64_t p) {
    if (!p || (p&15)) return NULL;
    heap_header *h=(heap_header *)(uintptr_t)(p-sizeof(heap_header));
    return h->magic==HEAP_MAGIC?h:NULL;
}

static uint64_t heap_allocate(size_t bytes,size_t alignment,int zero) {
    size_t count=bytes?bytes:1,align=alignment>16?alignment:16;
    if (align>32768) invalid("unsupported allocation alignment=%zu",alignment);
    if (count>SIZE_MAX-align-32) return 0;
    size_t total=count+align+16;
    unsigned char *raw=zero?calloc(1,total):malloc(total);
    if (!raw) return 0;
    uintptr_t p=((uintptr_t)raw+16+align-1)&~(uintptr_t)(align-1);
    heap_header *h=(heap_header *)(p-16);
    h->size=count; h->magic=HEAP_MAGIC; h->offset=(uint16_t)(p-(uintptr_t)raw);
    h->aligned=alignment!=0; h->heap=0;
    return (uint64_t)p;
}

uint64_t win64_alloc(size_t n, int zero) {
    uint64_t p=heap_allocate(n,0,zero);
    if (!p) win64_set_error(8);
    return p;
}

void win64_free(uint64_t p) {
    if (!p) return;
    heap_header *h=heap_block(p);
    if (!h) invalid("invalid native HLE allocation free address=0x%" PRIx64,p);
    h->magic=0;
    free((unsigned char *)(uintptr_t)p-h->offset);
}

uint64_t win64_argument(CPU *c,unsigned i) {
    switch (i) { case 0: return c->rcx; case 1: return c->rdx; case 2: return c->r8; case 3: return c->r9; default: break; }
    uint64_t value; memcpy(&value,win64_pointer(c->rsp+40+(i-4)*8,8,0),8); return value;
}

static int is_crt(const char *dll) { return !strcasecmp(dll,"msvcrt.dll") || !strcasecmp(dll,"ucrtbase.dll") ||
    !strncasecmp(dll,"api-ms-win-crt-",15) || !strcasecmp(dll,"vcruntime140.dll"); }

static uint64_t crt_allocate(size_t bytes,size_t alignment) {
    if (!alignment) return win64_alloc(bytes,0);
    if (alignment&(alignment-1)) invalid("invalid aligned allocation alignment");
    uint64_t p=heap_allocate(bytes,alignment,0); if (!p) crt_errno=12;
    return p;
}

static uint64_t crt_resize(uint64_t old_address,size_t bytes,size_t alignment) {
    heap_header *old=old_address?heap_block(old_address):NULL;
    if (old_address && (!old || old->heap || (!!old->aligned)!=(!!alignment))) invalid("invalid CRT reallocation");
    if (old_address && !bytes) { win64_free(old_address); return 0; }
    uint64_t replacement=crt_allocate(bytes,alignment); if (!replacement) return 0;
    if (old) { memcpy((void *)(uintptr_t)replacement,(const void *)(uintptr_t)old_address,old->size<bytes?old->size:bytes); win64_free(old_address); }
    return replacement;
}

static int fast_malloc(CPU *c) { c->rax=win64_alloc((size_t)c->rcx,0); if (!c->rax) crt_errno=12; return 1; }
static int fast_calloc(CPU *c) {
    uint64_t a=c->rcx,b=c->rdx;
    if (b && a>SIZE_MAX/b) { crt_errno=12; c->rax=0; return 1; }
    c->rax=win64_alloc((size_t)(a*b),1); if (!c->rax) crt_errno=12; return 1;
}
static int fast_free(CPU *c) { win64_free(c->rcx); return 1; }
static int fast_realloc(CPU *c) { c->rax=crt_resize(c->rcx,(size_t)c->rdx,0); return 1; }
static int fast_aligned_malloc(CPU *c) {
    if (!c->rdx) invalid("zero _aligned_malloc alignment");
    c->rax=crt_allocate((size_t)c->rcx,(size_t)c->rdx); return 1;
}
static int fast_aligned_realloc(CPU *c) {
    uint64_t d=c->r8; if (!d || (d&(d-1))) invalid("invalid _aligned_realloc alignment");
    c->rax=crt_resize(c->rcx,(size_t)c->rdx,(size_t)d); return 1;
}
static int fast_aligned_free(CPU *c) {
    if (c->rcx) { heap_header *h=heap_block(c->rcx); if (!h || !h->aligned) invalid("invalid _aligned_free"); win64_free(c->rcx); }
    return 1;
}
static int (*heap_fast_import(const char *name))(CPU *) {
    if (!strcmp(name,"malloc")) return fast_malloc;
    if (!strcmp(name,"calloc")) return fast_calloc;
    if (!strcmp(name,"free")) return fast_free;
    if (!strcmp(name,"realloc")) return fast_realloc;
    if (!strcmp(name,"_aligned_malloc")) return fast_aligned_malloc;
    if (!strcmp(name,"_aligned_realloc")) return fast_aligned_realloc;
    if (!strcmp(name,"_aligned_free")) return fast_aligned_free;
    return NULL;
}

static pthread_once_t crt_exit_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t crt_exit_mutex;
static void crt_exit_lock_init(void) {
    pthread_mutexattr_t attr;
    if(pthread_mutexattr_init(&attr)||pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE)||pthread_mutex_init(&crt_exit_mutex,&attr))
        invalid("CRT exit mutex initialization");
    pthread_mutexattr_destroy(&attr);
}
static void crt_exit_enter(void){pthread_once(&crt_exit_once,crt_exit_lock_init);pthread_mutex_lock(&crt_exit_mutex);}
static void crt_exit_leave(void){pthread_mutex_unlock(&crt_exit_mutex);}

static int crt_atexit_register(uint64_t function) {
    metadata_lock();
    if (exit_count==exit_capacity) {
        if (exit_capacity>SIZE_MAX/(2*sizeof(*exit_functions))) { metadata_unlock(); return -1; }
        size_t next=exit_capacity?exit_capacity*2:32;
        uint64_t *grown=realloc(exit_functions,next*sizeof(*grown));
        if (!grown) { metadata_unlock(); return -1; }
        exit_functions=grown; exit_capacity=next;
    }
    exit_functions[exit_count++]=function;
    metadata_unlock(); return 0;
}

static int initialize_onexit_table(uint64_t address) {
    if (!address) return -1;
    metadata_lock();
    uint64_t *table=win64_pointer(address,24,1);
    /* The HLE owns plain-pointer tables. Native UCRT uses its private encoded
     * pointers, but likewise preserves any table whose first != end. */
    if (table[0]==table[2]) memset(table,0,24);
    metadata_unlock(); return 0;
}

static int register_onexit_function(uint64_t address,uint64_t function) {
    if (!address) return -1;
    metadata_lock();
    uint64_t *table=win64_pointer(address,24,1);
    if (table[1]<table[0] || table[2]<table[1] || ((table[1]-table[0])&7) || ((table[2]-table[0])&7)) invalid("invalid onexit table geometry");
    size_t count=(size_t)((table[1]-table[0])/8),capacity=(size_t)((table[2]-table[0])/8);
    if (count>1048576 || capacity>1048576) invalid("invalid onexit table capacity");
    if (count==capacity) {
        size_t next=capacity?capacity*2:32; uint64_t replacement=win64_alloc(next*8,0);
        if (!replacement) { metadata_unlock(); return -1; }
        if (count) memcpy(win64_pointer(replacement,count*8,1),win64_pointer(table[0],count*8,0),count*8);
        win64_free(table[0]); table[0]=replacement; table[1]=replacement+count*8; table[2]=replacement+next*8;
    }
    memcpy(win64_pointer(table[1],8,1),&function,8); table[1]+=8;
    metadata_unlock(); return 0;
}

static void crt_exit_callback(CPU *c,uint64_t function,int tls) {
    CPU saved=*c;uint64_t frame=(c->rsp-32)&~UINT64_C(15);
    win64_pointer(frame-8,(size_t)(c->rsp-frame+8),1);c->rsp=frame;
    if(tls){c->rcx=0;c->rdx=0;c->r8=0;}
    push64(c,UINT64_C(0xe5ef0000));dispatch(c,function);
    if(c->rsp!=frame)invalid("CRT exit callback stack imbalance function=0x%" PRIx64,function);
    *c=saved;
}

static int execute_onexit_table(CPU *c,uint64_t address) {
    if(!address)return -1;
    crt_exit_enter();
    metadata_lock();uint64_t *table=win64_pointer(address,24,1);
    uint64_t first=table[0],last=table[1],cursor=last;
    for(;;){
        if(!first||first==UINT64_MAX)break;
        if(last<first||table[2]<last||((last-first)&7)||((table[2]-first)&7)||(table[2]-first)/8>1048576)
            invalid("invalid executing onexit table geometry");
        uint64_t function=0;
        while(cursor>first){cursor-=8;uint64_t *slot=win64_pointer(cursor,8,1);function=*slot;*slot=0;if(function)break;}
        if(!function){win64_free(first);memset(table,0,24);break;}
        metadata_unlock();crt_exit_callback(c,function,0);metadata_lock();
        table=win64_pointer(address,24,1);
        if(first!=table[0]||last!=table[1]){first=table[0];last=table[1];cursor=last;}
    }
    metadata_unlock();crt_exit_leave();return 0;
}

static void crt_exit_cleanup(CPU *c) {
    crt_exit_enter();
    metadata_lock();uint64_t tls=tls_exit_callback;tls_exit_callback=0;metadata_unlock();
    if(tls)crt_exit_callback(c,tls,1);
    for(;;){
        metadata_lock();if(!exit_count){metadata_unlock();break;}uint64_t function=exit_functions[--exit_count];
        metadata_unlock();if(function)crt_exit_callback(c,function,0);
    }
    if(fflush(NULL))fprintf(stderr,"CRT_EXIT flush_failed errno=%d\n",errno);
    crt_exit_leave();
}

static int execute(CPU *c,const char *dll,const char *name) {
    uint64_t a=c->rcx,b=c->rdx,d=c->r8,e=c->r9;
    if (!strcasecmp(dll,"xinput9_1_0.dll") && (!strcmp(name,"XInputGetState") || !strcmp(name,"XInputSetState"))) {
        /* Native 9.1.0 rejects invalid index/NULL; no controller touches no buffer. */
        if((uint32_t)a<4 && b){win64_set_error(0);c->rax=1167;}else c->rax=160;return 1;
    }
    if ((!strcasecmp(dll,"kernel32.dll") || !strcasecmp(dll,"kernelbase.dll") || !strcasecmp(dll,"ntdll.dll")) &&
        !strcmp(name,"RtlPcToFileHeader")) {
        c->rax=image_containing(a); memcpy(win64_pointer(b,8,1),&c->rax,8); return 1;
    }
    if ((!strcasecmp(dll,"advapi32.dll") || !strcasecmp(dll,"kernel32.dll") || !strcasecmp(dll,"kernelbase.dll")) &&
        !strcmp(name,"RegOpenKeyExW")) {
        /* shortcut: Only the absent Logitech SDK registration is modeled; extend on a proven caller. */
        int host_errno=errno;char subkey[128];
        if(a!=UINT64_C(0xffffffff80000002) || (uint32_t)d || (uint32_t)e!=0x20019)return 0;
        string_w_ascii(b,subkey,sizeof(subkey));
        if(strcasecmp(subkey,"SOFTWARE\\Classes\\CLSID\\{A6519E67-7632-4375-AFDF-CAA889744403}\\ServerBinary"))return 0;
        uint64_t output=win64_argument(c,4),empty=0;if(!output)return 0;
        memcpy(win64_pointer(output,8,1),&empty,8);c->rax=2;errno=host_errno;return 1;
    }
    if (!strcasecmp(dll,"kernel32.dll") || !strcasecmp(dll,"kernelbase.dll")) {
        if (!strcmp(name,"GetModuleHandleA") || !strcmp(name,"GetModuleHandleW") ||
            !strcmp(name,"LoadLibraryA") || !strcmp(name,"LoadLibraryW")) {
            int host_errno=errno;
            if (!a) { c->rax=image_base; return 1; }
            char wide[128]; const char *key;
            if (name[strlen(name)-1]=='W') { string_w_ascii(a,wide,sizeof(wide)); key=wide; } else key=win64_string_a(a);
            char module_key[128]; canonical(key,module_key);
            if (!strcmp(module_key,"frostpunk.exe")) { c->rax=image_base; return 1; }
            /* shortcut: Optional Windows providers stay unavailable until a native backend exists. */
            if (!strcmp(module_key,"lightfx.dll") || !strcmp(module_key,"rzchromasdk64.dll") || !strcmp(module_key,"imm32.dll")) {
                c->rax=0; win64_set_error(126); errno=host_errno; return 1;
            }
            module *m=module_name(key); int newly_loaded=0;
            if ((!m || !m->refs) && !strncmp(name,"LoadLibrary",11)) {
                /* Only implemented HLE providers can be loaded at this boundary. */
                char normalized[128]; canonical(key,normalized);
                if (!strcmp(normalized,"msvcrt.dll") || !strcmp(normalized,"advapi32.dll") || !strcmp(normalized,"shell32.dll") ||
                    !strcmp(normalized,"d3d11.dll") || !strcmp(normalized,"dxgi.dll") || !strcmp(normalized,"shcore.dll")) { m=module_add(normalized,0); newly_loaded=1; }
                else { fprintf(stderr,"STOP unimplemented module load %s\n",normalized); fflush(stderr); _Exit(70); }
            }
            if (!m || !m->refs) { win64_set_error(126); c->rax=0; }
            else { if (!strncmp(name,"LoadLibrary",11) && !newly_loaded) ++m->refs; c->rax=m->handle; }
            fprintf(stderr,"HLE %s name=%s result=0x%" PRIx64 "\n",name,key,c->rax); return 1;
        }
        if (!strcmp(name,"GetProcAddress")) {
            module *m=module_handle(a); if (!m) { win64_set_error(6); c->rax=0; return 1; }
            if (b<=0xffff) { win64_set_error(127); c->rax=0; return 1; }
            const char *symbol=win64_string_a(b); c->rax=get_export(m,symbol);
            fprintf(stderr,"HLE GetProcAddress %s!%s result=0x%" PRIx64 "\n",m->name,symbol,c->rax); return 1;
        }
        if (!strcmp(name,"FreeLibrary")) {
            module *m=module_handle(a); if (!m) { win64_set_error(6); c->rax=0; }
            else { if (!m->pinned) --m->refs; c->rax=1; } return 1;
        }
        if (!strcmp(name,"GetLastError")) { c->rax=win64_get_error(); return 1; }
        if (!strcmp(name,"GetCommandLineA")) { c->rax=(uintptr_t)full_command_line; return 1; }
        if (!strcmp(name,"GetCommandLineW")) { c->rax=(uintptr_t)full_command_line_w; return 1; }
        if (!strcmp(name,"GetModuleFileNameA") || !strcmp(name,"GetModuleFileNameW")) {
            if (a && a!=image_base) { win64_set_error(126); c->rax=0; return 1; }
            if (!(uint32_t)d) { win64_set_error(122); c->rax=0; return 1; }
            size_t length=strlen(executable_name),capacity=(uint32_t)d,count=length<capacity?length:capacity-1;
            int wide=name[strlen(name)-1]=='W'; void *out=win64_pointer(b,(count+1)*(wide?2:1),1);
            memcpy(out,wide?(const void *)executable_name_w:(const void *)executable_name,count*(wide?2:1));
            memset((char *)out+count*(wide?2:1),0,wide?2:1);
            c->rax=length>=capacity?capacity:length; if (length>=capacity) win64_set_error(122); return 1;
        }
        if (!strcmp(name,"SetLastError")) { win64_set_error((uint32_t)a); return 1; }
        if (!strcmp(name,"SetUnhandledExceptionFilter")) {
            c->rax=unhandled_exception_filter; unhandled_exception_filter=a; return 1;
        }
        if (!strcmp(name,"GetCurrentProcessId")) { c->rax=(uint32_t)getpid(); return 1; }
        if (!strcmp(name,"GetCurrentThreadId")) { c->rax=(uint32_t)gettid(); return 1; }
        if (!strcmp(name,"GetCurrentProcess")) { c->rax=UINT64_MAX; return 1; }
        if (!strcmp(name,"GetCurrentThread")) { c->rax=UINT64_MAX-1; return 1; }
        if (!strcmp(name,"EncodePointer") || !strcmp(name,"DecodePointer")) {
            pthread_once(&pointer_cookie_once,initialize_pointer_cookie);
            unsigned rotation=(unsigned)(pointer_cookie&63);
            c->rax=!strcmp(name,"EncodePointer")?rotate_pointer(a^pointer_cookie,rotation):
                rotate_pointer(a,(64-rotation)&63)^pointer_cookie; return 1;
        }
        if (!strcmp(name,"InitializeSListHead")) {
            if (a&15) invalid("unaligned SLIST_HEADER");
            memset(win64_pointer(a,16,1),0,16); return 1;
        }
        if (!strcmp(name,"GetProcessHeap")) { c->rax=(uintptr_t)&process_heap_identity; return 1; }
        if (!strcmp(name,"HeapAlloc") || !strcmp(name,"HeapFree") || !strcmp(name,"HeapReAlloc") || !strcmp(name,"HeapSize")) {
            uint64_t heap=(uintptr_t)&process_heap_identity;
            if (a!=heap) { win64_set_error(6); c->rax=!strcmp(name,"HeapSize")?UINT64_MAX:0; return 1; }
            if (!strcmp(name,"HeapAlloc")) {
                if (b&~UINT64_C(9)) invalid("unsupported HeapAlloc flags=0x%" PRIx64,b);
                c->rax=win64_alloc((size_t)d,(b&8)!=0); if (c->rax) heap_block(c->rax)->heap=1; return 1;
            }
            heap_header *old=heap_block(d);
            if (!old || !old->heap) { win64_set_error(87); c->rax=!strcmp(name,"HeapSize")?UINT64_MAX:0; return 1; }
            if (!strcmp(name,"HeapSize")) { if (b&~UINT64_C(1)) invalid("unsupported HeapSize flags"); c->rax=old->size; return 1; }
            if (!strcmp(name,"HeapFree")) { if (b&~UINT64_C(1)) invalid("unsupported HeapFree flags"); win64_free(d); c->rax=1; return 1; }
            if (b&~UINT64_C(0x19)) invalid("unsupported HeapReAlloc flags");
            if (b&0x10) { c->rax=0; return 1; }
            uint64_t replacement=win64_alloc((size_t)e,(b&8)!=0); if (!replacement) { c->rax=0; return 1; }
            heap_block(replacement)->heap=1;
            memcpy((void *)(uintptr_t)replacement,(const void *)(uintptr_t)d,old->size<(size_t)e?old->size:(size_t)e);
            win64_free(d); c->rax=replacement; return 1;
        }
        if (!strcmp(name,"GetSystemTimeAsFileTime")) {
            struct timespec now; if (clock_gettime(CLOCK_REALTIME,&now)) invalid("realtime clock");
            uint64_t ticks=((uint64_t)now.tv_sec+UINT64_C(11644473600))*10000000+(uint64_t)now.tv_nsec/100;
            memcpy(win64_pointer(a,8,1),&ticks,8); return 1;
        }
        if (!strcmp(name,"GetStartupInfoW") || !strcmp(name,"GetStartupInfoA")) {
            unsigned char *info=win64_pointer(a,104,1); memset(info,0,104); uint32_t cb=104; memcpy(info,&cb,4); return 1;
        }
        if (!strcmp(name,"IsDebuggerPresent")) { c->rax=0; return 1; }
        if (!strcmp(name,"IsProcessorFeaturePresent")) {
            /* The translated guest contract exposes CMOV and SSE/SSE2 only. */
            c->rax=(a==3 || a==6 || a==10); return 1;
        }
        if (!strcmp(name,"GetACP") || !strcmp(name,"GetOEMCP")) { c->rax=65001; return 1; }
        if (!strcmp(name,"AreFileApisANSI")) { c->rax=1; return 1; }
        if (!strcmp(name,"OutputDebugStringA")) { fprintf(stderr,"GUEST_DEBUG %s\n",win64_string_a(a)); return 1; }
        if (!strcmp(name,"TerminateProcess") || !strcmp(name,"ExitProcess")) {
            if(!strcmp(name,"TerminateProcess")&&a!=UINT64_MAX){win64_set_error(5);c->rax=0;return 1;}
            uint32_t code=(uint32_t)(!strcmp(name,"ExitProcess")?a:b);
            fprintf(stderr,"GUEST_PROCESS_EXIT api=%s code=%u\n",name,code);fflush(stderr);
            if(frostpunk_android_exiting)frostpunk_android_exiting(code);_Exit((int)code);
        }
        if (!strcmp(name,"VirtualAlloc")) {
            if (!b || b>SIZE_MAX-4095 || !(d&0x3000) || (d&~UINT64_C(0x3000))) { win64_set_error(87); c->rax=0; return 1; }
            size_t page=(size_t)sysconf(_SC_PAGESIZE);
            uint64_t aligned=a&~(uint64_t)((d&0x2000)?65535:page-1);
            if (b>SIZE_MAX-(size_t)(a-aligned)-65536) { win64_set_error(8); c->rax=0; return 1; }
            size_t n=((size_t)(a-aligned+b)+page-1)&~(page-1);
            int prot=host_protection((uint32_t)e); if (prot<0) { win64_set_error(87); c->rax=0; return 1; }
            if (a && !(d&0x2000)) {
                aligned=a&~(GUEST_PAGE-1); n=((size_t)(a-aligned+b)+GUEST_PAGE-1)&~(size_t)(GUEST_PAGE-1);
                allocation *m=allocation_containing(aligned,n);
                if (!m || !m->mapped || !host_protect(aligned,aligned+n,prot)) { win64_set_error(487); c->rax=0; return 1; }
                protection_set(aligned,n,(uint32_t)e,0x1000); c->rax=aligned; return 1;
            }
            int committed=(d&0x1000)!=0; if (!committed) prot=PROT_NONE;
            size_t reservation=a?n:n+65536;
            void *p=mmap((void *)(uintptr_t)aligned,reservation,prot,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if (p==MAP_FAILED || (a && (uint64_t)(uintptr_t)p!=aligned)) {
                if (p!=MAP_FAILED) munmap(p,reservation); win64_set_error(a?487:8); c->rax=0; return 1;
            }
            if (!a) {
                uint64_t raw=(uintptr_t)p, selected=(raw+65535)&~UINT64_C(65535);
                size_t prefix=(size_t)(selected-raw),suffix=reservation-prefix-n;
                if (prefix) munmap(p,prefix);
                if (suffix) munmap((void *)(uintptr_t)(selected+n),suffix);
                p=(void *)(uintptr_t)selected;
            }
            remember_allocation(p,n,1,(uint32_t)e); c->rax=(uint64_t)(uintptr_t)p;
            protection_set(c->rax,n,committed?(uint32_t)e:0,committed?0x1000:0x2000);
            fprintf(stderr,"HLE VirtualAlloc address=0x%" PRIx64 " bytes=%zu guest_protection=0x%x host_NX=1\n",c->rax,n,(unsigned)e); return 1;
        }
        if (!strcmp(name,"VirtualFree")) {
            if (d==0x4000) {
                uint64_t low=a&~(GUEST_PAGE-1);
                allocation *m=allocation_containing(low,b?(size_t)b:1);
                if (!m || !m->mapped || (!b && low!=(uintptr_t)m->p)) { win64_set_error(487); c->rax=0; return 1; }
                size_t count=b?((size_t)(a-low+b)+GUEST_PAGE-1)&~(size_t)(GUEST_PAGE-1):m->size;
                if (!allocation_containing(low,count) || !host_decommit(low,low+count)) { win64_set_error(487); c->rax=0; return 1; }
                protection_set(low,count,0,0x2000); c->rax=1; return 1;
            }
            allocation *m=allocation_at(a);
            if (!m || !m->mapped || b || d!=0x8000) { win64_set_error(87); c->rax=0; return 1; }
            c->rax=munmap(m->p,m->size)==0; if (c->rax) forget_allocation(m); else win64_set_error(87); return 1;
        }
        if (!strcmp(name,"VirtualProtect")) {
            if (!b || !accessible(a,(size_t)b,-2) || !accessible(e,4,1)) { win64_set_error(487); c->rax=0; return 1; }
            region *r=regions; while (r && !(a>=r->base && a-r->base<r->size)) r=r->next;
            allocation *m=r?allocation_at(r->owner):NULL;
            if (!r || (m && !m->mapped)) { win64_set_error(487); c->rax=0; return 1; }
            int prot=host_protection((uint32_t)d); if (prot<0) { win64_set_error(87); c->rax=0; return 1; }
            uint64_t low=a&~(GUEST_PAGE-1),high=(a+b+GUEST_PAGE-1)&~(GUEST_PAGE-1);
            if (high<low || !host_protect(low,high,prot)) { win64_set_error(487); c->rax=0; return 1; }
            uint32_t old=r->protection; protection_set(low,(size_t)(high-low),(uint32_t)d,0);
            memcpy(win64_pointer(e,4,1),&old,4); c->rax=1; return 1;
        }
        if (!strcmp(name,"VirtualQuery")) {
            if (d<48) { win64_set_error(24); c->rax=0; return 1; }
            if (a>=UINT64_C(0x0000800000000000)) { win64_set_error(87); c->rax=0; return 1; }
            unsigned char *info=win64_pointer(b,48,1); memset(info,0,48);
            uint64_t low=a&~(GUEST_PAGE-1),limit=UINT64_C(0x0000800000000000);
            region *found=NULL;
            for (region *r=regions;r;r=r->next) { if (low>=r->base && low-r->base<r->size) found=r; else if (r->base>low && r->base<limit) limit=r->base; }
            uint64_t base=low,allocation_base=found?found->owner:0,length=found?found->base+found->size-low:limit-low;
            uint32_t protection=found?found->protection:0,state=found?found->state:0x10000,type=found?(image_containing(low)?0x1000000:0x20000):0;
            allocation *m=found?allocation_at(found->owner):NULL; uint32_t initial=m?m->protection:protection;
            memcpy(info,&base,8);memcpy(info+8,&allocation_base,8);memcpy(info+16,&initial,4);memcpy(info+24,&length,8);
            memcpy(info+32,&state,4);memcpy(info+36,&protection,4);memcpy(info+40,&type,4);c->rax=48;return 1;
        }
        if (!strcmp(name,"IsBadReadPtr")) { c->rax=!accessible(a,(size_t)b,0); return 1; }
        if (!strcmp(name,"QueryPerformanceCounter") || !strcmp(name,"QueryPerformanceFrequency")) {
            int64_t result=1000000000; if (!strcmp(name,"QueryPerformanceCounter")) {
                result=(int64_t)win64_game_clock_ns(CLOCK_MONOTONIC);
            }
            memcpy(win64_pointer(a,8,1),&result,8); c->rax=1; return 1;
        }
        if (!strcmp(name,"Sleep")) { struct timespec t={(uint32_t)a/1000,((uint32_t)a%1000)*1000000}; while (nanosleep(&t,&t) && errno==EINTR) {} return 1; }
    }
    if (is_crt(dll)) {
        if (!strcmp(name,"_time64")) {
            void *output=a?win64_pointer(a,8,1):NULL;
            struct timespec now; if (clock_gettime(CLOCK_REALTIME,&now)) invalid("CRT realtime clock");
            int64_t seconds=(int64_t)now.tv_sec;
            if (output) memcpy(output,&seconds,8);
            c->rax=(uint64_t)seconds; return 1;
        }
        if (!strcmp(name,"_set_fmode")) {
            if (a!=0x4000 && a!=0x8000 && a!=0x10000) invalid("invalid _set_fmode argument");
            default_file_mode=(int)a; c->rax=0; return 1;
        }
        if (!strcmp(name,"_get_fmode")) { memcpy(win64_pointer(a,4,1),&default_file_mode,4); c->rax=0; return 1; }
        if (!strcmp(name,"__p__commode")) { c->rax=(uintptr_t)&commit_mode; return 1; }
        if (!strcmp(name,"__setusermatherr")) { atomic_store_explicit(&user_math_error,a,memory_order_release); return 1; }
        if (!strcmp(name,"_set_app_type")) { application_type=(int)a; return 1; }
        if (!strcmp(name,"_set_new_mode")) { c->rax=(uint32_t)new_mode; new_mode=(int)a; return 1; }
        if (!strcmp(name,"_configure_narrow_argv")) { argument_mode=(int)a; c->rax=0; return 1; }
        if (!strcmp(name,"_get_narrow_winmain_command_line")) { c->rax=(uintptr_t)command_line; return 1; }
        if (!strcmp(name,"_initialize_narrow_environment")) { c->rax=0; return 1; }
        if (!strcmp(name,"_errno")) { c->rax=(uintptr_t)&crt_errno; return 1; }
        if (!strcmp(name,"_initialize_onexit_table")) { c->rax=(uint64_t)(int64_t)initialize_onexit_table(a); return 1; }
        if (!strcmp(name,"_register_thread_local_exe_atexit_callback")) { tls_exit_callback=a; return 1; }
        if (!strcmp(name,"_crt_atexit")) { c->rax=(uint64_t)(int64_t)crt_atexit_register(a); return 1; }
        if (!strcmp(name,"_register_onexit_function")) { c->rax=(uint64_t)(int64_t)register_onexit_function(a,b); return 1; }
        if (!strcmp(name,"_execute_onexit_table")) { c->rax=(uint64_t)(int64_t)execute_onexit_table(c,a); return 1; }
        if (!strcmp(name,"_cexit")) { crt_exit_cleanup(c); return 1; }
        if (!strcmp(name,"_c_exit")) return 1;
        if (!strcmp(name,"exit") || !strcmp(name,"_exit") || !strcmp(name,"_Exit")) {
            uint32_t code=(uint32_t)a;if(!strcmp(name,"exit"))crt_exit_cleanup(c);
            fprintf(stderr,"GUEST_PROCESS_EXIT api=%s code=%u\n",name,code);fflush(stderr);
            if(frostpunk_android_exiting)frostpunk_android_exiting(code);_Exit((int)code);
        }
        if (!strcmp(name,"_initterm") || !strcmp(name,"_initterm_e")) {
            if (b<a || (b-a)%8 || b-a>16*1024*1024) invalid("invalid CRT initializer array");
            win64_pointer(a,(size_t)(b-a),0);
            for (uint64_t slot=a;slot<b;slot+=8) {
                uint64_t fn=rd64(slot); if (!fn) continue;
                uint64_t saved_rsp=c->rsp,frame=(saved_rsp-32)&~UINT64_C(15);
                win64_pointer(frame-8,(size_t)(saved_rsp-frame+8),1);
                c->rsp=frame; push64(c,UINT64_C(0xe5ef0000)); dispatch(c,fn);
                if (c->rsp!=frame) invalid("CRT callback stack imbalance function=0x%" PRIx64,fn);
                c->rsp=saved_rsp;
                if (!strcmp(name,"_initterm_e") && (uint32_t)c->rax) return 1;
            }
            if (!strcmp(name,"_initterm_e")) c->rax=0; return 1;
        }
        if (!strcmp(name,"_invalid_parameter_noinfo_noreturn") || !strcmp(name,"_invalid_parameter_noinfo") ||
            !strcmp(name,"_wassert") || !strcmp(name,"terminate") || !strcmp(name,"_purecall")) invalid("CRT fatal handler %s",name);
        if (!strcmp(name,"fesetround")) {
            int mode; switch ((int)a) { case 0:mode=FE_TONEAREST;break; case 0x100:mode=FE_DOWNWARD;break;
                case 0x200:mode=FE_UPWARD;break; case 0x300:mode=FE_TOWARDZERO;break; default:c->rax=UINT64_MAX;return 1; }
            c->rax=(uint64_t)(int64_t)fesetround(mode);
            if (!c->rax) { c->mxcsr=(c->mxcsr&~0x6000u)|(((unsigned)a>>8)<<13);win64_jump_set_round((unsigned)a>>8); }
            return 1;
        }
        if (!strcmp(name,"fegetround")) { c->rax=((c->mxcsr>>13)&3u)<<8; return 1; }
        if (!strcmp(name,"malloc")) return fast_malloc(c);
        if (!strcmp(name,"calloc")) return fast_calloc(c);
        if (!strcmp(name,"realloc")) { c->rax=crt_resize(a,(size_t)b,0); return 1; }
        if (!strcmp(name,"_aligned_malloc")) {
            if (!b) invalid("zero _aligned_malloc alignment");
            c->rax=crt_allocate((size_t)a,(size_t)b); return 1;
        }
        if (!strcmp(name,"_aligned_realloc")) {
            if (!d || (d&(d-1))) invalid("invalid _aligned_realloc alignment");
            c->rax=crt_resize(a,(size_t)b,(size_t)d); return 1;
        }
        if (!strcmp(name,"_aligned_free")) return fast_aligned_free(c);
        if (!strcmp(name,"free")) {
            win64_free(a);
            return 1;
        }
        if (!strcmp(name,"memcpy") || !strcmp(name,"memmove")) {
            memmove(win64_pointer(a,(size_t)d,1),win64_pointer(b,(size_t)d,0),(size_t)d); c->rax=a; return 1;
        }
        if (!strcmp(name,"memset")) { memset(win64_pointer(a,(size_t)d,1),(int)b,(size_t)d); c->rax=a; return 1; }
        if (!strcmp(name,"memcmp")) { c->rax=(uint64_t)(int64_t)memcmp(win64_pointer(a,(size_t)d,0),win64_pointer(b,(size_t)d,0),(size_t)d); return 1; }
        if (!strcmp(name,"strlen")) { c->rax=strlen(win64_string_a(a)); return 1; }
        if (!strcmp(name,"strcmp")) { c->rax=(uint64_t)(int64_t)strcmp(win64_string_a(a),win64_string_a(b)); return 1; }
    }
    return 0;
}

/* Import dispatch. import_chain() is the original provider order; every
 * provider is one step. A per-import cache (keyed by the static dll/name
 * strings) remembers which step handled a name, so later calls resume at
 * that step instead of re-walking unrelated providers, and a few hot CRT and
 * kernel entries get direct handlers. Earlier steps only ever declined these
 * names, so skipping them is behaviour-preserving. */
enum { IMPORT_CRT=1, IMPORT_KERNEL=2, IMPORT_USER=4 };
static unsigned import_flags(const char *dll) {
    return (is_crt(dll)?IMPORT_CRT:0u) |
        ((!strcasecmp(dll,"kernel32.dll") || !strcasecmp(dll,"kernelbase.dll"))?IMPORT_KERNEL:0u) |
        (!strcasecmp(dll,"user32.dll")?IMPORT_USER:0u);
}

static int import_chain(CPU *c, const char *dll, const char *name, unsigned flags, unsigned start, unsigned *handled) {
    const int crt=(flags&IMPORT_CRT)!=0, kernel=(flags&IMPORT_KERNEL)!=0, user=(flags&IMPORT_USER)!=0;
    unsigned step=start;
#define IMPORT_DONE do { *handled=step; c->rsp+=8; return 1; } while (0)
#define IMPORT_STEP(n) /* fallthrough */ case n: step=n;
    switch (start) {
    IMPORT_STEP(0)
    if (crt && !strcmp(name,"powf")) {
        if (!win64_stdio_call(c,name) && !win64_math_call(c,name)) return 0;
        IMPORT_DONE;
    }
    IMPORT_STEP(1)
    if (crt && (!strcmp(name,"memcpy") || !strcmp(name,"memmove") ||
                !strcmp(name,"strcat_s") || !strcmp(name,"strcpy_s") || !strcmp(name,"strncmp"))) {
        /* Preserve stdio's first-call FILE registration even when it declines
         * this name. Other earlier providers reject these CRT copies purely. */
        if (!win64_stdio_call(c,name)) {
            if (name[0]=='m') {
                uint64_t a=c->rcx,b=c->rdx,d=c->r8;
                memmove(win64_pointer(a,(size_t)d,1),win64_pointer(b,(size_t)d,0),(size_t)d); c->rax=a;
            } else if (!win64_string_call(c,name)) return 0;
        }
        IMPORT_DONE;
    }
    IMPORT_STEP(2)
    if (kernel && !strcmp(name,"QueryPerformanceCounter")) {
        /* Keep the kernel provider's first-call initialization and lock boundary. */
        if (!win64_kernel_call(c,name) && !execute(c,dll,name)) return 0;
        IMPORT_DONE;
    }
    IMPORT_STEP(3)
    if (!strcasecmp(dll,"advapi32.dll") && win64_identity_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(4)
    if (!strcasecmp(dll,"anselsdk64.dll") && !strcmp(name,"?isAnselAvailable@ansel@@YA_NXZ")) { c->rax=0; IMPORT_DONE; }
    IMPORT_STEP(5)
    if (kernel && win64_environment_windows_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(6)
    if (win64_shader_import(c,dll,name)) IMPORT_DONE;
    IMPORT_STEP(7)
    if (!strcasecmp(dll,"newsfeed.dll") && win64_newsfeed_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(8)
    if (win64_steam_import(c,dll,name)) IMPORT_DONE;
    IMPORT_STEP(9)
    if (win64_d3d11_import(c,dll,name)) IMPORT_DONE;
    IMPORT_STEP(10)
    if (!strcasecmp(dll,"dinput8.dll") && win64_dinput_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(11)
    if (!strcasecmp(dll,"winmm.dll") && (win64_time_winmm_call(c,name) || win64_openal_winmm_call(c,name))) IMPORT_DONE;
    IMPORT_STEP(12)
    if (!strcasecmp(dll,"ole32.dll") && win64_com_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(13)
    if (!strcasecmp(dll,"shell32.dll") && win64_shell_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(14)
    if (!strcasecmp(dll,"shlwapi.dll") && win64_path_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(15)
    if (!strcasecmp(dll,"vcruntime140.dll") && win64_jump_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(16)
    if (!strcasecmp(dll,"vcruntime140.dll") && win64_rtti_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(17)
    if (!strcasecmp(dll,"dbghelp.dll") && win64_dbghelp_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(18)
    if (kernel && win64_error_message_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(19)
    if (kernel && win64_encoding_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(20)
    if (kernel && !strcmp(name,"RaiseException") && win64_threads_raise_exception(c)) IMPORT_DONE;
    IMPORT_STEP(21)
    if (crt && win64_environment_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(22)
    if (crt && win64_time_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(23)
    if (crt && win64_wide_stdio_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(24)
    if (crt && win64_stdio_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(25)
    if (crt && win64_locale_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(26)
    if (crt && win64_string_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(27)
    if (crt && win64_scan_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(28)
    if (crt && win64_utility_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(29)
    if (crt && win64_math_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(30)
    if ((user || !strcasecmp(dll,"gdi32.dll") || !strcasecmp(dll,"shcore.dll")) && win64_display_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(31)
    if (user && win64_accessibility_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(32)
    if (user && win64_window_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(33)
    if (kernel && win64_system_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(34)
    if (kernel && win64_sync_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(35)
    if ((kernel || crt) && win64_threads_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(36)
    if (kernel && win64_kernel_call(c,name)) IMPORT_DONE;
    IMPORT_STEP(37)
    {
    /* Never hold this across guest callbacks or kernel waits. */
    int locked=!strncmp(name,"Virtual",7) || !strncmp(name,"Heap",4) || !strncmp(name,"_aligned_",9) || !strcmp(name,"realloc") || !strncmp(name,"GetModuleHandle",15) ||
        !strncmp(name,"LoadLibrary",11) || !strcmp(name,"GetProcAddress") || !strcmp(name,"FreeLibrary") || !strcmp(name,"RtlPcToFileHeader") || !strcmp(name,"SetUnhandledExceptionFilter");
    if (locked) metadata_lock();
    int mutating=!strcmp(name,"VirtualAlloc") || !strcmp(name,"VirtualFree") || !strcmp(name,"VirtualProtect");
    if(mutating)win64_region_mutation_begin();
    int done=execute(c,dll,name);
    if(mutating)win64_region_mutation_end();
    if (locked) metadata_unlock();
    if (!done) return 0;
    IMPORT_DONE;
    }
    }
#undef IMPORT_STEP
#undef IMPORT_DONE
    return 0;
}

/* Direct handlers: same results as the chain step they replace. */
typedef int (*fast_import_fn)(CPU *c);
static int fast_memmove(CPU *c) { memmove((void *)(uintptr_t)c->rcx,(const void *)(uintptr_t)c->rdx,(size_t)c->r8); c->rax=c->rcx; return 1; }
static int fast_memset(CPU *c) { memset((void *)(uintptr_t)c->rcx,(int)c->rdx,(size_t)c->r8); c->rax=c->rcx; return 1; }
static int fast_memcmp(CPU *c) { c->rax=(uint64_t)(int64_t)memcmp((const void *)(uintptr_t)c->rcx,(const void *)(uintptr_t)c->rdx,(size_t)c->r8); return 1; }
static int fast_strlen(CPU *c) { c->rax=strlen(win64_string_a(c->rcx)); return 1; }
static int fast_strcmp(CPU *c) { c->rax=(uint64_t)(int64_t)strcmp(win64_string_a(c->rcx),win64_string_a(c->rdx)); return 1; }
static int fast_qpc(CPU *c) {
    int64_t result=(int64_t)win64_game_clock_ns(CLOCK_MONOTONIC);
    memcpy(win64_pointer(c->rcx,8,1),&result,8); c->rax=1; return 1;
}
__attribute__((visibility("default"))) void frostpunk_android_set_paused(int paused) {
    port_adpf_block(1, paused);
    static pthread_mutex_t lifecycle_mutex=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lifecycle_mutex);
    win64_time_pause(paused);win64_openal_pause(paused);
    pthread_mutex_unlock(&lifecycle_mutex);
}
static int fast_qpf(CPU *c) { int64_t result=1000000000; memcpy(win64_pointer(c->rcx,8,1),&result,8); c->rax=1; return 1; }
static int fast_sleep(CPU *c) {
    uint32_t ms=(uint32_t)c->rcx;
    /* Sleep(0) gives up the rest of the time slice and returns at once when no
     * other thread is ready; nanosleep(0) would wait for timer slack instead. */
    if (!ms) { sched_yield(); return 1; }
    struct timespec t={ms/1000,(ms%1000)*1000000}; while (nanosleep(&t,&t) && errno==EINTR) {}
    return 1;
}

static fast_import_fn fast_import_for(const char *name,unsigned flags,unsigned step) {
    fast_import_fn f=NULL;
    if (flags&IMPORT_CRT) {
        if (step==1 && (!strcmp(name,"memcpy") || !strcmp(name,"memmove"))) return fast_memmove;
        /* Step 0 runs stdio's one-time FILE registration and its decline, then the UCRT-exact
         * powf; once cached, only the powf handler itself remains. */
        if (step==0 && !strcmp(name,"powf")) return win64_math_fast_powf;
        if (!strcmp(name,"memset")) return fast_memset;
        if (!strcmp(name,"memcmp")) return fast_memcmp;
        if (!strcmp(name,"strlen")) return fast_strlen;
        if (!strcmp(name,"strcmp")) return fast_strcmp;
        if ((f=heap_fast_import(name))) return f;
        /* Only at the step whose provider these handlers stand in for: step 1 is the CRT copy block
         * (stdio declines strncmp/strcpy_s, then the string provider), 26 the string provider,
         * 28 the utility provider (atoi, qsort, bsearch), 29 the math provider. */
        if (step==1 && (!strcmp(name,"strncmp") || !strcmp(name,"strcpy_s"))) return win64_string_fast_import(name);
        if (step==26 && (f=win64_string_fast_import(name))) return f;
        if (step==28 && (f=win64_utility_fast_import(name))) return f;
        if (step==29 && (f=win64_math_fast_import(name))) return f;
    }
    if (flags&IMPORT_KERNEL) {
        if (!strcmp(name,"QueryPerformanceCounter")) return fast_qpc;
        if (!strcmp(name,"QueryPerformanceFrequency")) return fast_qpf;
        if (!strcmp(name,"Sleep")) return fast_sleep;
        if ((f=(fast_import_fn)win64_threads_fast_import(name))) return f;
        if (step==36 && (f=win64_kernel_fast_import(name))) return f;
    }
    if ((flags&IMPORT_USER) && step==32 && (f=win64_window_fast_import(name))) return f;
    return NULL;
}

#define IMPORT_CACHE 8192u
typedef struct { _Atomic(uintptr_t) key; _Atomic(const char *) name; const char *dll; fast_import_fn fast; unsigned flags,step; atomic_int ready; } import_entry;
static import_entry import_cache[IMPORT_CACHE];

static import_entry *import_slot(const char *dll,const char *name) {
    uintptr_t key=((uintptr_t)name*UINT64_C(0x9E3779B97F4A7C15))^(uintptr_t)dll;
    if (!key) key=1;
    size_t h=(size_t)((key*UINT64_C(0xff51afd7ed558ccd))>>51)&(IMPORT_CACHE-1);
    for (unsigned probe=0;probe<IMPORT_CACHE;++probe) {
        import_entry *e=&import_cache[(h+probe)&(IMPORT_CACHE-1)];
        uintptr_t seen=atomic_load_explicit(&e->key,memory_order_acquire);
        if (!seen) {
            uintptr_t expected=0;
            if (atomic_compare_exchange_strong(&e->key,&expected,key)) {
                e->dll=dll; atomic_store_explicit(&e->name,name,memory_order_release); return e;
            }
            seen=expected;
        }
        if (seen==key) {
            const char *owner;
            while (!(owner=atomic_load_explicit(&e->name,memory_order_acquire))) {}
            if (owner==name && e->dll==dll) return e;
        }
    }
    return NULL;
}

#ifdef FP_IMPORT_AUDIT
/* Audit build: always walk the full chain and report names whose handling
 * step depends on the arguments (the cache would skip earlier providers). */
static _Atomic uint64_t import_audit_steps[IMPORT_CACHE];
#endif

/* Diagnostics (debug.fp.import_verify=1): the direct handlers of the pure imports (math, string
 * comparison and search, atoi) are not handed to the lifted code's inline path; every call then comes
 * here, runs the handler on a copy of the CPU state and the provider chain on the real one, and logs
 * IMPORT_VERIFY_MISMATCH when the CPU state or errno differ (IMPORT_VERIFY every 100000 calls). */
static int import_verify_mode(void) {
    static _Atomic int mode=-1;int value=atomic_load_explicit(&mode,memory_order_relaxed);
    if (value<0) {
        char text[PROP_VALUE_MAX]={0};int saved=errno;
        value=__system_property_get("debug.fp.import_verify",text)>0&&text[0]=='1';errno=saved;
        atomic_store_explicit(&mode,value,memory_order_relaxed);
    }
    return value;
}
static int import_verifiable(const char *name) {
    static const char *const pure[]={"strncmp","_stricmp","_strnicmp","strchr","strcspn","atoi","floorf","floor","ceilf","ceil",
        "roundf","round","sinf","cosf","tanf","atan2f","asinf","acosf","atanf","fmodf","expf","logf","log10f","exp2f","tanhf",
        "sinhf","coshf","sqrtf","sqrt"};
    for (size_t i=0;i<sizeof(pure)/sizeof(*pure);++i) if (!strcmp(name,pure[i])) return 1;
    return 0;
}
static void import_verify(CPU *c,import_entry *e,const char *dll,const char *name) {
    static _Atomic uint64_t calls,mismatches;
    CPU copy=*c;int saved=errno;
    e->fast(&copy);copy.rsp+=8;int fast_errno=errno;errno=saved;
    unsigned handled;import_chain(c,dll,name,e->flags,e->step,&handled);
    uint64_t n=atomic_fetch_add_explicit(&calls,1,memory_order_relaxed)+1;
    if (memcmp(&copy,c,sizeof(*c))||fast_errno!=errno) {
        uint64_t m=atomic_fetch_add_explicit(&mismatches,1,memory_order_relaxed)+1;
        if (m<=50) fprintf(stderr,"IMPORT_VERIFY_MISMATCH %s rax=%" PRIx64 "/%" PRIx64 " xmm0=%" PRIx64 "/%" PRIx64 " mxcsr=%x/%x errno=%d/%d\n",name,
            copy.rax,c->rax,copy.xmm[0].u64[0],c->xmm[0].u64[0],(unsigned)copy.mxcsr,(unsigned)c->mxcsr,fast_errno,errno);
    }
    if (!(n%100000)) fprintf(stderr,"IMPORT_VERIFY calls=%" PRIu64 " mismatches=%" PRIu64 "\n",n,atomic_load(&mismatches));
}

int win64_dispatch_import(CPU *c, const char *dll, const char *name) {
    FP_PERF_IMPORT_NAME(dll,name);
    import_entry *e=import_slot(dll,name);
#ifdef FP_IMPORT_AUDIT
    {
        unsigned handled=0;int result=import_chain(c,dll,name,import_flags(dll),0,&handled);
        if (result && e && handled<64) {
            uint64_t bit=UINT64_C(1)<<handled,old=atomic_fetch_or(&import_audit_steps[e-import_cache],bit);
            if (old && !(old&bit)) fprintf(stderr,"IMPORT_AUDIT %s!%s steps=0x%" PRIx64 "\n",dll,name,old|bit);
        }
        return result;
    }
#endif
    if (e && atomic_load_explicit(&e->ready,memory_order_acquire)) {
        if (e->fast) {
            if (__builtin_expect(import_verify_mode(),0) && import_verifiable(name)) { import_verify(c,e,dll,name); return 1; }
            e->fast(c); c->rsp+=8; return 1;
        }
        unsigned handled;
        return import_chain(c,dll,name,e->flags,e->step,&handled);
    }
    unsigned flags=import_flags(dll),handled=0;
    int result=import_chain(c,dll,name,flags,0,&handled);
    if (result && e && !atomic_load_explicit(&e->ready,memory_order_relaxed)) {
        e->flags=flags; e->step=handled; e->fast=fast_import_for(name,flags,handled);
        atomic_store_explicit(&e->ready,1,memory_order_release);
    }
    return result;
}

/* Direct handler of an import that was already dispatched once (NULL when it has none): lets
 * the dispatcher and the fastlift units skip the name-keyed cache on later calls. The caller
 * pops the return address exactly as win64_dispatch_import does. */
fast_import_fn win64_import_fast(const char *dll, const char *name) {
    import_entry *e=import_slot(dll,name);
    if (import_verify_mode() && import_verifiable(name)) return NULL;
    return e && atomic_load_explicit(&e->ready,memory_order_acquire) ? e->fast : NULL;
}

/* android/vulkan/rdvk_runtime.c: COM proxies of the native Vulkan rendering device. */
extern int frostpunk_rdvk_dispatch(CPU *c,uint64_t target) __attribute__((weak));
extern int frostpunk_vorbisfile_dispatch(CPU *c,uint64_t target) __attribute__((weak));
int win64_dispatch_dynamic(CPU *c,uint64_t address) {
    if (frostpunk_rdvk_dispatch && frostpunk_rdvk_dispatch(c,address)) { FP_PERF(FP_PERF_D3D11); c->rsp+=8; return 1; }
    /* android/host/vorbisfile_native.c: the read callback of its verification mode (0xE6300000). */
    if (address==UINT64_C(0xE6300000) && frostpunk_vorbisfile_dispatch && frostpunk_vorbisfile_dispatch(c,address)) { c->rsp+=8; return 1; }
    if (win64_dinput_dispatch(c,address)) { FP_PERF(FP_PERF_DINPUT); c->rsp+=8; return 1; }
    if (win64_openal_dispatch(c,address)) { FP_PERF(FP_PERF_OPENAL); c->rsp+=8; return 1; }
    if (win64_shader_dispatch(c,address)) { FP_PERF(FP_PERF_SHADER); c->rsp+=8; return 1; }
    if (win64_steam_dispatch(c,address)) { FP_PERF(FP_PERF_STEAM); c->rsp+=8; return 1; }
    if (win64_d3d11_dispatch(c,address)) { FP_PERF(FP_PERF_D3D11); c->rsp+=8; return 1; }
    FP_PERF(FP_PERF_EXPORT);
    /* Static function calls cannot name an export slot; avoid the shared lock. */
    if (address<EXPORT_BASE || address-EXPORT_BASE>=(uint64_t)EXPORT_CAPACITY*8) return 0;
    metadata_lock();
    if (address<EXPORT_BASE || address-EXPORT_BASE>=export_count*8) { metadata_unlock(); return 0; }
    if ((address-EXPORT_BASE)%8) invalid("unaligned dynamic export");
    export_entry *e=&exports[(address-EXPORT_BASE)/8];
    metadata_unlock();
    if (!win64_dispatch_import(c,e->owner->name,e->name)) {
        fprintf(stderr,"STOP unimplemented dynamic export %s!%s return=0x%" PRIx64 "\n",e->owner->name,e->name,rd64(c->rsp)); fflush(stderr); _Exit(70);
    }
    return 1;
}

void win64_init(uint64_t image,size_t image_bytes,uint64_t stack,size_t stack_bytes) {
    const char *profile=getenv("FROSTPUNK_SERVICES_PROFILE");char config[4096];
    if(profile&&snprintf(config,sizeof(config),"%s/AppData/11bitstudios/Frostpunk/config.bin",profile)<(int)sizeof(config)&&!access(config,R_OK)){
        char *flag=strstr(command_line,"-gfxconfig");if(flag)memset(flag,' ',10);
        flag=strstr(full_command_line,"-gfxconfig");if(flag)memset(flag,' ',10);
    }
    {
        /* -novideo turns the whole video engine off: startup logos, scenario cinematics and the
         * tutorial videos alike.  The engine always starts with videos now; the launcher's "skip
         * intro videos" only suppresses the logos played before the main menu (mobile_controls.c
         * holds the engine's own novideo bit during that phase and releases it afterwards). */
        char *flag=strstr(command_line,"-novideo");if(flag)memset(flag,' ',8);
        flag=strstr(full_command_line,"-novideo");if(flag)memset(flag,' ',8);
        const char *skip=getenv("PORT_SKIP_INTRO");
        win64_skip_intro=!(skip&&!strcmp(skip,"0"));
    }
    {
        /* Engine switches for A/B diagnostics (e.g. -disableTessellation); absent in releases. */
        char extra[PROP_VALUE_MAX]={0};
        if(__system_property_get("debug.fp.args",extra)>0&&extra[0]&&
           strlen(command_line)+strlen(extra)+2<sizeof(command_line)&&strlen(full_command_line)+strlen(extra)+2<sizeof(full_command_line)){
            strcat(command_line," ");strcat(command_line,extra);
            strcat(full_command_line," ");strcat(full_command_line,extra);
            fprintf(stderr,"FROSTPUNK_DEBUG_ARGS %s\n",extra);
        }
    }
    {
        /* Adreno 650 / Turnip (2026-11-06): KGSL "GPU hang detected ... TIMEOUTFAULT" with the
         * engine's terrain/water/shadow tessellation, about once per 1-3 min in the city with GMEM
         * rendering and once per ~45 min with system-memory rendering; KGSL's recovery then hit a
         * freed command buffer and the device was lost. With the engine's own -disableTessellation
         * no hang was seen, and at phone resolutions the picture is practically unchanged (mean
         * difference < 0.5/255). debug.fp.turnip_tessellation=1 keeps tessellation for tests. */
        const char *driver=getenv("FROSTPUNK_VULKAN_DRIVER");char keep[PROP_VALUE_MAX]={0};
        static const char flag[]=" -disableTessellation";
        if(driver&&!strcmp(driver,"turnip")&&!(__system_property_get("debug.fp.turnip_tessellation",keep)>0&&keep[0]=='1')
           &&!strstr(full_command_line,"-disableTessellation")
           &&strlen(command_line)+sizeof(flag)<sizeof(command_line)&&strlen(full_command_line)+sizeof(flag)<sizeof(full_command_line)){
            strcat(command_line,flag);strcat(full_command_line,flag);
            fprintf(stderr,"FROSTPUNK_TURNIP_TESSELLATION disabled=1\n");
        }
    }
    image_base=image; win64_region_add(image,image_bytes,1); win64_region_add(stack,stack_bytes,1); win64_image_add(image,image_bytes);
    module_add("kernel32.dll",1); module_add("kernelbase.dll",1); module_add("ntdll.dll",1); module_add("user32.dll",1);
    module_add("gdi32.dll",1);
    module_add("ole32.dll",1);
    module_add("shell32.dll",1);
    module_add("shlwapi.dll",1);
    win64_set_error(0);
    win64_region_add((uint64_t)(uintptr_t)command_line,sizeof(command_line),0);
    win64_region_add((uint64_t)(uintptr_t)executable_name,sizeof(executable_name),0);
    for (size_t i=0;i<sizeof(executable_name);++i) executable_name_w[i]=(unsigned char)executable_name[i];
    for (size_t i=0;i<sizeof(full_command_line);++i) full_command_line_w[i]=(unsigned char)full_command_line[i];
    win64_region_add((uintptr_t)executable_name_w,sizeof(executable_name_w),0);
    win64_region_add((uintptr_t)full_command_line,sizeof(full_command_line),0);
    win64_region_add((uintptr_t)full_command_line_w,sizeof(full_command_line_w),0);
    win64_thread_crt_attach();
    win64_region_add((uint64_t)(uintptr_t)&commit_mode,sizeof(commit_mode),1);
}

static void region_cache_compare(uint64_t p,size_t n,int writing) {
    metadata_lock();
    int cached=accessible(p,n,writing);
    int reference=!n?1:(!p || p>UINT64_MAX-n)?0:accessible_linear(p,n,writing,0);
    if (cached!=reference) invalid("region cache/reference mismatch address=0x%" PRIx64 " bytes=%zu write=%d",p,n,writing);
    metadata_unlock();
}

typedef struct { uint64_t base; unsigned iterations; } region_cache_stress;
static void *region_cache_reader(void *opaque) {
    region_cache_stress *test=opaque;
    for (unsigned i=0;i<test->iterations;++i) {
        region_cache_compare(test->base+16,8,1);
        region_cache_compare(test->base+128,8,0);
        region_cache_compare(test->base+128,8,1);
        region_cache_compare(test->base+120,80,1);
    }
    return NULL;
}
static void *region_cache_mutator(void *opaque) {
    region_cache_stress *test=opaque;
    for (unsigned i=0;i<test->iterations;++i) {
        win64_region_add(test->base+128,64,0);
        region_cache_compare(test->base+128,8,1);
        win64_region_remove(test->base+128);
    }
    return NULL;
}

static void region_cache_self_test(void) {
    unsigned char *priority_storage=malloc(96); if (!priority_storage) invalid("region split priority storage");
    uint64_t priority=(uintptr_t)priority_storage;
    win64_region_add(priority,96,1);win64_region_add(priority+3,7,0);
    metadata_lock();protection_set(priority+16,24,1,0);metadata_unlock();
    if (accessible(priority+3,1,1) || !accessible(priority+3,1,0) || !accessible(priority+10,6,1) ||
        accessible(priority+16,24,0) || !accessible(priority+40,56,1)) invalid("protection left split promoted lower priority region self-test");
    win64_region_add(priority+42,7,0);
    metadata_lock();protection_set(priority+60,4,2,0);metadata_unlock();
    if (accessible(priority+42,7,1) || !accessible(priority+42,7,0) || accessible(priority+40,12,1)) invalid("protection split preserved newer outside overlay self-test");
    win64_region_add(priority+20,4,1);
    if (!accessible(priority+20,4,1) || accessible(priority+19,6,0)) invalid("newer overlay after protection split self-test");
    win64_region_remove(priority+20);win64_region_remove(priority+42);win64_region_remove(priority+3);win64_region_remove(priority);free(priority_storage);
    unsigned char *storage=malloc(8192); if (!storage) invalid("region cache test storage");
    uint64_t base=(uintptr_t)storage;
    win64_region_add(base,8192,1);
    region_cache_compare(base+16,32,1);
    metadata_lock();
    if (!region_cache_count || region_cache[0].base!=base || region_cache[0].size!=8192) invalid("region cache population self-test");
    metadata_unlock();
    region_cache_compare(base+16,32,1);
    win64_region_add(base+256,64,0);
    region_cache_compare(base+128,8,1);
    metadata_lock();
    if (!region_cache_count || region_cache[0].base!=base || region_cache[0].size!=256) invalid("region cache segment before overlay self-test");
    metadata_unlock();
    region_cache_compare(base+400,8,1);
    metadata_lock();
    if (!region_cache_count || region_cache[0].base!=base+320 || region_cache[0].size!=8192-320) invalid("region cache segment after overlay self-test");
    metadata_unlock();
    win64_region_add(base+768,64,0);
    region_cache_compare(base+400,8,1);
    metadata_lock();
    if (!region_cache_count || region_cache[0].base!=base+320 || region_cache[0].size!=448) invalid("region cache segment between overlays self-test");
    metadata_unlock();
    if (accessible(base+400,400,1) || !accessible(base+400,400,0)) invalid("region cache clipped crossing overlay self-test");
    win64_region_remove(base+768);
    region_cache_compare(base+256,8,1);
    if (accessible(base+256,8,1) || !accessible(base+256,8,0)) invalid("region cache readonly overlay self-test");
    region_cache_compare(base+240,96,1);
    if (accessible(base+240,96,1) || !accessible(base+240,96,0)) invalid("region cache crossing readonly overlay self-test");
    metadata_lock(); protection_set(base+256,64,1,0); metadata_unlock();
    if (accessible(base+240,96,0) || accessible(base+240,96,1) || !accessible(base+240,96,-2)) invalid("region cache crossing noaccess self-test");
    metadata_lock(); protection_set(base+256,64,0,0x2000); metadata_unlock();
    for (int writing=-2;writing<=1;++writing) region_cache_compare(base+240,96,writing);
    if (accessible(base+240,96,0) || accessible(base+240,96,-2) || !accessible(base+240,96,-1)) invalid("region cache crossing reserved/decommitted self-test");
    metadata_lock(); protection_set(base+256,64,4,0x1000); metadata_unlock();
    win64_region_remove(base+256);
    if (!accessible(base+256,8,1)) invalid("region cache overlay removal self-test");
    metadata_lock(); protection_set(base+4096,4096,2,0); metadata_unlock();
    region_cache_compare(base+4096,8,1);
    if (accessible(base+4096,8,1) || !accessible(base+4096,8,0) || !accessible(base,4096,1) || accessible(base+4090,16,1)) invalid("region cache protection split self-test");
    metadata_lock(); protection_set(base+4096,4096,0,0x2000); metadata_unlock();
    for (int writing=-2;writing<=1;++writing) region_cache_compare(base+4096,8,writing);
    if (accessible(base+4096,8,0) || accessible(base+4096,8,-2) || !accessible(base+4096,8,-1)) invalid("region cache reserved/decommitted self-test");
    metadata_lock(); protection_set(base+4096,4096,4,0x1000); metadata_unlock();
    if (!accessible(base+4096,8,1)) invalid("region cache recommit self-test");
    win64_region_remove(base);
    if (accessible(base+16,8,0)) invalid("region cache removed owner fragments self-test");
    win64_region_add(base,8192,0);
    if (accessible(base+16,8,1) || !accessible(base+16,8,0)) invalid("region cache address reuse readonly self-test");
    win64_region_remove(base); win64_region_add(base,8192,1);
    if (!accessible(base+16,8,1)) invalid("region cache address reuse writable self-test");
    unsigned char small[REGION_CACHE_CAPACITY+2][16];
    for (unsigned i=0;i<REGION_CACHE_CAPACITY+2;++i) { win64_region_add((uintptr_t)small[i],16,i&1); region_cache_compare((uintptr_t)small[i],16,i&1); }
    metadata_lock();
    if (region_cache_count!=REGION_CACHE_CAPACITY) invalid("region cache full capacity self-test");
    for (unsigned i=0;i<REGION_CACHE_CAPACITY;++i)
        if (region_cache[i].base!=(uintptr_t)small[REGION_CACHE_CAPACITY+1-i] || region_cache[i].size!=16)
            invalid("region cache genuine LRU eviction self-test");
    metadata_unlock();
    region_cache_compare((uintptr_t)small[0],16,0);
    metadata_lock();
    if (region_cache_count!=REGION_CACHE_CAPACITY || region_cache[0].base!=(uintptr_t)small[0] ||
        region_cache[REGION_CACHE_CAPACITY-1].base!=(uintptr_t)small[3]) invalid("region cache evicted entry reload self-test");
    metadata_unlock();
    for (unsigned i=0;i<REGION_CACHE_CAPACITY+2;++i) { region_cache_compare((uintptr_t)small[i],16,1); win64_region_remove((uintptr_t)small[i]); }
    region_cache_stress stress={base,2000}; pthread_t reader,mutator;
    if (pthread_create(&reader,NULL,region_cache_reader,&stress) || pthread_create(&mutator,NULL,region_cache_mutator,&stress)) invalid("region cache stress thread creation");
    if (pthread_join(reader,NULL) || pthread_join(mutator,NULL)) invalid("region cache stress thread join");
    const uint64_t probes[]={0,base-1,base,base+128,base+4090,base+8191,base+8192,UINT64_MAX};
    const size_t sizes[]={0,1,8,8192};
    for (unsigned i=0;i<sizeof(probes)/sizeof(*probes);++i)
        for (unsigned j=0;j<sizeof(sizes)/sizeof(*sizes);++j)
            for (int writing=-2;writing<=1;++writing) region_cache_compare(probes[i],sizes[j],writing);
    win64_region_remove(base); free(storage);
    fprintf(stderr,"REGION_CACHE_SELF_TEST entries=%u capacity_plus_two_eviction=1 snapshots=1 single_pass=1 clipped_segments=3 split_priority_preserved=1 uncached_oracle=1 overlap_priority=1 invalidation=1 split_commit_decommit=1 address_reuse=1 concurrent_checks=10000 passed\n",(unsigned)REGION_CACHE_CAPACITY);
}

static void onexit_registration_self_test(void) {
    metadata_lock();
    uint64_t *saved_functions=exit_functions; size_t saved_count=exit_count,saved_capacity=exit_capacity;
    exit_functions=NULL; exit_count=exit_capacity=0;
    if (crt_atexit_register(0) || crt_atexit_register(UINT64_C(0x12345678)) || exit_count!=2 || exit_capacity!=32 ||
        exit_functions[0] || exit_functions[1]!=UINT64_C(0x12345678)) invalid("CRT atexit registration self-test");
    free(exit_functions); exit_functions=saved_functions; exit_count=saved_count; exit_capacity=saved_capacity;
    uint64_t table[3]={0},saved[3]; win64_region_add((uintptr_t)table,sizeof(table),1);
    if (initialize_onexit_table(0)!=-1 || register_onexit_function(0,1)!=-1 || initialize_onexit_table((uintptr_t)table)) invalid("onexit NULL/initialize self-test");
    if (register_onexit_function((uintptr_t)table,0) || register_onexit_function((uintptr_t)table,UINT64_C(0x12345678))) invalid("onexit append self-test");
    memcpy(saved,table,sizeof(saved));
    if (initialize_onexit_table((uintptr_t)table) || memcmp(saved,table,sizeof(saved)) || table[1]-table[0]!=16 ||
        rd64(table[0]) || rd64(table[0]+8)!=UINT64_C(0x12345678)) invalid("onexit populated-table preservation self-test");
    win64_free(table[0]);
    table[0]=0x30; table[1]=0x20; table[2]=0x10; memcpy(saved,table,sizeof(saved));
    if (initialize_onexit_table((uintptr_t)table) || memcmp(saved,table,sizeof(saved))) invalid("onexit unequal-end initialization self-test");
    table[0]=table[2]=0x10; table[1]=0x20;
    if (initialize_onexit_table((uintptr_t)table) || table[0] || table[1] || table[2]) invalid("onexit equal-end initialization self-test");
    win64_region_remove((uintptr_t)table); metadata_unlock();
    fputs("ONEXIT_REGISTRATION_SELF_TEST NULL_returns_minus1=1 NULL_callback=1 preserve_populated=1 append_order=1 capacity32=1 passed\n",stderr);
}

static void xinput_disconnected_self_test(void) {
    const uint32_t indices[]={0,1,2,3,4,UINT32_MAX};
    const char *names[]={"XInputGetState","XInputSetState"};
    uint64_t stack[8]={0},output[4],before[4];
    uint32_t saved_error=win64_get_error();int saved_errno=crt_errno;
    for(size_t api=0;api<sizeof(names)/sizeof(*names);++api)
    for(size_t i=0;i<sizeof(indices)/sizeof(*indices);++i)for(unsigned kind=0;kind<4;++kind) {
        memset(output,0xa5,sizeof(output));memcpy(before,output,sizeof(output));
        CPU c;memset(&c,0xa5,sizeof(c));c.rcx=UINT64_C(0x1234567800000000)|indices[i];
        c.rdx=kind==0?(uintptr_t)output:kind==1?0:kind==2?1:UINT64_MAX;c.rsp=(uintptr_t)&stack[3];
        int disconnected=indices[i]<4 && kind!=1;
        CPU expected=c;expected.rax=disconnected?1167:160;expected.rsp+=8;
        win64_set_error(UINT32_C(0x12345678));win64_stdio_set_errno(31337);
        if(!win64_dispatch_import(&c,"XINPUT9_1_0.dll",names[api]) ||
           memcmp(&c,&expected,sizeof(c)) || memcmp(output,before,sizeof(output)) ||
           win64_get_error()!=(disconnected?0:UINT32_C(0x12345678)) || crt_errno!=31337)
            invalid("XInput disconnected ABI self-test");
    }
    win64_set_error(saved_error);win64_stdio_set_errno(saved_errno);
    fputs("XINPUT_SELF_TEST controllers=0 GetState_SetState=1 disconnected1167=1 invalid_index_NULL160=1 buffers_untouched=1 full_CPU_single_pop=1 cases=48 passed\n",stderr);
}
void win64_self_test(void) {
    uint64_t stack[16]={0}; char kernel[]="KERNEL32.DLL", symbol[]="GetLastError", absent[]="NoSuchFrostpunkExport";
    win64_init(0x140000000,0x1000,(uint64_t)(uintptr_t)stack,sizeof(stack));
    win64_region_add((uint64_t)(uintptr_t)kernel,sizeof(kernel),0);
    win64_region_add((uint64_t)(uintptr_t)symbol,sizeof(symbol),0);
    win64_region_add((uint64_t)(uintptr_t)absent,sizeof(absent),0);
    CPU c={0}; c.rsp=(uint64_t)(uintptr_t)&stack[6]; c.rcx=(uint64_t)(uintptr_t)kernel;
    if (!win64_dispatch_import(&c,"KERNEL32.dll","GetModuleHandleA") || !c.rax || c.rsp!=(uint64_t)(uintptr_t)&stack[7]) invalid("module self-test");
    uint64_t handle=c.rax; c.rsp=(uint64_t)(uintptr_t)&stack[6]; c.rcx=handle; c.rdx=(uint64_t)(uintptr_t)symbol;
    win64_dispatch_import(&c,"KERNEL32.dll","GetProcAddress"); uint64_t fn=c.rax;
    if (fn<EXPORT_BASE) invalid("export self-test");
    c.rsp=(uint64_t)(uintptr_t)&stack[6]; c.rcx=handle; c.rdx=(uint64_t)(uintptr_t)absent;
    win64_dispatch_import(&c,"KERNEL32.dll","GetProcAddress"); if (c.rax) invalid("absent export self-test");
    c.rsp=(uint64_t)(uintptr_t)&stack[6]; if (!win64_dispatch_dynamic(&c,fn) || c.rax!=127) invalid("GetLastError self-test");
    CPU before_dynamic=c;uint32_t error_before_dynamic=win64_get_error();
    const uint64_t non_exports[]={0,EXPORT_BASE-1,EXPORT_BASE+(uint64_t)EXPORT_CAPACITY*8,UINT64_MAX,UINT64_C(0x140e02420)};
    for (size_t i=0;i<sizeof(non_exports)/sizeof(*non_exports);++i)
        if (win64_dispatch_dynamic(&c,non_exports[i]) || memcmp(&c,&before_dynamic,sizeof(c)) || win64_get_error()!=error_before_dynamic)
            invalid("non-export dispatch self-test");
    static const char touch_exports[][24]={"RegisterTouchWindow","GetTouchInputInfo","CloseTouchInputHandle"};
    win64_region_add((uintptr_t)touch_exports,sizeof(touch_exports),0);
    uint32_t touch_error=win64_get_error();int touch_errno=crt_errno,touch_host_errno=errno;
    for (size_t i=0;i<sizeof(touch_exports)/sizeof(touch_exports[0]);++i) {
        CPU probe;memset(&probe,0xa5,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
        probe.rcx=module_name("user32.dll")->handle;probe.rdx=(uintptr_t)touch_exports[i];
        CPU expected=probe;expected.rax=0;expected.rsp+=8;
        win64_set_error(0x5a17);crt_errno=31337;errno=E2BIG;
        if (!win64_dispatch_import(&probe,"KERNEL32.dll","GetProcAddress") ||
            memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=127 || crt_errno!=31337 || errno!=E2BIG)
            invalid("unavailable optional Win32 touch export self-test");
    }
    win64_region_remove((uintptr_t)touch_exports);
    win64_set_error(touch_error);crt_errno=touch_errno;errno=touch_host_errno;
    fputs("WIN32_TOUCH_SELF_TEST optional_exports=3 absent127=1 full_CPU_single_pop=1 passed\n",stderr);
    {
        static const char names[][64]={"LightFX.dll","LiGhTfX.DlL","C:\\vendor\\LightFX.dll","C:/vendor/LIGHTFX",
            "RzChromaSDK64.dll","rZcHrOmAsDk64.DlL","C:\\vendor\\RzChromaSDK64.dll","C:/vendor/RZCHROMASDK64"};
        uint16_t wide[8][64]={0};
        for(size_t i=0;i<8;++i)for(size_t j=0;names[i][j];++j)wide[i][j]=(unsigned char)names[i][j];
        win64_region_add((uintptr_t)names,sizeof(names),0);win64_region_add((uintptr_t)wide,sizeof(wide),0);
        module saved_modules[MODULE_CAPACITY];memcpy(saved_modules,modules,sizeof(modules));
        size_t saved_module_count=module_count,saved_export_count=export_count;
        uint32_t saved_error=win64_get_error();int saved_errno=crt_errno,saved_host_errno=errno;
        const char *apis[]={"LoadLibraryA","LoadLibraryW","GetModuleHandleA","GetModuleHandleW"};
        const char *dlls[]={"KERNEL32.dll","KERNELBASE.dll"};
        for(size_t i=0;i<8;++i)for(unsigned api=0;api<4;++api)for(unsigned dll=0;dll<2;++dll)for(unsigned repeat=0;repeat<2;++repeat){
            CPU probe;memset(&probe,0xa5,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.rcx=api&1?(uintptr_t)wide[i]:(uintptr_t)names[i];
            CPU expected=probe;expected.rax=0;expected.rsp+=8;
            win64_set_error(0x5a17);crt_errno=31337;errno=E2BIG;
            if(!win64_dispatch_import(&probe,dlls[dll],apis[api]) || memcmp(&probe,&expected,sizeof(probe)) ||
               win64_get_error()!=126 || crt_errno!=31337 || errno!=E2BIG || module_count!=saved_module_count ||
               export_count!=saved_export_count || memcmp(modules,saved_modules,sizeof(modules)))
                invalid("unavailable lighting module ABI self-test");
        }
        win64_region_remove((uintptr_t)wide);win64_region_remove((uintptr_t)names);
        static const char keys[][96]={"SOFTWARE\\Classes\\CLSID\\{A6519E67-7632-4375-AFDF-CAA889744403}\\ServerBinary",
            "software\\CLASSES\\clsid\\{a6519e67-7632-4375-afdf-caa889744403}\\serverbinary"};
        uint16_t key_wide[2][96]={0};uint64_t output[3];
        for(size_t i=0;i<2;++i)for(size_t j=0;keys[i][j];++j)key_wide[i][j]=(unsigned char)keys[i][j];
        win64_region_add((uintptr_t)key_wide,sizeof(key_wide),0);win64_region_add((uintptr_t)output,sizeof(output),1);
        const char *registry_dlls[]={"ADVAPI32.dll","KERNEL32.dll","KERNELBASE.dll"};
        for(unsigned key=0;key<2;++key)for(unsigned dll=0;dll<3;++dll)for(unsigned high=0;high<2;++high)for(unsigned repeat=0;repeat<2;++repeat){
            output[0]=output[1]=output[2]=UINT64_C(0xa5a5a5a5a5a5a5a5);stack[11]=(uintptr_t)&output[1];
            CPU probe;memset(&probe,0xa5,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.rcx=UINT64_C(0xffffffff80000002);probe.rdx=(uintptr_t)key_wide[key];
            probe.r8=high?UINT64_C(0xa5a5a5a500000000):0;probe.r9=probe.r8|0x20019;
            CPU expected=probe;expected.rax=2;expected.rsp+=8;
            win64_set_error(0x5a17);crt_errno=31337;errno=E2BIG;
            if(!win64_dispatch_import(&probe,registry_dlls[dll],"RegOpenKeyExW") || memcmp(&probe,&expected,sizeof(probe)) ||
               output[1] || output[0]!=UINT64_C(0xa5a5a5a5a5a5a5a5) || output[2]!=output[0] ||
               win64_get_error()!=0x5a17 || crt_errno!=31337 || errno!=E2BIG || module_count!=saved_module_count ||
               export_count!=saved_export_count || memcmp(modules,saved_modules,sizeof(modules)))
                invalid("absent Logitech registration ABI self-test");
        }
        win64_region_remove((uintptr_t)output);win64_region_remove((uintptr_t)key_wide);stack[11]=0;
        win64_set_error(saved_error);crt_errno=saved_errno;errno=saved_host_errno;
        fputs("WIN32_PERIPHERAL_ABSENCE_IMPORT_SELF_TEST dlls=2 dll_cases=128 registry_cases=24 null126=1 key_missing2=1 full_cpu=1 rsp=1 errors=1 no_handles=1 passed\n",stderr);
    }
    {
        uint32_t saved_error=win64_get_error();int saved_errno=crt_errno,saved_host_errno=errno;
        stack[11]=UINT64_C(0x140e1fb90);
        for(unsigned key=0;key<2;++key)for(unsigned base=0;base<2;++base)for(unsigned width=0;width<2;++width){
            CPU probe;memset(&probe,0xa5,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.rcx=key?UINT64_MAX:0;probe.rdx=base?UINT64_MAX:0;probe.r8=0;probe.r9=width?UINT64_MAX:1;
            CPU expected=probe;expected.rax=0;expected.rsp+=8;
            win64_set_error(0x5a17);crt_errno=31337;errno=E2BIG;
            if(!win64_dispatch_import(&probe,"api-ms-win-crt-utility-l1-1-0.dll","bsearch") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=0x5a17 || crt_errno!=31337 || errno!=E2BIG)
                invalid("bsearch empty import ABI self-test");
        }
        stack[11]=0;win64_set_error(saved_error);crt_errno=saved_errno;errno=saved_host_errno;
        fputs("CRT_BSEARCH_IMPORT_SELF_TEST zero_count=1 opaque_key=1 size_t64=1 no_callback=1 full_cpu=1 rsp=1 errors=1 cases=8 passed\n",stderr);
    }
    {
        static const int32_t rects[][4]={{0,0,1,1},{1,0,1,1},{2,0,1,1},{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},{INT32_MAX,0,INT32_MIN,1},{0,1,1,0},{0,0,0,0}};
        static const uint64_t expected_results[]={1,0,1,1,0,1,1,1};
        uint32_t saved_error=win64_get_error();int saved_errno=crt_errno,saved_host_errno=errno;
        win64_region_add((uintptr_t)rects,sizeof(rects),0);
        for(unsigned i=0;i<8;++i){
            CPU probe;memset(&probe,0xa5,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];probe.rcx=i?(uintptr_t)rects[i-1]:0;
            CPU expected=probe;expected.rax=expected_results[i];expected.rsp+=8;
            win64_set_error(0x5a17);crt_errno=31337;errno=E2BIG;
            if(!win64_dispatch_import(&probe,i&1?"uSeR32.DlL":"USER32.dll","IsRectEmpty") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=0x5a17 || crt_errno!=31337 || errno!=E2BIG)
                invalid("IsRectEmpty signed RECT/NULL import ABI self-test");
        }
        win64_region_remove((uintptr_t)rects);win64_set_error(saved_error);crt_errno=saved_errno;errno=saved_host_errno;
        fputs("USER32_ISRECTEMPTY_IMPORT_SELF_TEST null=1 signed_long=1 rect16=1 read_only=1 full_cpu=1 rsp=1 errors=1 cases=8 passed\n",stderr);
    }
    c.rsp=(uint64_t)(uintptr_t)&stack[6]; c.rcx=8; c.rdx=16;
    win64_dispatch_import(&c,"msvcrt.dll","calloc"); uint64_t p=c.rax;
    if (!p || !accessible(p,128,1) || accessible(p+127,2,0)) invalid("heap bounds self-test");
    for (size_t i=0;i<128;++i) if (*(unsigned char *)(uintptr_t)(p+i)) invalid("calloc self-test");
    c.rsp=(uint64_t)(uintptr_t)&stack[6]; c.rcx=p; win64_dispatch_import(&c,"msvcrt.dll","free");
    if (accessible(p,1,0)) invalid("freed win64_pointer self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=0; c.rdx=8192; c.r8=0x3000; c.r9=0x40;
    win64_dispatch_import(&c,"kernel32.dll","VirtualAlloc"); uint64_t vm=c.rax;
    if (!vm || !accessible(vm,8192,1)) invalid("VirtualAlloc self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm; c.rdx=4096; c.r8=2; c.r9=(uintptr_t)&stack[1];
    win64_dispatch_import(&c,"kernel32.dll","VirtualProtect");
    if (!c.rax || (uint32_t)stack[1]!=0x40 || accessible(vm,1,1) || !accessible(vm+4096,4096,1)) invalid("VirtualProtect split self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm; c.rdx=0; c.r8=0x8000;
    win64_dispatch_import(&c,"kernel32.dll","VirtualFree");
    if (!c.rax || accessible(vm+4096,1,0)) invalid("VirtualFree split regions self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=0; c.rdx=8192; c.r8=0x2000; c.r9=4;
    win64_dispatch_import(&c,"kernel32.dll","VirtualAlloc"); vm=c.rax;
    if (!vm || (vm&65535) || accessible(vm,1,0) || !accessible(vm,8192,-1)) invalid("reserve-only self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm; c.rdx=(uintptr_t)stack; c.r8=48;
    win64_dispatch_import(&c,"kernel32.dll","VirtualQuery");
    uint64_t host_page=(uint64_t)sysconf(_SC_PAGESIZE),reserved=(8192+host_page-1)&~(host_page-1);
    if (c.rax!=48 || stack[0]!=vm || stack[1]!=vm || stack[3]!=reserved || (uint32_t)stack[4]!=0x2000 || (uint32_t)(stack[4]>>32)) invalid("reserved VirtualQuery self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm+4096; c.rdx=4096; c.r8=0x1000; c.r9=4;
    win64_dispatch_import(&c,"kernel32.dll","VirtualAlloc");
    if (c.rax!=vm+4096 || accessible(vm,1,0) || !accessible(vm+4096,4096,1)) invalid("commit subrange self-test");
    memset(win64_pointer(vm+4096,4096,1),0xa5,4096);
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm+4096; c.rdx=4096; c.r8=0x4000;
    win64_dispatch_import(&c,"kernel32.dll","VirtualFree");
    if (!c.rax || accessible(vm+4096,1,0)) invalid("decommit self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm+4096; c.rdx=4096; c.r8=0x1000; c.r9=4;
    win64_dispatch_import(&c,"kernel32.dll","VirtualAlloc");
    if (c.rax!=vm+4096) invalid("recommit self-test");
    for (size_t i=0;i<4096;++i) if (*(unsigned char *)(uintptr_t)(vm+4096+i)) invalid("recommit zero-fill self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=vm; c.rdx=0; c.r8=0x8000;
    win64_dispatch_import(&c,"kernel32.dll","VirtualFree");
    if (!c.rax || accessible(vm+4096,1,-1)) invalid("reserve release self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=0x8000;
    win64_dispatch_import(&c,"ucrtbase.dll","_set_fmode");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=(uintptr_t)&stack[1];
    win64_dispatch_import(&c,"ucrtbase.dll","_get_fmode");
    if (c.rax || (uint32_t)stack[1]!=0x8000) invalid("file mode state self-test");
    stack[4]=fn; win64_set_error(123);
    c.rsp=(uintptr_t)&stack[6]; c.rcx=(uintptr_t)&stack[4]; c.rdx=(uintptr_t)&stack[5];
    win64_dispatch_import(&c,"ucrtbase.dll","_initterm_e");
    if (c.rax!=123 || c.rsp!=(uintptr_t)&stack[7]) invalid("initializer shadow space/result self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=image_base; c.rdx=(uintptr_t)stack; c.r8=4;
    win64_dispatch_import(&c,"kernel32.dll","GetModuleFileNameW");
    if (c.rax!=4 || win64_get_error()!=122 || stack[0]!=UINT64_C(0x0000005c003a0043)) invalid("UTF16 module filename truncation self-test");
    c.rsp=(uintptr_t)&stack[6]; win64_dispatch_import(&c,"kernel32.dll","GetProcessHeap"); uint64_t heap=c.rax;
    c.rsp=(uintptr_t)&stack[6]; c.rcx=heap; c.rdx=8; c.r8=32;
    win64_dispatch_import(&c,"kernel32.dll","HeapAlloc"); uint64_t heap_data=c.rax;
    if (!heap_data || memcmp(win64_pointer(heap_data,32,0),(unsigned char[32]){0},32)) invalid("HeapAlloc zero self-test");
    memset(win64_pointer(heap_data,32,1),0x5a,32);
    c.rsp=(uintptr_t)&stack[6]; c.rcx=heap; c.rdx=8; c.r8=heap_data; c.r9=64;
    win64_dispatch_import(&c,"kernel32.dll","HeapReAlloc"); heap_data=c.rax;
    if (!heap_data) invalid("HeapReAlloc self-test");
    for (unsigned i=0;i<64;++i) if (*(unsigned char *)(uintptr_t)(heap_data+i)!=(i<32?0x5a:0)) invalid("HeapReAlloc preserve/zero self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=heap; c.rdx=0; c.r8=heap_data;
    win64_dispatch_import(&c,"kernel32.dll","HeapFree");
    if (!c.rax || accessible(heap_data,1,0)) invalid("HeapFree self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=511; c.rdx=64;
    win64_dispatch_import(&c,"ucrtbase.dll","_aligned_malloc"); uint64_t aligned=c.rax;
    if (!aligned || (aligned&63)) invalid("aligned allocation self-test");
    memset(win64_pointer(aligned,511,1),0xa7,511);
    c.rsp=(uintptr_t)&stack[6]; c.rcx=aligned; c.rdx=1024; c.r8=64;
    win64_dispatch_import(&c,"ucrtbase.dll","_aligned_realloc"); aligned=c.rax;
    if (!aligned || (aligned&63)) invalid("aligned reallocation self-test");
    for (unsigned i=0;i<511;++i) if (*(unsigned char *)(uintptr_t)(aligned+i)!=0xa7) invalid("aligned reallocation preserve self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=aligned;
    win64_dispatch_import(&c,"ucrtbase.dll","_aligned_free");
    if (accessible(aligned,1,0)) invalid("aligned free self-test");
    _Alignas(16) uint64_t slist[2]={UINT64_MAX,UINT64_MAX}; win64_region_add((uintptr_t)slist,sizeof(slist),1);
    c.rsp=(uintptr_t)&stack[6]; c.rcx=(uintptr_t)slist;
    win64_dispatch_import(&c,"kernel32.dll","InitializeSListHead");
    if (slist[0] || slist[1]) invalid("InitializeSListHead self-test");
    win64_region_remove((uintptr_t)slist);
    uint64_t pointer_values[]={0,UINT64_MAX,image_base,fn};
    for (unsigned i=0;i<4;++i) {
        c.rsp=(uintptr_t)&stack[6]; c.rcx=pointer_values[i];
        win64_dispatch_import(&c,"kernel32.dll","EncodePointer");
        if (!i && !c.rax) invalid("EncodePointer cookie self-test");
        c.rsp=(uintptr_t)&stack[6]; c.rcx=c.rax;
        win64_dispatch_import(&c,"kernel32.dll","DecodePointer");
        if (c.rax!=pointer_values[i]) invalid("Encode/DecodePointer roundtrip self-test");
    }
    uint64_t pcs[]={image_base,image_base+0xfff,image_base-1,image_base+0x1000,handle,fn,0,UINT64_MAX};
    for (unsigned i=0;i<sizeof(pcs)/sizeof(pcs[0]);++i) {
        stack[0]=UINT64_MAX; c.rsp=(uintptr_t)&stack[6]; c.rcx=pcs[i]; c.rdx=(uintptr_t)stack;
        win64_dispatch_import(&c,i&1?"ntdll.dll":"kernel32.dll","RtlPcToFileHeader");
        if (c.rax!=(i<2?image_base:0) || stack[0]!=c.rax || c.rsp!=(uintptr_t)&stack[7]) invalid("RtlPcToFileHeader image bounds self-test");
    }
    c.rsp=(uintptr_t)&stack[6]; c.rcx=image_base; c.rdx=(uintptr_t)stack; c.r8=48;
    win64_dispatch_import(&c,"kernel32.dll","VirtualQuery");
    if (c.rax!=48 || (uint32_t)stack[5]!=0x1000000) invalid("loaded PE MEM_IMAGE self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=fn;
    win64_dispatch_import(&c,"kernel32.dll","SetUnhandledExceptionFilter");
    if (c.rax) invalid("initial unhandled exception filter self-test");
    c.rsp=(uintptr_t)&stack[6]; c.rcx=0;
    win64_dispatch_import(&c,"kernel32.dll","SetUnhandledExceptionFilter");
    if (c.rax!=fn || unhandled_exception_filter) invalid("restore unhandled exception filter self-test");
    unsigned char calendar[16];win64_region_add((uintptr_t)calendar,sizeof(calendar),1);
    for (unsigned i=0;i<2;++i) {
        memset(calendar,0xa5,sizeof(calendar));struct timespec before,after;
        if (clock_gettime(CLOCK_REALTIME,&before)) invalid("clock before _time64 self-test");
        c.rsp=(uintptr_t)&stack[6];c.rcx=i?(uintptr_t)(calendar+3):0;
        win64_dispatch_import(&c,"api-ms-win-crt-time-l1-1-0.dll","_time64");
        if (clock_gettime(CLOCK_REALTIME,&after)) invalid("clock after _time64 self-test");
        if ((int64_t)c.rax<before.tv_sec || (int64_t)c.rax>after.tv_sec || c.rsp!=(uintptr_t)&stack[7]) invalid("_time64 epoch/result/stack self-test");
        if (i) { int64_t stored;memcpy(&stored,calendar+3,8);if ((uint64_t)stored!=c.rax) invalid("unaligned _time64 output self-test"); }
        for (unsigned n=0;n<sizeof(calendar);++n) if ((!i || n<3 || n>=11) && calendar[n]!=0xa5) invalid("_time64 output canary self-test");
    }
    win64_region_remove((uintptr_t)calendar);
    uint64_t previous_math_handler=win64_math_error_handler();
    c.rsp=(uintptr_t)&stack[6];c.rcx=fn;win64_dispatch_import(&c,"ucrtbase.dll","__setusermatherr");
    if(win64_math_error_handler()!=fn||c.rsp!=(uintptr_t)&stack[7])invalid("math error handler storage self-test");
    c.rsp=(uintptr_t)&stack[6];c.rcx=previous_math_handler;win64_dispatch_import(&c,"ucrtbase.dll","__setusermatherr");
    if(win64_math_error_handler()!=previous_math_handler)invalid("math error handler restore self-test");
    win64_kernel_self_test();
    win64_threads_self_test();
    win64_sync_self_test();
    win64_stdio_self_test();
    win64_wide_stdio_self_test();
    win64_locale_self_test();
    win64_string_self_test();
    win64_system_self_test();
    win64_scan_self_test();
    win64_utility_self_test();
    win64_newsfeed_self_test();
    win64_window_self_test();
    win64_dinput_self_test();
    win64_accessibility_self_test();
    xinput_disconnected_self_test();
    win64_shell_self_test();
    win64_path_self_test();
    win64_jump_self_test();
    win64_display_self_test();
    win64_math_self_test();
    win64_shader_self_test();
    win64_environment_self_test();
    win64_com_self_test();
    win64_time_self_test();
    win64_dbghelp_self_test();
    win64_error_message_self_test();
    win64_encoding_self_test();
    win64_openal_self_test();
    {
        const uint32_t input[]={UINT32_C(0x41800000),UINT32_C(0x80000000),UINT32_C(0xc2fc0000),UINT32_C(0x42fe0000)};
        const uint32_t output[]={UINT32_C(0x47800000),UINT32_C(0x3f800000),UINT32_C(0x00800000),UINT32_C(0x7f000000)};
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned mode=0;mode<16;++mode)for(unsigned i=0;i<4;++i){
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.mxcsr=UINT32_C(0x1fa5)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
            probe.xmm[0].u32[0]=input[i];expected=probe;expected.xmm[0].u32[0]=output[i];expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"api-ms-win-crt-math-l1-1-0.dll","exp2f") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE || crt_errno!=1927)
                invalid("exp2f exact powers/XMM/CPU/RSP/control/error import self-test");
        }
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("CRT_EXP2F_IMPORT_SELF_TEST exact_powers=1 xmm_low32=1 full_cpu=1 rsp=1 errors=1 controls=16 passed\n",stderr);
    }
    {
        const uint32_t bases[]={0x40000000,0x3f800000,0x3e800000,0x80000000,0x80000000,0,0,0x40000000};
        const uint32_t powers[]={0x80000000,0xc0400000,0x3f800000,0x40400000,0x40000000,0x400ccccd,0,0x40400000};
        const uint32_t outputs[]={0x3f800000,0x3f800000,0x3e800000,0x80000000,0,0,0x3f800000,0x41000000};
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned mode=0;mode<16;++mode)for(unsigned i=0;i<8;++i){
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.mxcsr=UINT32_C(0x1f85)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
            probe.xmm[0].u32[0]=bases[i];probe.xmm[1].u32[0]=powers[i];expected=probe;expected.rsp+=8;
            uint32_t reference=outputs[i];if(i==7){expected.mxcsr|=32u;if(mode&1u)--reference;}
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"api-ms-win-crt-math-l1-1-0.dll","powf"))invalid("powf normal import not handled");
            uint32_t value=probe.xmm[0].u32[0],distance=value>reference?value-reference:reference-value;
            expected.xmm[0].u32[0]=value;
            if(distance>(i==7?4u:0u)||memcmp(&probe,&expected,sizeof(probe))||win64_get_error()!=UINT32_C(0x1357bace)||errno!=ERANGE||crt_errno!=1927)
                invalid("powf normal shortcuts/power/XMM/CPU/RSP/control/error import self-test");
        }
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("CRT_POWF_NORMAL_IMPORT_SELF_TEST shortcuts=1 integral_power=1 ulp4=1 xmm_lanes=1 full_cpu=1 rsp=1 errors=1 controls=16 passed\n",stderr);
    }
    {
        const uint32_t inputs[]={0,0x80000000,0x3e800000,0xbe800000,0x7f812345,0xffc12345};
        const uint32_t outputs[]={0,0x80000000,0x3e82bc2d,0xbe82bc2d,0x7fc12345,0xffc12345};
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned mode=0;mode<16;++mode)for(unsigned i=0;i<6;++i){
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.mxcsr=UINT32_C(0x1f85)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
            probe.xmm[0].u32[0]=inputs[i];expected=probe;expected.rsp+=8;if(i==2||i==3)expected.mxcsr|=32u;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"api-ms-win-crt-math-l1-1-0.dll","tanf"))invalid("tanf import not handled");
            uint32_t value=probe.xmm[0].u32[0],reference=outputs[i],distance=value>reference?value-reference:reference-value;
            expected.xmm[0].u32[0]=value;
            if(distance>((i==2||i==3)?4u:0u)||memcmp(&probe,&expected,sizeof(probe))||win64_get_error()!=UINT32_C(0x1357bace)||errno!=ERANGE||crt_errno!=1927)
                invalid("tanf finite/zero/NaN/XMM/CPU/RSP/control/error import self-test");
        }
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("CRT_TANF_IMPORT_SELF_TEST finite=1 signed_zero=1 nan=1 ulp4=1 xmm_lanes=1 full_cpu=1 rsp=1 errors=1 controls=16 passed\n",stderr);
    }
    {
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        CPU metrics={0};metrics.rcx=0;
        if(!win64_display_call(&metrics,"GetSystemMetrics"))invalid("display policy probe unavailable");
        int configured=metrics.rax!=0;if(!configured)win64_display_configure(960,540,60);
        uint64_t previous_reserved=stack[11];stack[11]=0;
        const uint32_t flags[]={0,2,4,6,4};
        for(unsigned i=0;i<5;++i) {
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];
            probe.rcx=0;probe.rdx=0;probe.r8=i==4;probe.r9=UINT64_C(0xa5a5a5a500000000)|flags[i];
            expected=probe;expected.rax=i==4?UINT32_C(0xfffffffb):0;expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"USER32.dll","ChangeDisplaySettingsExW") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE || crt_errno!=1927)
                invalid("virtual same-mode display import flags/EAX32/CPU/RSP/error self-test");
        }
        stack[11]=previous_reserved;if(!configured)win64_display_configure(0,0,0);
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("WIN32_DISPLAY_MODE_IMPORT_SELF_TEST same_default=1 flags=1 eax32=1 rsp=1 cpu=1 errors=1 passed\n",stderr);
    }
    {
        const uint64_t values[]={UINT64_C(0xa5a5a5a500000001),UINT64_C(0xa5a5a5a500000000),0,UINT64_C(0x80000000)};
        const uint32_t results[]={1,0,UINT32_MAX,0};
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned i=0;i<4;++i) {
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));
            probe.rsp=(uintptr_t)&stack[6];probe.rcx=values[i];
            expected=probe;expected.rax=results[i];expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"USER32.dll","ShowCursor") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE || crt_errno!=1927)
                invalid("ShowCursor import balanced count/low32/EAX32/CPU/RSP/error self-test");
        }
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("WIN32_SHOW_CURSOR_IMPORT_SELF_TEST balanced_count=1 low32=1 eax32=1 rsp=1 cpu=1 errors=1 passed\n",stderr);
    }
    {
        static const uint16_t title[]={0x0046,0x0072,0x006f,0x0073,0x0074,0};
        win64_region_add((uintptr_t)title,sizeof(title),0);
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned i=0;i<2;++i) {
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));
            probe.rsp=(uintptr_t)&stack[6];probe.rcx=i;probe.rdx=(uintptr_t)title;
            expected=probe;expected.rax=1;expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"USER32.dll","SetWindowTextW") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=1400 || errno!=ERANGE || crt_errno!=1927)
                invalid("window title invalid HWND import native BOOL/CPU/RSP/error self-test");
        }
        win64_region_remove((uintptr_t)title);
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("WIN32_TITLE_IMPORT_SELF_TEST invalid_hwnd1400=1 native_bool1=1 rsp=1 cpu=1 errors=1 passed\n",stderr);
    }
    {
        static const uint16_t messages[][4]={{0},{0x0416,0xd83d,0xde42,0}};
        win64_region_add((uintptr_t)messages,sizeof(messages),0);
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned i=0;i<2;++i) {
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));
            probe.rsp=(uintptr_t)&stack[6];probe.rcx=UINT64_C(0xa5a5a5a5ffffffff);
            expected=probe;expected.rax=0;expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,i?"KeRnElBaSe.DlL":"KERNEL32.dll","AttachConsole") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=6 || errno!=ERANGE || crt_errno!=1927)
                invalid("detached parent console import CPU/RSP/low32/error self-test");
            memset(&probe,0xa7,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];probe.rcx=(uintptr_t)messages[i];
            expected=probe;expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,i?"KERNELBASE.dll":"kernel32.dll","OutputDebugStringW") ||
               memcmp(&probe,&expected,sizeof(probe)) || win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE || crt_errno!=1927)
                invalid("UTF16 debug output import CPU/RSP/error self-test");
        }
        win64_region_remove((uintptr_t)messages);
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("WIN32_CONSOLE_IMPORT_SELF_TEST parent_failure6=1 low32=1 debug_utf16=1 rsp=1 cpu=1 errors=1 passed\n",stderr);
    }
    {
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        CPU probe,expected;memset(&probe,0xa7,sizeof(probe));probe.rsp=(uintptr_t)&stack[6];expected=probe;
        struct timespec before,after;
        if(clock_gettime(CLOCK_BOOTTIME,&before))invalid("uptime clock before import self-test");
        win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
        if(!win64_dispatch_import(&probe,"KERNEL32.dll","GetTickCount") ||
           win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE || crt_errno!=1927)
            invalid("uptime import error preservation self-test");
        if(clock_gettime(CLOCK_BOOTTIME,&after))invalid("uptime clock after import self-test");
        uint64_t low=(uint64_t)before.tv_sec*1000+(uint64_t)before.tv_nsec/1000000;
        uint64_t high=(uint64_t)after.tv_sec*1000+(uint64_t)after.tv_nsec/1000000;
        expected.rax=probe.rax;expected.rsp+=8;
        if(high<low || high-low>UINT32_MAX || probe.rax>UINT32_MAX ||
           (uint32_t)((uint32_t)probe.rax-(uint32_t)low)>high-low || memcmp(&probe,&expected,sizeof(probe)))
            invalid("uptime import measurement/DWORD/CPU/RSP self-test");
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fputs("WIN32_UPTIME_IMPORT_SELF_TEST boottime_bracket=1 dword=1 rsp=1 cpu=1 errors=1 passed\n",stderr);
    }
    {
        CPU probe,expected;memset(&probe,0xa7,sizeof(probe));
        probe.rsp=(uintptr_t)&stack[6];probe.rax=UINT64_MAX;probe.rcx=UINT64_MAX;
        probe.rdx=UINT64_MAX;probe.r8=UINT64_C(0x1234567800000000);
        expected=probe;expected.rax=0;expected.rsp+=8;
        uint32_t previous_error=win64_get_error();int previous_errno=errno;
        win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;
        if(!win64_dispatch_import(&probe,"WINMM.DLL","waveOutGetDevCapsW") ||
           memcmp(&probe,&expected,sizeof(probe)) ||
           win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE)
            invalid("WinMM zero-size capability import CPU/RSP/error self-test");
        win64_set_error(previous_error);errno=previous_errno;
        fprintf(stderr,"WINMM_CAPS_IMPORT_SELF_TEST zero_size=1 rsp=1 cpu=1 errors=1 passed\n");
    }
    {
        const char *names[]={"timeBeginPeriod","timeEndPeriod"};
        uint32_t previous_error=win64_get_error();int previous_errno=errno,previous_crt_errno=crt_errno;
        for(unsigned i=0;i<2;++i){
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));
            probe.rsp=(uintptr_t)&stack[6];probe.rcx=UINT64_C(0x1234567800000001);
            expected=probe;expected.rax=0;expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;crt_errno=1927;
            if(!win64_dispatch_import(&probe,"WINMM.DLL",names[i]) ||
               memcmp(&probe,&expected,sizeof(probe)) ||
               win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE || crt_errno!=1927)
                invalid("WinMM timer-period import CPU/RSP/low32/error self-test");
        }
        win64_set_error(previous_error);errno=previous_errno;crt_errno=previous_crt_errno;
        fprintf(stderr,"WINMM_TIMER_IMPORT_SELF_TEST period1_pair=1 low32=1 rsp=1 cpu=1 errors=1 passed\n");
    }
    {
        const uint64_t values[]={UINT64_C(0x1234567800000030),UINT64_C(0x1234567800000039),
                                 UINT64_C(0x123456780000002f),UINT64_C(0x12345678ffffffff)};
        uint32_t previous_error=win64_get_error();int previous_errno=errno;
        for(unsigned i=0;i<4;++i){
            CPU probe,expected;memset(&probe,0xa7,sizeof(probe));
            probe.rsp=(uintptr_t)&stack[6];probe.rcx=values[i];
            expected=probe;expected.rax=i<2?4:0;expected.rsp+=8;
            win64_set_error(UINT32_C(0x1357bace));errno=ERANGE;
            if(!win64_dispatch_import(&probe,"api-ms-win-crt-string-l1-1-0.dll","isdigit") ||
               memcmp(&probe,&expected,sizeof(probe)) ||
               win64_get_error()!=UINT32_C(0x1357bace) || errno!=ERANGE)
                invalid("CRT isdigit import CPU/RSP/low32/error self-test");
        }
        win64_set_error(previous_error);errno=previous_errno;
        fprintf(stderr,"CRT_CTYPE_IMPORT_SELF_TEST isdigit_mask=4 EOF=1 low32=1 cpu=1 rsp=1 errors=1 passed\n");
    }
    region_cache_self_test();
    onexit_registration_self_test();
    c.rsp=(uintptr_t)&stack[6];c.rcx=0;
    win64_dispatch_import(&c,"ucrtbase.dll","_initialize_onexit_table");
    if (c.rax!=UINT64_MAX || c.rsp!=(uintptr_t)&stack[7]) invalid("onexit initialize Win64 return/RSP self-test");
    c.rsp=(uintptr_t)&stack[6];c.rcx=0;c.rdx=0;
    win64_dispatch_import(&c,"ucrtbase.dll","_register_onexit_function");
    if (c.rax!=UINT64_MAX || c.rsp!=(uintptr_t)&stack[7]) invalid("onexit register Win64 return/RSP self-test");
    fprintf(stderr,"WIN64_SELF_TEST modules=1 exports=1 errors=1 heap=1 guest_stack=1 reserve_commit_decommit=1 image_lookup=8 passed\n");
}
