/* Exact existing byte/priority/race suite plus locked-history-specific checks. */
#include "layout_body.inc"

#ifndef REGION_LAYOUT_RELEASE_BENCH
static uint64_t journal_checks,journal_policy_checks;
static void jcheck(int valid,const char *why){++journal_checks;layout_assert(valid,why);}
#ifdef REGION_JOURNAL_CANDIDATE
#define JP(valid,why) do{++journal_policy_checks;layout_assert(valid,why);}while(0)
#else
#define JP(valid,why) ((void)0)
#endif
static void sync_now(void){
 metadata_lock();uint64_t v=atomic_load_explicit(&region_epoch,memory_order_acquire);
#ifdef REGION_JOURNAL_CANDIDATE
 region_cache_sync(v);
#else
 if(region_cache_epoch!=v){region_cache_count=0;region_cache_epoch=v;}
#endif
 metadata_unlock();
}
static void known_begin(uint64_t low,uint64_t high){
#ifdef REGION_JOURNAL_CANDIDATE
 region_mutation_start(1,low,high);
#else
 (void)low;(void)high;win64_region_mutation_begin();
#endif
}
static void set_attribute(uint64_t base,uint32_t protection){
 region *r=regions;while(r&&r->base!=base)r=r->next;
 layout_assert(r!=NULL,"attribute fixture record exists");r->protection=protection;
}
typedef struct {atomic_uint command,ack;unsigned action,pairs;uint64_t base,other;} journal_job;
static void *journal_mutator(void *opaque){
 journal_job *j=opaque;unsigned seen=0;
 for(;;){unsigned c;while((c=atomic_load_explicit(&j->command,memory_order_acquire))==seen)sched_yield();
  if(c==UINT32_MAX)break;
  switch(j->action){
   case 1:win64_region_add(j->base,128,1);break;
   case 2:win64_region_add(j->base,16,0);break;
   case 3:win64_region_remove(j->base);break;
   case 4:win64_region_mutation_begin();win64_region_mutation_end();break;
   case 5:win64_region_mutation_begin();set_attribute(j->base,2);win64_region_mutation_end();break;
   case 6:known_begin(j->base,j->base+128);win64_region_mutation_begin();set_attribute(j->other,2);win64_region_mutation_end();win64_region_mutation_end();break;
   case 7:known_begin(j->base,j->base+128);win64_region_add(j->base,128,1);win64_region_mutation_end();break;
   case 8:win64_region_mutation_begin();win64_region_add(j->base,128,1);win64_region_mutation_end();break;
   case 9:for(unsigned k=0;k<j->pairs;++k){win64_region_add(j->base,128,1);win64_region_remove(j->base);}break;
   default:abort();
  }
  seen=c;atomic_store_explicit(&j->ack,c,memory_order_release);
 }
 return NULL;
}
static void mutate(journal_job *j,unsigned action,uint64_t base,uint64_t other,unsigned pairs){
 unsigned c=atomic_load_explicit(&j->command,memory_order_relaxed)+1;
 j->action=action;j->base=base;j->other=other;j->pairs=pairs;
 atomic_store_explicit(&j->command,c,memory_order_release);
 while(atomic_load_explicit(&j->ack,memory_order_acquire)!=c)sched_yield();
}
static journal_job *known_race_job;
static uint64_t known_race_address;
static void known_race_probe(void){region_fast_probe=NULL;mutate(known_race_job,2,known_race_address,0,0);}
static void warm_ranges(uint64_t base,unsigned count){
 for(unsigned i=0;i<count;++i)jcheck(accessible(base+i*256,128,1),"warm stable ranges");
 jcheck(region_cache_count==count,"exact warm cache population");
}
static void history_matrix(void){
 const uint64_t stable=UINT64_C(0x6800000000),other=stable+UINT64_C(0x100000);
 for(unsigned i=0;i<64;++i)win64_region_add(stable+i*256,128,1);
 journal_job job={0};pthread_t worker;jcheck(!pthread_create(&worker,NULL,journal_mutator,&job),"journal worker starts");
 warm_ranges(stable,64);mutate(&job,1,other,0,0);sync_now();JP(region_cache_count==64,"disjoint add retains all snapshots");
 for(unsigned i=0;i<64;++i)jcheck(accessible(stable+i*256,128,1),"disjoint add authority unchanged");
 mutate(&job,3,other,0,0);sync_now();JP(region_cache_count==64,"disjoint remove retains all snapshots");
 /* Half-open adjacent endpoints must not invalidate a safe clipped snapshot. */
 mutate(&job,2,stable+128,0,0);sync_now();JP(region_cache_count==64,"adjacent span does not intersect");
 jcheck(accessible(stable,128,1),"adjacent RO does not change prior region");mutate(&job,3,stable+128,0,0);
 warm_ranges(stable,64);mutate(&job,2,stable+32,0,0);sync_now();JP(region_cache_count==63,"overlap drops precisely intersecting snapshot");
 jcheck(!accessible(stable+24,32,1)&&accessible(stable+24,32,0),"newest partial RO overlay priority");
 jcheck(accessible(stable+256,128,1),"disjoint snapshot still RW");mutate(&job,3,stable+32,0,0);
 jcheck(accessible(stable,128,1),"removing overlay restores original authority");
 /* Unknown mutation revokes all, including direct post-add attribute changes. */
 warm_ranges(stable,64);mutate(&job,5,stable,0,0);sync_now();jcheck(!region_cache_count,"public unknown clears all snapshots");
 jcheck(!accessible(stable,128,1)&&accessible(stable,128,0),"unknown attribute change revalidated");
 protection_set(stable,128,4,0x1000);warm_ranges(stable,64);
 mutate(&job,6,other,stable,0);sync_now();jcheck(!region_cache_count,"nested unknown poisons known outer");
 jcheck(!accessible(stable,128,1),"nested unknown outside nominal span revokes RW");
 protection_set(stable,128,4,0x1000);warm_ranges(stable,64);
 mutate(&job,7,other,0,0);sync_now();jcheck(!region_cache_count,"nested controlled add conservatively clears");mutate(&job,3,other,0,0);
 warm_ranges(stable,64);mutate(&job,8,other,0,0);sync_now();jcheck(!region_cache_count,"unknown outer cannot become known");mutate(&job,3,other,0,0);
 /* Exactly sixteen records are replayable; eighteen overwrite history. */
 warm_ranges(stable,64);mutate(&job,9,other,0,8);sync_now();JP(region_cache_count==64,"exact full history retained");
 for(unsigned i=0;i<64;++i)jcheck(accessible(stable+i*256,128,1),"full history authority unchanged");
 mutate(&job,9,other,0,9);sync_now();jcheck(!region_cache_count,"history overrun clears all");
 warm_ranges(stable,64);mutate(&job,4,0,0,0);mutate(&job,9,other,0,1);sync_now();jcheck(!region_cache_count,"unknown middle history clears all");
 /* Split an owner into discontiguous records, warm every fragment, then remove
  * by the original owner. Removal must journal ALL fragments before freeing. */
 const uint64_t split=other+4096;win64_region_add(split,1024,1);protection_set(split+256,256,2,0);
 jcheck(accessible(split,128,1)&&accessible(split+256,128,0)&&accessible(split+768,128,1),"split fragments warm");
 mutate(&job,3,split,0,0);sync_now();
 jcheck(!accessible(split,1,0)&&!accessible(split+256,1,0)&&!accessible(split+768,1,0),"all removed owner fragments revoked");
 /* Same-address RO/RW reuse is observed from the reader, never dereferenced. */
 mutate(&job,2,other,0,0);jcheck(!accessible(other,16,1)&&accessible(other,16,0),"reused address initially RO");
 mutate(&job,3,other,0,0);mutate(&job,1,other,0,0);jcheck(accessible(other,128,1),"same-address RW replacement");mutate(&job,3,other,0,0);
 /* The journal does not bypass the second atomic observation, even if the
  * intervening writer is a controlled known-range mutation. */
 known_race_job=&job;known_race_address=stable+32;jcheck(accessible(stable+32,16,1),"known race warm RW");
 region_fast_probe=known_race_probe;jcheck(!accessible(stable+32,16,1),"known add between double epoch revokes old RW");mutate(&job,3,stable+32,0,0);
#ifdef REGION_JOURNAL_CANDIDATE
 warm_ranges(stable,64);mutate(&job,1,other,0,0);
 metadata_lock();uint64_t v=atomic_load(&region_epoch);region_journal[(v>>1)&15].epoch=0;metadata_unlock();sync_now();JP(!region_cache_count,"missing exact history record clears");mutate(&job,3,other,0,0);
 warm_ranges(stable,64);mutate(&job,1,other,0,0);
 metadata_lock();v=atomic_load(&region_epoch);region_journal[(v>>1)&15].high=region_journal[(v>>1)&15].low;metadata_unlock();sync_now();JP(!region_cache_count,"invalid history geometry clears");mutate(&job,3,other,0,0);
 for(unsigned bad=0;bad<4;++bad){warm_ranges(stable,64);metadata_lock();v=atomic_load(&region_epoch);
  region_cache_epoch=bad==0?v-1:bad==1?v+2:bad==2?0:v-34;metadata_unlock();sync_now();JP(!region_cache_count,"odd future missing or overrun cache epoch clears");}
 /* An odd epoch never retains authority, including a second same-epoch sync. */
 warm_ranges(stable,64);win64_region_mutation_begin();region_cache_epoch=atomic_load(&region_epoch);region_cache_sync(region_cache_epoch);JP(!region_cache_count,"same odd epoch clears authority");
 region_cache_sync(region_cache_epoch);JP(!region_cache_count,"odd empty cache has no authority");win64_region_mutation_end();
#endif
 atomic_store_explicit(&job.command,UINT32_MAX,memory_order_release);jcheck(!pthread_join(worker,NULL),"journal worker joins");
 for(unsigned i=0;i<64;++i)win64_region_remove(stable+i*256);full_invariants();jcheck(!regions&&!region_index_count,"history matrix teardown");
}
#endif

