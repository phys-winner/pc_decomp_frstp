/* Actual production region/cache code. The independent byte oracle is a
 * separate executable; this one forces cross-thread mutation interleavings. */
#if defined(_WIN32)
#include "production_region.inc"
#else
#define WIN64_REGION_CACHE_TEST 1
#include "../android/host/win64.c"
#endif
#include <time.h>
#if !defined(_WIN32)
#include <sys/wait.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(fixture_teb)||bytes>8)abort();memcpy(&v,fixture_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(fixture_teb)||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
#endif
static void require_region(int value,const char *why){if(!value){fprintf(stderr,"REGION_TLS_FAIL %s\n",why);abort();}}
static uint64_t nanos(void){
#if defined(_WIN32)
    LARGE_INTEGER now,hz;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&hz);return (uint64_t)((double)now.QuadPart*1e9/hz.QuadPart);
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
#endif
}
typedef struct {atomic_uint command,ack;uint64_t address;size_t bytes;int writing,wanted;} region_job;
static uint64_t checks;
static void *region_worker(void *opaque){region_job *job=opaque;unsigned seen=0;
    for(;;){unsigned command;while((command=atomic_load_explicit(&job->command,memory_order_acquire))==seen)sched_yield();
        if(command==UINT32_MAX)break;
        for(unsigned i=0;i<8;++i)require_region(accessible(job->address,job->bytes,job->writing)==job->wanted,"cross-thread epoch result");
        seen=command;atomic_store_explicit(&job->ack,seen,memory_order_release);
    }return NULL;
}
static void ask(region_job *job,uint64_t address,size_t bytes,int writing,int wanted){
    unsigned next=atomic_load_explicit(&job->command,memory_order_relaxed)+1;
    job->address=address;job->bytes=bytes;job->writing=writing;job->wanted=wanted;
    atomic_store_explicit(&job->command,next,memory_order_release);
    while(atomic_load_explicit(&job->ack,memory_order_acquire)!=next)sched_yield();checks+=8;
}
static atomic_uint race_entered,race_changed;
static uint64_t race_address;
static void fast_probe(void){region_fast_probe=NULL;atomic_store_explicit(&race_entered,1,memory_order_release);
    while(!atomic_load_explicit(&race_changed,memory_order_acquire))sched_yield();}
static void *race_writer(void *ignored){(void)ignored;while(!atomic_load_explicit(&race_entered,memory_order_acquire))sched_yield();
    protection_set(race_address,16,2,0);atomic_store_explicit(&race_changed,1,memory_order_release);return NULL;}
static void *physical_map(size_t bytes){
#if defined(_WIN32)
    return VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);return p==MAP_FAILED?NULL:p;
