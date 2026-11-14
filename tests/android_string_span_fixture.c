/* Exercise the production pointer gate, including physical guard pages. */
#ifndef STRING_SPAN_SOURCE
#define STRING_SPAN_SOURCE "../android/host/win64.c"
#endif
#include STRING_SPAN_SOURCE
#include <sched.h>
#include <assert.h>
#include <sys/wait.h>
#include <time.h>

static const char *byte_scan(uint64_t p) {
    for(size_t n=0;n<32768;++n) {
        if(!accessible(p+n,1,0))invalid("unterminated guest string at 0x%" PRIx64,p);
        if(!*(const char *)(uintptr_t)(p+n))return (const char *)(uintptr_t)p;
    }
    invalid("guest string exceeds 32767 bytes");
}
static unsigned checks;
static void compare_scan(uint64_t p) {
    errno=EILSEQ;const char *candidate=win64_string_a(p);assert(errno==EILSEQ);
    errno=EILSEQ;assert(candidate==byte_scan(p)&&errno==EILSEQ);++checks;
}
static void rejected(uint64_t p) {
    for(unsigned variant=0;variant<2;++variant) {
        pid_t child=fork();assert(child>=0);
        if(!child){if(variant)win64_string_a(p);else byte_scan(p);_Exit(99);}
        int status;assert(waitpid(child,&status,0)==child);
        assert(WIFEXITED(status)&&WEXITSTATUS(status)==73);++checks;
    }
}
static uint64_t now(void) {
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}

#ifdef WIN64_REGION_CACHE_TEST
static atomic_uint race_entered,race_changed;
static unsigned char *race_area;static size_t race_page;
static void bulk_proof_probe(void){region_fast_probe=NULL;atomic_store_explicit(&race_entered,1,memory_order_release);while(!atomic_load_explicit(&race_changed,memory_order_acquire))sched_yield();}
static void first_byte_probe(void){region_fast_probe=bulk_proof_probe;}
static void *span_writer(void *unused){(void)unused;while(!atomic_load_explicit(&race_entered,memory_order_acquire))sched_yield();
    win64_region_mutation_begin();assert(!mprotect(race_area+race_page,race_page,PROT_NONE));protection_set((uintptr_t)race_area+race_page,race_page,1,0);win64_region_mutation_end();
    atomic_store_explicit(&race_changed,1,memory_order_release);return NULL;
}
static void forced_span_mutation(void){
    for(unsigned variant=0;variant<2;++variant){
        race_page=(size_t)sysconf(_SC_PAGESIZE);race_area=mmap(NULL,2*race_page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(race_area!=MAP_FAILED);
        memset(race_area,'a',2*race_page);race_area[race_page+15]=0;win64_region_add((uintptr_t)race_area,2*race_page,1);uint64_t p=(uintptr_t)race_area+race_page-1;assert(accessible(p,1,0));
        pid_t child=fork();assert(child>=0);
        if(!child){atomic_store(&race_entered,0);atomic_store(&race_changed,0);pthread_t writer;assert(!pthread_create(&writer,NULL,span_writer,NULL));region_fast_probe=first_byte_probe;
            if(variant)win64_string_a(p);else byte_scan(p);_Exit(99);
        }
        int status;assert(waitpid(child,&status,0)==child);int exit_code=WIFEXITED(status)?WEXITSTATUS(status):-1;
        fprintf(stderr,"SPAN_RACE variant=%u expected_exit=73 actual_exit=%d signal=%d\n",variant,exit_code,WIFSIGNALED(status)?WTERMSIG(status):0);
        assert(WIFEXITED(status)&&exit_code==73);++checks;win64_region_remove((uintptr_t)race_area);assert(!munmap(race_area,2*race_page));
    }
}
#endif

int main(void) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *p=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(p!=MAP_FAILED);p+=page;assert(!mprotect(p,page,PROT_READ|PROT_WRITE));
    win64_region_add((uintptr_t)p,page,1);
    for(size_t length=0;length<256;++length)for(size_t offset=0;offset<64;++offset) {
        memset(p,0xa5,page);p[offset+length]=0;compare_scan((uintptr_t)p+offset);
    }
    for(size_t length=0;length<128;++length) {
        memset(p,0xa5,page);p[page-1]=0;compare_scan((uintptr_t)p+page-1-length);
    }
    /* A denied overlay after NUL must not invalidate the earlier string. */
    for(size_t length=0;length<128;++length) {
        memset(p,0xa5,page);p[length]=0;protection_set((uintptr_t)p+length+1,1,1,0);
        compare_scan((uintptr_t)p);protection_set((uintptr_t)p+length+1,1,4,0);
    }
    memset(p,0xa5,page);rejected((uintptr_t)p+page-32);rejected(0);rejected(UINT64_MAX);
    win64_region_remove((uintptr_t)p);assert(!munmap(p-page,page*3));
    unsigned char *large=malloc(32769);assert(large);memset(large,'a',32769);
    win64_region_add((uintptr_t)large,32769,1);large[32767]=0;compare_scan((uintptr_t)large);
    large[32767]='a';large[32768]=0;rejected((uintptr_t)large);
    large[4095]=0;
    uint64_t times[2];volatile const char *sink=NULL;
    for(unsigned variant=0;variant<2;++variant) {
        uint64_t start=now();
        for(unsigned i=0;i<2000;++i)sink=variant?win64_string_a((uintptr_t)large):byte_scan((uintptr_t)large);
        times[variant]=now()-start;
    }
    assert(sink==(const char*)large);win64_region_remove((uintptr_t)large);free(large);
#ifdef WIN64_REGION_CACHE_TEST
    forced_span_mutation();
#endif
    printf("{\"success\":true,\"cases\":%u,\"byte_ns\":%llu,\"chunk_ns\":%llu}\n",checks,
        (unsigned long long)times[0],(unsigned long long)times[1]);
    return 0;
}
