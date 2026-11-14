/* Private-node layout comparison: exact extracted code on host, full C on ARM. */
#if defined(_WIN32)
#include "production_region.inc"
#else
#ifndef REGION_LAYOUT_RELEASE_BENCH
#define WIN64_REGION_CACHE_TEST 1
#endif
#include REGION_LAYOUT_SOURCE
#include <time.h>
#endif
#include <stddef.h>
_Static_assert(sizeof(region)==88 && offsetof(region,owner)==8,"private node size/owner preserved");
#ifdef REGION_LAYOUT_CANDIDATE
_Static_assert(offsetof(region,left)==24 && offsetof(region,right)==32 && offsetof(region,max_end)==40 && offsetof(region,height)==56,"packed traversal prefix");
#else
_Static_assert(offsetof(region,left)==56 && offsetof(region,right)==64 && offsetof(region,max_end)==72 && offsetof(region,height)==80,"actual baseline layout");
#endif
#ifndef REGION_LAYOUT_RELEASE_BENCH
#define main independent_byte_main
#include "region_cache_byte_oracle.c"
#undef main
#define main original_tls_main
#include "tls_body.inc"
#undef main
#else
static uint64_t nanos(void){
#if defined(_WIN32)
 LARGE_INTEGER n,f;QueryPerformanceCounter(&n);QueryPerformanceFrequency(&f);return (uint64_t)((double)n.QuadPart*1e9/f.QuadPart);
#else
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
#endif
}
#endif
static void layout_assert(int valid,const char *why){if(!valid){fprintf(stderr,"REGION_LAYOUT_FAIL %s\n",why);abort();}}
static unsigned node_invariants(region *r,uint64_t *end,size_t *count){
 if(!r){*end=0;return 0;}uint64_t le,re;unsigned lh=node_invariants(r->left,&le,count),rh=node_invariants(r->right,&re,count);++*count;
 uint64_t want=r->base+r->size;if(le>want)want=le;if(re>want)want=re;
 layout_assert(r->height==1+(lh>rh?lh:rh)&&lh<=rh+1&&rh<=lh+1&&r->max_end==want,"exact subtree augmentation/balance");*end=want;return r->height;
}
static void full_invariants(void){
 metadata_lock();size_t count=0,list=0,owners=0;uint64_t end;node_invariants(region_index,&end,&count);region *prior=NULL;
 for(region*r=regions;r;r=r->next){layout_assert(r->previous==prior,"previous links");prior=r;++list;}
 for(size_t i=0;i<region_owner_capacity;++i)for(region*r=region_owners[i];r;r=r->owner_next){layout_assert(metadata_bucket(r->owner,region_owner_capacity)==i,"owner bucket identity");++owners;layout_assert(owners<=region_index_count,"owner chain acyclic");}
 layout_assert(count==region_index_count&&list==count&&owners==count,"tree/list/owner counts equal");metadata_unlock();
}
typedef struct{uint64_t base,checks,checksum;} layout_job;
static void *parallel_layout(void *opaque){layout_job*j=opaque;uint64_t sum=0;
 for(unsigned k=0;k<1024;++k){uint64_t b=j->base+(k&15)*512;win64_region_add(b,256,1);layout_assert(accessible(b,256,1),"parallel map RW");
  win64_region_add(b+32,16,0);layout_assert(!accessible(b+24,32,1)&&accessible(b+24,32,0),"parallel newest RO overlay");win64_region_remove(b+32);
  protection_set(b+96,16,2,0);layout_assert(!accessible(b+96,1,1)&&accessible(b+96,1,0),"parallel split RO");
  protection_set(b+96,16,4,0x1000);layout_assert(accessible(b,256,1),"parallel split restore");win64_region_remove(b);layout_assert(!accessible(b,1,0),"parallel stale mapping revoked");
  j->checks+=7;sum=sum*UINT64_C(6364136223846793005)+(k+1);
 }j->checksum=sum;return NULL;
}
static void parallel_matrix(uint64_t *checks,uint64_t *checksum){
 pthread_t threads[4];layout_job jobs[4];for(unsigned i=0;i<4;++i){jobs[i]=(layout_job){UINT64_C(0x7100000000)+i*UINT64_C(0x100000),0,0};layout_assert(!pthread_create(&threads[i],NULL,parallel_layout,&jobs[i]),"parallel thread start");}
 for(unsigned i=0;i<4;++i){layout_assert(!pthread_join(threads[i],NULL),"parallel join");*checks+=jobs[i].checks;*checksum=*checksum*UINT64_C(6364136223846793005)+jobs[i].checksum+i+1;}full_invariants();layout_assert(!regions&&!region_index_count,"parallel leaves no live regions");
}
typedef struct{const char*name;unsigned background,iterations;uint64_t ns,checksum;} benchmark_result;
static benchmark_result bench(unsigned mode){
 unsigned n=mode?32768:4096,iterations=mode==2?12000:40000;uint64_t start_base=UINT64_C(0x5000000000);uint32_t rng=0x12345678;uint64_t sum=0;
 for(unsigned i=0;i<n;++i)win64_region_add(start_base+(uint64_t)i*4096,128,1);full_invariants();
 uint64_t started=nanos();
 for(unsigned i=0;i<iterations;++i){rng=rng*1664525u+1013904223u;unsigned at=mode?(rng&(n-1)):n/2;uint64_t b=start_base+(uint64_t)at*4096+512;
  win64_region_add(b,128,1);int a=accessible(b,128,1),v=accessible(b+16,16,0),revoked;
  if(mode==2){win64_region_add(b+32,16,0);layout_assert(!accessible(b+24,32,1)&&accessible(b+24,32,0),"benchmark overlap bounds");win64_region_remove(b+32);
   protection_set(b+64,16,2,0);layout_assert(!accessible(b+64,16,1),"benchmark split permission");protection_set(b+64,16,4,0x1000);}
  win64_region_remove(b);revoked=!accessible(b,1,0);layout_assert(a&&v&&revoked,"whole add validate remove answers");sum=sum*UINT64_C(6364136223846793005)+(unsigned)(a+2*v+4*revoked)+i;
 }uint64_t elapsed=nanos()-started;full_invariants();
 for(unsigned i=0;i<n;++i)win64_region_remove(start_base+(uint64_t)i*4096);full_invariants();layout_assert(!regions&&!region_index_count,"benchmark teardown");
 return(benchmark_result){mode==0?"compact_same_address":mode==1?"large_random_addresses":"large_random_overlap_split",n,iterations,elapsed,sum};
}
#ifdef REGION_LAYOUT_SHARED
__attribute__((visibility("default"))) int region_layout_entry(int argc,char**argv){
#else
int main(int argc,char**argv){
#endif
 if(argc==2&&!strcmp(argv[1],"--unknown-owner")){win64_region_remove(UINT64_C(0xabc000));return 99;}
 if(argc==2&&!strcmp(argv[1],"--bad-geometry")){win64_region_add(UINT64_MAX-1,8,1);return 99;}
 if(argc==2&&!strcmp(argv[1],"--epoch-overflow")){atomic_store(&region_epoch,UINT64_MAX-1);win64_region_mutation_begin();return 99;}
 layout_assert(argc==1,"fixture arguments");uint64_t parallel_checks=0,parallel_checksum=0;
#ifndef REGION_LAYOUT_RELEASE_BENCH
 layout_assert(independent_byte_main()==0,"independent array byte oracle");region_owners=NULL;region_owner_capacity=0;
 layout_assert(original_tls_main(1,argv)==0,"original TLS cache/race suite");full_invariants();parallel_matrix(&parallel_checks,&parallel_checksum);
#endif
 benchmark_result results[3];for(unsigned i=0;i<3;++i)results[i]=bench(i);
 printf("{\"success\":true,\"record_bytes\":%zu,\"owner_offset\":%zu,\"traversal_prefix_bytes\":%zu,\"parallel_checks\":%"PRIu64",\"parallel_checksum\":%"PRIu64",\"whole_add_validate_remove\":[",sizeof(region),offsetof(region,owner),offsetof(region,max_end)+sizeof(uint64_t),parallel_checks,parallel_checksum);
 for(unsigned i=0;i<3;++i){benchmark_result*r=&results[i];printf("%s{\"name\":\"%s\",\"background\":%u,\"iterations\":%u,\"ns\":%"PRIu64",\"checksum\":%"PRIu64"}",i?",":"",r->name,r->background,r->iterations,r->ns,r->checksum);}
 printf("],\"benchmark_ranges_metadata_only\":true,\"game_fps_proven\":false}\n");return 0;
}
