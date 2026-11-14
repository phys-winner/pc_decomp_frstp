/* Appended after freshly extracted production functions by review_region_cache.py.
 * The expected state lives in an independent newest-first array of byte cells;
 * it does not traverse production regions or reuse their span/cache algorithm. */
typedef struct {uint64_t base,owner;size_t size;int active;uint32_t prot[128],state[128];} model_record;
static model_record model[128];static size_t model_count;
static uint32_t seed=UINT32_C(0x12345678);
static uint64_t cases,extra_cases,index_checks;
static unsigned current_layout,current_phase;
static uint32_t random32(void){seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);return seed;}
static void require(int value,const char *why){if(!value){fprintf(stderr,"ORACLE_FAILURE %s\n",why);exit(1);}}
static const model_record *model_at(uint64_t p,size_t *offset){
 for(size_t j=model_count;j>0;--j){const model_record *m=&model[j-1];
  if(m->active&&p>=m->base&&p-m->base<m->size){*offset=(size_t)(p-m->base);return m;}}
 return NULL;
}
static int expected_byte(uint64_t p,int writing){size_t offset=0;const model_record *m=model_at(p,&offset);if(!m)return 0;
 if(writing!=-1&&m->state[offset]!=UINT32_C(0x1000))return 0;
 if(writing>=0){uint32_t protection=m->prot[offset];if(protection==1)return 0;
  if(writing&&!(protection&UINT32_C(0xcc)))return 0;}return 1;
}
static int expected(uint64_t p,size_t bytes,int writing){
 if(!bytes)return 1;if(!p||p>UINT64_MAX-bytes)return 0;
 for(size_t i=0;i<bytes;++i)if(!expected_byte(p+i,writing))return 0;return 1;
}
/* These checks do not decide expected access. They separately verify that no
 * index entry has a freed/unknown pointer, duplicates a live record, has stale
 * geometry ordering, or understates the maximum end of any earlier record. */
/* Independent structure invariants; never used to compute expected access. */
static region *live_regions[8192];
static unsigned char tree_seen[8192],owner_seen[8192];
static size_t live_region_count;
static size_t test_bucket(uint64_t key,size_t capacity){
 key^=key>>30;key*=UINT64_C(0xbf58476d1ce4e5b9);key^=key>>27;
 key*=UINT64_C(0x94d049bb133111eb);key^=key>>31;return (size_t)key&(capacity-1);
}
static size_t live_position(region *r){
 for(size_t i=0;i<live_region_count;++i)if(live_regions[i]==r)return i;
 require(0,"tree/hash node belongs to live list before dereference");return 0;
}
static int independent_less(region *a,region *b){return a->base<b->base||(a->base==b->base&&(uintptr_t)a<(uintptr_t)b);}
typedef struct {unsigned height;uint64_t end;size_t count;} tree_result;
static tree_result tree_visit(region *r,region *low,region *high){
 if(!r)return (tree_result){0,0,0};
 size_t i=live_position(r);require(!tree_seen[i],"AVL node appears only once/no cycles");tree_seen[i]=1;
 require(!low||independent_less(low,r),"strict AVL lower bound by base and pointer");
 require(!high||independent_less(r,high),"strict AVL upper bound by base and pointer");
 tree_result left=tree_visit(r->left,low,r),right=tree_visit(r->right,r,high);
 unsigned h=1+(left.height>right.height?left.height:right.height);
 require(r->height==h,"independent recursive AVL height");
 require(left.height<=right.height+1&&right.height<=left.height+1,"AVL balance at every node");
 uint64_t end=r->base+r->size;if(left.end>end)end=left.end;if(right.end>end)end=right.end;
 require(r->max_end==end,"exact independently recomputed subtree maximum end");
 return (tree_result){h,end,1+left.count+right.count};
}
static void index_check(void){
 live_region_count=0;region *previous=NULL;
 for(region *r=regions;r;r=r->next){
  require(live_region_count<8192,"bounded acyclic region list");
  for(size_t j=0;j<live_region_count;++j)require(live_regions[j]!=r,"region list contains no repeated pointer");
  require(r->previous==previous,"region previous is exact prior list node");
  require(r->size&&r->base<=UINT64_MAX-r->size,"region geometry before tree dereference");
  live_regions[live_region_count++]=r;previous=r;
 }
 memset(tree_seen,0,sizeof(tree_seen));memset(owner_seen,0,sizeof(owner_seen));
 tree_result t=tree_visit(region_index,NULL,NULL);
 require(t.count==live_region_count&&t.count==region_index_count,"AVL/list/count bijection");
 require(!region_owner_capacity||(region_owner_capacity>=64&&!(region_owner_capacity&(region_owner_capacity-1))),"owner capacity power of two");
 require(!live_region_count||region_owner_capacity,"nonempty list has owner buckets");
 size_t hashes=0;
 for(size_t bucket=0;bucket<region_owner_capacity;++bucket){
  for(region *r=region_owners[bucket];r;r=r->owner_next){
   size_t i=live_position(r);require(!owner_seen[i],"owner node appears once/no cycles");owner_seen[i]=1;
   require(test_bucket(r->owner,region_owner_capacity)==bucket,"owner key hashes to exact bucket");++hashes;
  }
 }
 require(hashes==live_region_count,"owner/list count bijection");
 for(size_t i=0;i<live_region_count;++i)require(tree_seen[i]&&owner_seen[i],"each live region is in AVL and owner hash exactly once");
 ++index_checks;
}

