/* Link only the actual production region/cache self-test. Section GC discards
 * unrelated Win64 imports; no guest CPU or game entry is executed here. */
#include "../android/host/win64.c"
#include <sys/wait.h>
static atomic_int fail_next_malloc;
static atomic_int fail_next_realloc;
static atomic_int fail_next_calloc;
void *__real_malloc(size_t n);
void *__real_realloc(void *p,size_t n);
void *__real_calloc(size_t count,size_t size);
void *__wrap_malloc(size_t n) {
    if (atomic_exchange(&fail_next_malloc,0)) { errno=ENOMEM; return NULL; }
    return __real_malloc(n);
}
void *__wrap_realloc(void *p,size_t n) {
    if(atomic_exchange(&fail_next_realloc,0)){errno=ENOMEM;return NULL;}
    return __real_realloc(p,n);
}
void *__wrap_calloc(size_t count,size_t size) {
    if(atomic_exchange(&fail_next_calloc,0)){errno=ENOMEM;return NULL;}
    return __real_calloc(count,size);
}
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned n) {
    uint64_t value=0;if(offset+n>sizeof(fixture_teb)||n>8)abort();memcpy(&value,fixture_teb+offset,n);return value;
}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned n) {
    if(offset+n>sizeof(fixture_teb)||n>8)abort();memcpy(fixture_teb+offset,&value,n);
}
#ifdef WIN64_REGION_PROFILE
static void profile_fixture(void) {
    unsigned char data[96],many[REGION_CACHE_CAPACITY+1][16];uint64_t base=(uintptr_t)data;
    metadata_lock();memset(&region_profile,0,sizeof(region_profile));metadata_unlock();
    win64_region_add(base,96,1);if(!accessible(base+1,1,1)||!accessible(base+2,1,1))abort();
    win64_region_add(base+16,16,0);if(!accessible(base+1,1,1)||accessible(base+16,1,1)||!accessible(base+18,1,0))abort();
    metadata_lock();protection_set(base+64,8,1,0);metadata_unlock();
    win64_region_remove(base);if(!accessible(base+16,1,0))abort();win64_region_remove(base+16);
    if(region_profile.queries!=6||region_profile.hits!=2||region_profile.misses!=4||region_profile.linear_calls!=1||
       region_profile.linear_nodes!=1||region_profile.linear_max!=1||region_profile.index_fast!=3||region_profile.index_fallback!=1||region_profile.index_gap||
       !region_profile.index_search_steps||region_profile.index_search_steps>32||region_profile.index_rebuilds||region_profile.index_insert_moves||region_profile.index_remove_scans||region_profile.insertions!=4||region_profile.evictions||
       region_profile.invalidate_calls!=6||region_profile.invalidated!=2||region_profile.protect_calls!=1||region_profile.protect_cleared!=2||
       region_profile.remove_calls!=2||region_profile.remove_nodes<4||region_profile.remove_nodes>5||region_profile.remove_max<3||region_profile.remove_max>4||region_profile.remove_records!=4||
       region_profile.live||region_profile.peak!=4) {fputs("PROFILE_KNOWN_COUNTS_FAIL\n",stderr);abort();}
    metadata_lock();memset(&region_profile,0,sizeof(region_profile));metadata_unlock();
    for(unsigned i=0;i<REGION_CACHE_CAPACITY+1;++i)win64_region_add((uintptr_t)many[i],16,1);
    errno=1927;win64_set_error(0x5a17);
    for(unsigned i=0;i<65536;++i)if(!accessible((uintptr_t)many[i%(REGION_CACHE_CAPACITY+1)],1,0))abort();
    if(errno!=1927||win64_get_error()!=0x5a17)abort();
    if(region_profile.queries!=65536||region_profile.hits||region_profile.misses!=65536||
       region_profile.linear_calls||region_profile.linear_nodes||region_profile.linear_max||region_profile.index_fast!=65536||region_profile.index_fallback||region_profile.index_gap||
       !region_profile.index_search_steps||region_profile.index_search_steps>65536*8||region_profile.insertions!=65536||region_profile.evictions!=65472||region_profile.live!=65)abort();
    pid_t child=fork();if(child<0)abort();if(!child){close(STDERR_FILENO);errno=1927;region_profile_emit();_Exit(errno==1927?0:2);}
    int status=0;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))abort();
    for(unsigned i=0;i<REGION_CACHE_CAPACITY+1;++i)win64_region_remove((uintptr_t)many[i]);
    metadata_lock();memset(&region_profile,0,sizeof(region_profile));metadata_unlock();
}
#endif
static size_t index_tree_check(region *r,region *low,region *high) {
    if(!r)return 0;
    if((low&&!region_before(low,r))||(high&&!region_before(r,high)))abort();
    size_t count=1+index_tree_check(r->left,low,r)+index_tree_check(r->right,r,high);
    unsigned left=region_height(r->left),right=region_height(r->right);
    uint64_t end=r->base+r->size;
    if(region_max_end(r->left)>end)end=region_max_end(r->left);
    if(region_max_end(r->right)>end)end=region_max_end(r->right);
    if(r->height!=1+(left>right?left:right)||left>right+1||right>left+1||r->max_end!=end)abort();
    return count;
}
static void index_fixture(void) {
    if(regions||region_index_count)abort();
    const uint64_t base=UINT64_C(0x10000000);const unsigned count=10000;
    for(unsigned i=0;i<count;++i)win64_region_add(base+i*32,16,i&1);
    if(region_index_count!=count)abort();
    if(index_tree_check(region_index,NULL,NULL)!=count)abort();
    for(region *r=regions,*previous=NULL;r;previous=r,r=r->next){
        if(r->previous!=previous)abort();size_t copies=0;
        for(region *p=region_owners[metadata_bucket(r->owner,region_owner_capacity)];p;p=p->owner_next)copies+=p==r;
        if(copies!=1)abort();
    }
    for(unsigned i=0;i<count;++i){
        uint64_t p=base+i*32;int want=(int)(i&1);
        if(!accessible(p,16,0)||accessible(p,16,1)!=want||accessible(p+16,1,0))abort();
        if(accessible(p,16,1)!=accessible_linear(p,16,1,0))abort();
    }
    /* A genuine cold lookup must not allocate index storage or a region. */
    atomic_store(&fail_next_malloc,1);atomic_store(&fail_next_realloc,1);atomic_store(&fail_next_calloc,1);
    metadata_lock();region_cache_count=0;metadata_unlock();
    if(!accessible(base,1,0)||!atomic_load(&fail_next_malloc)||!atomic_load(&fail_next_realloc)||!atomic_load(&fail_next_calloc))abort();
    atomic_store(&fail_next_malloc,0);atomic_store(&fail_next_realloc,0);atomic_store(&fail_next_calloc,0);
    for(unsigned i=0;i<count;++i)win64_region_remove(base+i*32);
    if(regions||region_index_count||region_cache_count)abort();
    /* A short predecessor must not hide an earlier long covering interval. */
    win64_region_add(base,96,1);win64_region_add(base+8,8,0);
    if(!accessible(base+40,8,1)||accessible(base+8,8,1))abort();
    win64_region_add(base,4,0);
    if(accessible(base,1,1)||!accessible(base+4,4,1)||accessible(base+4,8,1))abort();
    win64_region_remove(base);if(!accessible(base+8,8,0)||accessible(base+8,8,1)||accessible(base+40,1,0))abort();
    win64_region_remove(base+8);
    if(regions||region_index_count||region_cache_count)abort();
}
int main(void) {
    pid_t first=fork();if(first<0)return 10;
    if(!first){atomic_store(&fail_next_calloc,1);win64_region_add(UINT64_C(0x20000000),16,1);_Exit(99);}
    int first_status;if(waitpid(first,&first_status,0)!=first||!WIFEXITED(first_status)||WEXITSTATUS(first_status)!=73)return 11;
#ifdef WIN64_REGION_PROFILE
    profile_fixture();
#endif
    region_cache_self_test();
    index_fixture();
    uint64_t pointer=win64_alloc(32,0);if(!pointer||!accessible(pointer,8,1))return 2;
    atomic_store(&fail_next_malloc,1);
    if(!accessible(pointer+8,8,1)||!atomic_load(&fail_next_malloc))return 3;
    if(win64_alloc(16,0)||win64_get_error()!=8||atomic_load(&fail_next_malloc)||!accessible(pointer,32,1))return 4;
    pid_t child=fork();if(child<0)return 5;
    if(!child){atomic_store(&fail_next_malloc,1);win64_region_add(pointer,16,1);_Exit(99);}
    int status=0;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=73)return 6;
    child=fork();if(child<0)return 12;
    if(!child){uint64_t base=UINT64_C(0x20000000);size_t i=0;
        while(region_index_count<region_owner_capacity)win64_region_add(base+(i++)*32,16,1);
        atomic_store(&fail_next_calloc,1);win64_region_add(base+i*32,16,1);_Exit(99);}
    if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=73)return 13;
    child=fork();if(child<0)return 14;
    if(!child){while(allocation_count<allocation_capacity)if(!win64_alloc(16,0))abort();atomic_store(&fail_next_calloc,1);win64_alloc(16,0);_Exit(99);}
    if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=73)return 15;
    win64_free(pointer);if(accessible(pointer,8,0))return 7;
    pointer=win64_alloc(16,0);if(!pointer||!accessible(pointer,16,1))return 8;win64_free(pointer);
#ifdef WIN64_REGION_PROFILE
    if(region_profile.allocation_calls!=2||region_profile.allocation_nodes!=2||region_profile.allocation_max!=1||
       region_profile.forget_calls!=2||region_profile.forget_nodes!=2||region_profile.forget_max!=1)return 9;
#endif
    printf("{\"success\":true,\"cache_entries\":%u,\"genuine_eviction_records\":%u,\"single_pass\":true,\"clipped_segments\":3,\"address_index_records\":10000,\"intrusive_AVL\":true,\"region_record_bytes\":%zu,\"AVL_height_and_max_end\":true,\"owner_hash_previous_links\":true,\"ambiguous_priority_fallback\":true,\"owner_duplicates_removed\":true,\"concurrent_reference_checks\":10000,\"accessible_allocation_free\":true,\"allocation_failure_preserves_existing_region\":true,\"metadata_OOM_child_exit73\":true,\"owner_calloc_initial_and_growth_exit73\":true,\"allocation_index_growth_exit73\":true,\"game_boot_proven\":false}\n",(unsigned)REGION_CACHE_CAPACITY,(unsigned)REGION_CACHE_CAPACITY+2,sizeof(region));
    return 0;
}