/* Real two-thread release/acquire handoff. The timed interval includes writer
 * add/validation/remove, journal replay, reader validation and handshake cost. */
typedef struct {atomic_uint phase,ack;unsigned iterations,reads,pairs,unknown_period;uint64_t stable,mapped,checksum;} bench_job;
static void *bench_mutator(void *opaque){
 bench_job *j=opaque;uint64_t sum=0;
 for(unsigned i=1;i<=j->iterations;++i){
  while(atomic_load_explicit(&j->ack,memory_order_acquire)!=i-1)sched_yield();
  if(j->unknown_period&&i%j->unknown_period==0)win64_region_mutation_begin();
  for(unsigned k=0;k<j->pairs;++k){win64_region_add(j->mapped,128,1);layout_assert(accessible(j->mapped,128,1),"mutator mapped span RW");win64_region_remove(j->mapped);layout_assert(!accessible(j->mapped,1,0),"mutator unmapped span revoked");sum+=3;}
  if(j->unknown_period&&i%j->unknown_period==0)win64_region_mutation_end();
  atomic_store_explicit(&j->phase,i,memory_order_release);
 }
 while(atomic_load_explicit(&j->ack,memory_order_acquire)!=j->iterations)sched_yield();j->checksum=sum;return NULL;
}
static void cross_bench(unsigned mode){
 bench_job j={.iterations=mode==3?1200:5000,.reads=mode==2?1:64,.pairs=mode==3?9:1,.unknown_period=mode==1?8:0,
              .stable=UINT64_C(0x7800000000),.mapped=UINT64_C(0x7900000000)};
 for(unsigned i=0;i<4096;++i)win64_region_add(j.stable+i*256,128,1);
 for(unsigned i=0;i<j.reads;++i)layout_assert(accessible(j.stable+i*256,128,1),"benchmark reader warm");
 pthread_t writer;layout_assert(!pthread_create(&writer,NULL,bench_mutator,&j),"benchmark writer starts");
 uint64_t sum=0,started=nanos();
 for(unsigned round=1;round<=j.iterations;++round){
  while(atomic_load_explicit(&j.phase,memory_order_acquire)!=round)sched_yield();
  for(unsigned i=0;i<j.reads;++i){int answer=accessible(j.stable+i*256+(round&31),16,1);layout_assert(answer,"benchmark reader authority");sum=sum*UINT64_C(6364136223846793005)+(unsigned)answer+i;}
  atomic_store_explicit(&j.ack,round,memory_order_release);
 }
 layout_assert(!pthread_join(writer,NULL),"benchmark writer joins");uint64_t elapsed=nanos()-started;sum+=j.checksum;
 for(unsigned i=0;i<4096;++i)win64_region_remove(j.stable+i*256);full_invariants();layout_assert(!regions&&!region_index_count,"cross-thread benchmark teardown");
 const char *names[]={"disjoint_64_reads","unknown_every_8","disjoint_one_read","history_overrun_18_records"};
 printf("{\"kind\":\"cross_thread_cycle\",\"name\":\"%s\",\"iterations\":%u,\"reads\":%u,\"map_pairs\":%u,\"unknown_period\":%u,\"ns\":%"PRIu64",\"checksum\":%"PRIu64",\"real_mutator_thread\":true,\"game_fps_proven\":false}\n",names[mode],j.iterations,j.reads,j.pairs,j.unknown_period,elapsed,sum);
}

#ifdef REGION_JOURNAL_SHARED
__attribute__((visibility("default"))) int region_journal_entry(int argc,char **argv){
#else
int main(int argc,char **argv){
#endif
 if(argc==2&&!strcmp(argv[1],"--unbalanced-end")){win64_region_mutation_end();return 99;}
 if(argc==2)return layout_fixture_main(argc,argv);
 layout_assert(argc==1,"journal arguments");
#ifndef REGION_LAYOUT_RELEASE_BENCH
 layout_assert(!layout_fixture_main(1,argv),"all original region tests unchanged");history_matrix();
 printf("{\"kind\":\"journal_proof\",\"success\":true,\"semantic_checks\":%"PRIu64",\"candidate_policy_checks\":%"PRIu64",\"public_unknown_flush\":true,\"nested_unknown_flush\":true,\"history_overrun_flush\":true,\"all_split_owner_fragments\":true,\"production_modified\":false}\n",journal_checks,journal_policy_checks);
#endif
 for(unsigned mode=0;mode<4;++mode)cross_bench(mode);
 return 0;
}