static void add(uint64_t base,size_t size,uint32_t protection,uint32_t state){
 require(model_count<128&&size<=128&&size>0&&base<=UINT64_MAX-size,"model add geometry");
 win64_region_add(base,size,protection!=2);regions->protection=protection;regions->state=state;
 model_record *m=&model[model_count++];m->base=m->owner=base;m->size=size;m->active=1;
 for(size_t i=0;i<size;++i){m->prot[i]=protection;m->state[i]=state;}index_check();
}
static void remove_owner(uint64_t owner){
 win64_region_remove(owner);int found=0;
 for(size_t j=0;j<model_count;++j)if(model[j].active&&model[j].owner==owner){model[j].active=0;found=1;}
 require(found,"independent owner existed");index_check();
}
static void protect(uint64_t begin,size_t bytes,uint32_t protection,uint32_t state){
 require(begin<=UINT64_MAX-bytes,"model protection geometry");protection_set(begin,bytes,protection,state);
 for(size_t j=0;j<model_count;++j){model_record *m=&model[j];if(!m->active)continue;
  for(size_t i=0;i<m->size;++i){uint64_t p=m->base+i;if(p>=begin&&p-begin<bytes){m->prot[i]=protection;if(state)m->state[i]=state;}}}index_check();
}
static void check(uint64_t p,size_t bytes,int writing,int extra){
 int want=expected(p,bytes,writing),cached=accessible(p,bytes,writing),linear=0,indexed=0;
 if(!bytes)linear=indexed=1;else if(p&&p<=UINT64_MAX-bytes){linear=accessible_linear(p,bytes,writing,0);indexed=accessible_indexed(p,bytes,writing,0);}
 if(want!=cached||want!=linear||want!=indexed){fprintf(stderr,"MISMATCH layout=%u phase=%u p=%"PRIx64" n=%zu writing=%d expected=%d cached=%d linear=%d indexed=%d\n",current_layout,current_phase,p,bytes,writing,want,cached,linear,indexed);
  for(region *r=regions;r;r=r->next)fprintf(stderr,"ACTUAL base=%"PRIx64" owner=%"PRIx64" bytes=%zu prot=%x state=%x\n",r->base,r->owner,r->size,r->protection,r->state);
  for(size_t j=model_count;j>0;--j){model_record *m=&model[j-1];if(m->active)fprintf(stderr,"MODEL base=%"PRIx64" bytes=%zu prot_first=%x state_first=%x\n",m->base,m->size,m->prot[0],m->state[0]);}exit(1);}
 if(extra)++extra_cases;else ++cases;
}
static void cache_check(void){
 index_check();
 require(region_cache_count<=REGION_CACHE_CAPACITY,"cache capacity");
 for(size_t j=0;j<region_cache_count;++j){region_snapshot s=region_cache[j];
  require(s.size>0&&s.size<=128&&s.base<=UINT64_MAX-s.size,"snapshot geometry");
  for(size_t i=0;i<s.size;++i){size_t offset=0;const model_record *m=model_at(s.base+i,&offset);
   require(m&&m->prot[offset]==s.protection&&m->state[offset]==s.state,"every cached byte has exact independent first-priority attributes");}}
}
static void clear_all(void){while(regions)remove_owner(regions->owner);require(!region_cache_count&&!region_index_count,"all removed cache/index empty");model_count=0;}
static void sweep(uint64_t base,size_t size,int extra){
 for(size_t offset=0;offset<size;++offset)for(size_t n=0;n<=size-offset;++n)for(int mode=-2;mode<=1;++mode)check(base+offset,n,mode,extra);
 cache_check();
}
int main(void){
 for(unsigned layout=0;layout<100;++layout){current_layout=layout;
  add(0x1000,96,4,0x1000);
  for(unsigned j=0;j<8;++j){uint64_t b=0x1000+random32()%80;size_t n=1+random32()%16;
   static const uint32_t protections[]={1,2,4,0x20,0x40};uint32_t prot=protections[random32()%5],state=(random32()%5)?0x1000:0x2000;add(b,n,prot,state);}
  for(unsigned phase=0;phase<3;++phase){current_phase=phase;sweep(0x1000,96,0);protect(0x1010+phase*8,24,phase?4:1,phase?0x1000:0x2000);}
  clear_all();
 }
 require(cases==UINT64_C(5702400),"legacy independent query matrix count");
 /* Queries cross several clipped boundaries, then invalidate/remove/readd and
  * split the same owner repeatedly while the independent byte model survives. */
 add(0x2000,96,4,0x1000);add(0x2008,8,2,0x1000);add(0x2020,8,1,0x1000);add(0x2040,8,4,0x2000);sweep(0x2000,96,1);
 remove_owner(0x2020);sweep(0x2000,96,1);add(0x2020,8,4,0x1000);sweep(0x2000,96,1);
 protect(0x2010,48,2,0);sweep(0x2000,96,1);protect(0x2018,16,0,0x2000);sweep(0x2000,96,1);
 protect(0x2018,16,4,0x1000);sweep(0x2000,96,1);remove_owner(0x2000);sweep(0x2000,96,1);clear_all();
 /* An owner can have multiple split records, and repeated registrations at one
  * address must all disappear together without leaving a stale permission. */
 add(0x3000,96,4,0x1000);add(0x3010,32,2,0x1000);add(0x3010,16,1,0x1000);protect(0x3018,8,0x40,0x1000);sweep(0x3000,96,1);
 remove_owner(0x3010);sweep(0x3000,96,1);add(0x3010,16,4,0x1000);sweep(0x3000,96,1);clear_all();
 /* Adjacent regions, exact ends, gaps, NULL and an exclusive UINT64_MAX end. */
 add(0,64,2,0x1000);add(64,64,4,0x1000);sweep(0,128,1);clear_all();
 add(UINT64_MAX-96,96,4,0x1000);add(UINT64_MAX-80,8,1,0x1000);add(UINT64_MAX-40,8,2,0x2000);sweep(UINT64_MAX-96,96,1);
 const uint64_t addresses[]={0,1,UINT64_MAX-1,UINT64_MAX};const size_t lengths[]={0,1,2,96,SIZE_MAX};
 for(size_t i=0;i<4;++i)for(size_t j=0;j<5;++j)for(int w=-2;w<=1;++w)check(addresses[i],lengths[j],w,1);
 remove_owner(UINT64_MAX-80);protect(UINT64_MAX-32,16,2,0x1000);sweep(UINT64_MAX-96,96,1);clear_all();
 /* Populate beyond the extracted production capacity. Each independent record
  * is disjoint, so every access contributes one distinct complete snapshot. */
 for(size_t j=0;j<REGION_CACHE_CAPACITY+2;++j)add(0x10000+j*32,16,j&1?2:4,0x1000);
 for(size_t j=0;j<REGION_CACHE_CAPACITY+2;++j)check(0x10000+j*32,16,0,1);
 require(region_cache_count==REGION_CACHE_CAPACITY,"full actual capacity");
 for(size_t j=0;j<REGION_CACHE_CAPACITY;++j)require(region_cache[j].base==0x10000+(REGION_CACHE_CAPACITY+1-j)*32,"exact newest-first eviction order");
 cache_check();check(0x10000,16,0,1);
 require(region_cache_count==REGION_CACHE_CAPACITY&&region_cache[0].base==0x10000,"reload first evicted entry");
 for(size_t j=1;j<REGION_CACHE_CAPACITY;++j)require(region_cache[j].base==0x10000+(REGION_CACHE_CAPACITY+2-j)*32,"reload preserves all retained priority");
 cache_check();remove_owner(0x10000+(REGION_CACHE_CAPACITY+1)*32);cache_check();
 check(0x10000+(REGION_CACHE_CAPACITY+1)*32,16,0,1);clear_all();
 /* The immediate address predecessor has ended, but a still-earlier interval
  * covers p. An ordinary predecessor-only index would incorrectly report a gap
  * or use the shorter record's attributes; the prefix maximum must force the
  * authoritative priority walk. Repeat with the covering region older/newer. */
 add(0x4000,96,2,0x1000);add(0x4010,8,4,0x1000);
 require(!expected(0x4030,1,1)&&expected(0x4030,1,0),"long predecessor explicit RO expectation");
 sweep(0x4000,96,1);remove_owner(0x4010);add(0x4010,8,1,0x2000);sweep(0x4000,96,1);clear_all();
 add(0x4010,8,1,0x1000);add(0x4000,96,4,0x1000);sweep(0x4000,96,1);clear_all();
 /* Equal bases do not encode list priority. Rebuild may reorder equal keys,
  * so every ambiguous byte must retain newest-first semantics regardless. */
 add(0x5000,96,4,0x1000);add(0x5000,8,2,0x1000);add(0x5000,32,4,0x2000);
 sweep(0x5000,96,1);protect(0x5010,24,1,0x1000);sweep(0x5000,96,1);
 remove_owner(0x5000);sweep(0x5000,96,1);add(0x5000,96,2,0x1000);sweep(0x5000,96,1);clear_all();
 /* A unique first byte does not prove a whole query. The next sorted base
  * introduces RO/reserved overlays; crossing queries must split/revalidate. */
 add(0x6000,96,4,0x1000);add(0x6020,8,2,0x1000);add(0x6040,8,4,0x2000);
 require(!expected(0x6010,32,1)&&!expected(0x6030,32,0),"crossing overlay denies write/read explicitly");
 sweep(0x6000,96,1);remove_owner(0x6020);sweep(0x6000,96,1);protect(0x6010,64,2,0x1000);sweep(0x6000,96,1);
 remove_owner(0x6000);sweep(0x6000,96,1);add(0x6000,96,4,0x1000);sweep(0x6000,96,1);clear_all();
 /* Insert/remove at beginning/middle/end, with a long prefix covering several
  * shorter entries. Removing multiple separated fragments must recompute the
  * whole affected suffix, not stop when an intermediate old prefix matches. */
 add(0x7000,96,2,0x1000);add(0x7060,16,4,0x1000);add(0x7020,8,4,0x1000);add(0x7040,8,1,0x1000);
 add(0x6ff0,16,4,0x1000);add(0x7030,8,4,0x2000);sweep(0x6ff0,128,1);
 protect(0x7010,64,4,0x1000);sweep(0x6ff0,128,1);remove_owner(0x7000);sweep(0x6ff0,128,1);
 remove_owner(0x6ff0);remove_owner(0x7060);add(0x7000,96,4,0x1000);sweep(0x6ff0,128,1);clear_all();
 printf("{\"success\":true,\"legacy_queries\":%"PRIu64",\"extra_queries\":%"PRIu64",\"layouts\":100,\"phases\":3,\"independent_array_byte_model\":true,\"cache_attribute_bytes_checked\":true,\"cache_capacity\":%u,\"eviction_records\":%u,\"indexed_and_linear_compared\":true,\"index_invariants_checked\":true,\"index_invariant_passes\":%"PRIu64",\"index_cases\":[\"ended_predecessor_earlier_cover\",\"duplicate_bases\",\"next_overlay_crossing\",\"protect_rebuild\",\"owner_fragments\",\"address_reuse\",\"prefix_suffix_mutations\"],\"writing_modes\":[-2,-1,0,1],\"single_thread_host\":true}\n",cases,extra_cases,(unsigned)REGION_CACHE_CAPACITY,(unsigned)REGION_CACHE_CAPACITY+2,index_checks);free(region_owners);return 0;
}