#endif
}
static int physical_protect(void *p,size_t bytes){
#if defined(_WIN32)
    DWORD old;return VirtualProtect(p,bytes,PAGE_READONLY,&old)!=0;
#else
    return !mprotect(p,bytes,PROT_READ);
#endif
}
static int physical_unmap(void *p,size_t bytes){
#if defined(_WIN32)
    (void)bytes;return VirtualFree(p,0,MEM_RELEASE)!=0;
#else
    return !munmap(p,bytes);
#endif
}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--epoch-overflow")){
        atomic_store(&region_epoch,UINT64_MAX-1);win64_region_mutation_begin();return 99;
    }
    region_cache_self_test();
    uint64_t p=win64_alloc(128,1);require_region(p!=0,"allocation");
    region_job job={0};pthread_t worker;require_region(!pthread_create(&worker,NULL,region_worker,&job),"thread");
    for(unsigned round=0;round<256;++round){
        ask(&job,p,128,1,1);win64_region_add(p+32,16,0);ask(&job,p+32,16,1,0);ask(&job,p+32,16,0,1);
        ask(&job,p+24,32,1,0);win64_region_remove(p+32);ask(&job,p+24,32,1,1);
        protection_set(p+64,16,1,0);ask(&job,p+64,16,0,0);ask(&job,p+56,32,0,0);
        protection_set(p+64,16,0,0x2000);ask(&job,p+64,16,-1,1);ask(&job,p+64,16,-2,0);
        protection_set(p+64,16,4,0x1000);ask(&job,p,128,1,1);
    }
    ask(&job,p,16,1,1);win64_free(p);ask(&job,p,16,0,0);
    /* Same logical address is deliberately rebound RO, then RW, then removed.
     * No freed pointer is dereferenced; the authority is checked independently. */
    win64_region_add(p,128,0);ask(&job,p,128,1,0);ask(&job,p,128,0,1);win64_region_remove(p);
    win64_region_add(p,128,1);ask(&job,p,128,1,1);win64_region_remove(p);ask(&job,p,128,0,0);
    /* Force a writer strictly between the fast path's first and second epoch
     * observations. The second observation must discard the old RW answer. */
    race_address=UINT64_C(0x40000000);win64_region_add(race_address,16,1);
    require_region(accessible(race_address,16,1),"race warm");uint64_t slow=region_slow_calls;
    pthread_t writer;require_region(!pthread_create(&writer,NULL,race_writer,NULL),"race thread");region_fast_probe=fast_probe;
    require_region(!accessible(race_address,16,1),"second epoch must reject warmed RW");
    require_region(region_slow_calls==slow+1,"epoch race re-enters locked oracle");require_region(!pthread_join(writer,NULL),"join writer");win64_region_remove(race_address);
    /* Physical release is inside the same exclusion boundary as metadata.
     * Mutator exits only after its unmap; the worker must deny the old address. */
    size_t mapping_bytes=65536;void *mapping=physical_map(mapping_bytes);require_region(mapping!=NULL,"physical map");
    uint64_t mapped=(uintptr_t)mapping;win64_region_add(mapped,mapping_bytes,1);ask(&job,mapped,16,1,1);
    win64_region_mutation_begin();require_region(physical_protect(mapping,mapping_bytes),"physical protect");
    protection_set(mapped,mapping_bytes,2,0);win64_region_mutation_end();ask(&job,mapped,16,1,0);ask(&job,mapped,16,0,1);
    win64_region_mutation_begin();require_region(physical_unmap(mapping,mapping_bytes),"physical unmap");
    win64_region_remove(mapped);win64_region_mutation_end();ask(&job,mapped,16,0,0);
    atomic_store_explicit(&job.command,UINT32_MAX,memory_order_release);require_region(!pthread_join(worker,NULL),"join worker");
#if !defined(_WIN32)
    pid_t child=fork();require_region(child>=0,"epoch overflow child");
    if(!child){atomic_store(&region_epoch,UINT64_MAX-1);win64_region_mutation_begin();_Exit(99);}
    int status;require_region(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==73,"epoch overflow fail closed");
#endif
    /* Warm, identical queries. Forced-slow mode retains the exact old locked
     * proof; no permission checks or index work are replaced by a fake loop. */
    unsigned char bytes[128];uint64_t base=(uintptr_t)bytes;win64_region_add(base,sizeof(bytes),1);
    require_region(accessible(base,128,1),"benchmark warm");const unsigned iterations=1000000;volatile unsigned sum=0;
    uint64_t start=nanos();for(unsigned i=0;i<iterations;++i)sum+=accessible(base+(i&63),8,1);uint64_t fast_ns=nanos()-start;
    uint64_t hits=region_fast_hits;region_force_slow=1;start=nanos();for(unsigned i=0;i<iterations;++i)sum+=accessible(base+(i&63),8,1);uint64_t slow_ns=nanos()-start;region_force_slow=0;
    require_region(sum==iterations*2&&hits>=iterations,"benchmark answers/fast hits");win64_region_remove(base);
    printf("{\"success\":true,\"cache_entries_per_thread\":%u,\"cross_thread_checks\":%"PRIu64",\"overlap_priority\":true,\"split_commit_decommit\":true,\"same_address_reuse\":true,\"cross_thread_free\":true,\"physical_protect_unmap\":true,\"forced_double_epoch_race\":true,\"benchmark_iterations\":%u,\"fast_ns\":%"PRIu64",\"locked_oracle_ns\":%"PRIu64",\"fast_hits\":%"PRIu64",\"game_boot_proven\":false}\n",
           (unsigned)REGION_CACHE_CAPACITY,checks,iterations,fast_ns,slow_ns,hits);return 0;
}
