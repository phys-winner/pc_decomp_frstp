/* Includes exact extracted baseline/candidate region code, never guest code. */
#include "production_region.inc"
#include <time.h>
static void avl_require(int value,const char *why){if(!value){fprintf(stderr,"AVL_PROOF_FAILURE %s\n",why);exit(1);}}
static uint64_t avl_nanos(void){
#if defined(_WIN32)
    LARGE_INTEGER now,hz;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&hz);
    return (uint64_t)((double)now.QuadPart*1e9/hz.QuadPart);
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
#endif
}
#if defined(FP_AVL_BENCH_LIBRARY)
#if defined(_WIN32)
#define AVL_EXPORT __declspec(dllexport)
#else
#define AVL_EXPORT __attribute__((visibility("default")))
#endif
/* Whole registration/validation/release costs, including the unchanged malloc,
 * mutex, epoch, owner hash, TLS snapshots and overlap fallback. Occupancies are
 * synthetic sensitivity cases, not recovered game region counts. */
AVL_EXPORT int fp_avl_cycle(unsigned occupancy,unsigned mode,unsigned iterations,uint64_t *elapsed,uint64_t *digest){
    if(!occupancy||occupancy>32768||mode>3||!iterations||regions)return 1;
    const uint64_t base=UINT64_C(0x100000000);
    for(unsigned i=0;i<occupancy;++i)win64_region_add(base+(uint64_t)i*4096,256,(int)(i&1));
    /* Cold-start eviction is after resident setup and outside timing. Each
     * cold-case validation also starts with an empty TLS snapshot cache. */
    static volatile unsigned char eviction[33554432];
    if(mode==3)for(size_t j=0;j<sizeof(eviction);j+=64)eviction[j]=(unsigned char)j;
    uint64_t sum=UINT64_C(0x706f6f6c);uint64_t start=avl_nanos();
    for(unsigned i=0;i<iterations;++i){
        unsigned slot=(unsigned)(((uint64_t)i*104729+17)%occupancy);
        uint64_t resident=base+(uint64_t)slot*4096,address=resident+(mode==2?32:512);
        int writable=(int)(i&1);win64_region_add(address,64,writable);
        if(mode==3)region_cache_count=0;
        int read=accessible(address,64,0),write=accessible(address,64,1);
        avl_require(read==1&&write==writable,"whole cycle new attributes");
        sum=sum*33+(unsigned)read*3+(unsigned)write;
        if(mode==0){
            int warm=accessible(address+8,8,writable);avl_require(warm,"whole cycle warm snapshot");sum=sum*33+(unsigned)warm;
        }else if(mode==1&&!(i&15)){
            protection_set(address+16,16,2,0);
            avl_require(!accessible(address+16,16,1)&&accessible(address+16,16,0),"whole cycle protection split");
            protection_set(address+32,16,0,0x2000);
            avl_require(!accessible(address+32,16,0)&&accessible(address+32,16,-1),"whole cycle reserve split");sum=sum*33+7;
        }else if(mode==2){
            int crossing=accessible(resident+24,80,1);
            avl_require(crossing==((slot&1)&&writable),"whole cycle crossing priority");sum=sum*33+(unsigned)crossing;
        }
        win64_region_remove(address);
        int after_read=accessible(address,64,0),after_write=accessible(address,64,1);
        avl_require(after_read==(mode==2)&&after_write==(mode==2&&(slot&1)),"whole cycle removal invalidates/reveals");
        sum=sum*33+(unsigned)after_read*3+(unsigned)after_write;
        avl_require(accessible(resident,16,1)==(int)(slot&1),"whole cycle retained resident");
    }
    *elapsed=avl_nanos()-start;*digest=sum;
    avl_require(region_index_count==occupancy,"whole cycle owner fragments cleaned");
    while(regions)win64_region_remove(regions->owner);
    avl_require(!region_index&&!region_cache_count,"whole cycle final empty");return 0;
}
#else
#define main avl_byte_oracle_main
#include "region_cache_byte_oracle.c"
#undef main
static uint64_t structural_mutations;
static int structural_deferred;
static void add_key(unsigned key,size_t size){win64_region_add(UINT64_C(0x20000000)+(uint64_t)key*1024,size,1);if(!structural_deferred)index_check();++structural_mutations;}
static void remove_key(unsigned key){win64_region_remove(UINT64_C(0x20000000)+(uint64_t)key*1024);if(!structural_deferred)index_check();++structural_mutations;}
static void empty_tree(void){while(regions){win64_region_remove(regions->owner);++structural_mutations;}index_check();}
static void rotations(void){
    /* A child root rotates while both augments stay equal. A parent must still
     * store the new pointer before deciding that propagation can stop. */
    const unsigned insertion[]={50,20,70,30};
    for(unsigned i=0;i<4;++i)add_key(insertion[i],1);
    avl_require(region_index->base==UINT64_C(0x20000000)+50*1024,"insert precondition root");
    add_key(25,1);
    avl_require(region_index->left->base==UINT64_C(0x20000000)+25*1024,"insert assigns rotated child with equal augments");empty_tree();
    const unsigned deletion[]={60,20,80,10,30,70,90,25,40};
    for(unsigned i=0;i<9;++i)add_key(deletion[i],1);
    avl_require(region_index->base==UINT64_C(0x20000000)+60*1024,"remove precondition root");
    remove_key(10);
    avl_require(region_index->left->base==UINT64_C(0x20000000)+30*1024,"remove assigns rotated child with equal augments");empty_tree();
}
static void maximum_change(void){
    const unsigned keys[]={50,20,70,10};for(unsigned i=0;i<4;++i)add_key(keys[i],1);
    unsigned height=region_index->left->height;add_key(30,100*1024);
    avl_require(region_index->left->height==height,"maximum-only propagation precondition");
    avl_require(region_index->max_end==UINT64_C(0x20000000)+130*1024,"changed max must propagate at equal height");
    remove_key(30);empty_tree();
}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--bad-geometry")){win64_region_add(123,0,1);return 99;}
    if(argc==2&&!strcmp(argv[1],"--bad-overflow")){win64_region_add(UINT64_MAX-7,8,1);return 99;}
    if(argc==2&&!strcmp(argv[1],"--bad-owner")){win64_region_remove(123);return 99;}
    if(argc==2&&!strcmp(argv[1],"--bad-tree")){region r={.base=1,.size=1};region_tree_remove(NULL,&r);return 99;}
    if(argc==2&&!strcmp(argv[1],"--bad-mutation")){win64_region_mutation_end();return 99;}
    if(argc==2&&!strcmp(argv[1],"--bad-epoch")){atomic_store(&region_epoch,UINT64_MAX-1);win64_region_mutation_begin();return 99;}
    rotations();if(argc==2&&!strcmp(argv[1],"--rotations-only"))return 0;
    maximum_change();if(argc==2&&!strcmp(argv[1],"--maximum-only"))return 0;
    /* Realistically deeper trees, permuted insert/remove/reuse, overlapping
     * long records and split owners. Independently check the list/AVL/hash
     * bijection at each checkpoint, not just the cached access answers. */
    structural_deferred=1;
    for(unsigned i=0;i<1024;++i)add_key((i*541)&1023,32+(i%7)*1024);
    index_check();
    for(unsigned i=0;i<4096;++i){unsigned key=(i*397)&1023;
        remove_key(key);add_key(key,32+(i%11)*1024);
        uint64_t p=UINT64_C(0x20000000)+(uint64_t)key*1024;
        if(!(i&31)){protection_set(p+8,8,2,0);index_check();++structural_mutations;
            avl_require(!accessible(p+8,8,1)&&accessible(p+8,8,0),"split newest-first access");}
    }
    empty_tree();uint64_t structure_passes=index_checks;
    free(region_owners);region_owners=NULL;region_owner_capacity=0;index_checks=0;
    avl_require(!avl_byte_oracle_main(),"independent byte oracle");
    printf("{\"success\":true,\"structural_mutations\":%"PRIu64",\"structural_invariant_passes\":%"PRIu64",\"rotation_child_assignment\":true,\"max_end_only_propagation\":true,\"legacy_queries\":%"PRIu64",\"extra_queries\":%"PRIu64",\"oracle_invariant_passes\":%"PRIu64",\"game_gain_proven\":false}\n",structural_mutations,structure_passes,cases,extra_cases,index_checks);return 0;
}
#endif
