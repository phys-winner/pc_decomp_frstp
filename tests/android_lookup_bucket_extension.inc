/* Reuses the exact extracted bootstrap bodies and existing namespace doubles.
 * Token pointer mode never executes synthetic addresses. Each mode is a fresh
 * process so baseline TLS cache entries never outlive a table-token assignment. */
static uint64_t baseline_init_ns,bucket_init_ns,bucket_repeat_init_ns,pointer_cases,parallel_cases;
_Static_assert(sizeof(fn_t)==sizeof(uintptr_t),"Target function-pointer token width");
static int pointer_mode;
static uint64_t random_step(uint64_t *state){*state^=*state<<13;*state^=*state>>7;*state^=*state<<17;return *state;}
static void bucket_initialize(void){
    uint64_t begin=now_ns();baseline_check_dispatch_table();baseline_init_ns=now_ns()-begin;
    begin=now_ns();candidate_check_dispatch_table();bucket_init_ns=now_ns()-begin;
    begin=now_ns();candidate_check_dispatch_table();bucket_repeat_init_ns=now_ns()-begin;
    if(main_bucket_index[0]||main_bucket_index[MAIN_BUCKET_COUNT]!=recomp_dispatch_count)exit(4);
    for(size_t i=0;i<MAIN_BUCKET_COUNT;++i)if(main_bucket_index[i]>main_bucket_index[i+1])exit(4);
}
static int pointer_pair(uint64_t target){
    event_count=0;
    fn_t a=baseline_main_lookup(target),b=candidate_main_lookup(target);++pointer_cases;
    if(a!=b){fprintf(stderr,"BUCKET_POINTER_FAIL target=%" PRIx64 " baseline=%p candidate=%p ready=%d\n",target,(void*)a,(void*)b,main_bucket_ready);return 0;}return 1;
}
static int pointer_proof(int aliases){
    pointer_mode=1;
    for(uint32_t i=0;i<recomp_dispatch_count;++i){uintptr_t token=UINT64_C(0x10000000)+(aliases?i%257u:i)*16u;memcpy(&recomp_dispatch_table[i].fn,&token,sizeof(token));}
    /* Cold lookup retains exact global lower_bound until normal init runs. */
    for(unsigned i=0;i<64;++i)if(!pointer_pair(tables[0][i*997u%counts[0]]))return 1;
    bucket_initialize();
    for(uint32_t i=0;i<recomp_dispatch_count;++i){uint64_t target=tables[0][i];if(!pointer_pair(target)||!pointer_pair(target-1)||!pointer_pair(target+1))return 1;}
    for(size_t bucket=0;bucket<MAIN_BUCKET_COUNT;++bucket){uint64_t target=MAIN_BUCKET_BASE+(bucket<<MAIN_BUCKET_SHIFT);if(!pointer_pair(target)||!pointer_pair(target+255))return 1;}
    uint64_t state=UINT64_C(0x140539000);for(unsigned i=0;i<65536;++i)if(!pointer_pair(MAIN_BUCKET_BASE+random_step(&state)%MAIN_BUCKET_BYTES))return 1;
    uint64_t edges[]={0,UINT64_MAX,MAIN_BUCKET_BASE-1,MAIN_BUCKET_BASE,MAIN_BUCKET_BASE+MAIN_BUCKET_BYTES-1,MAIN_BUCKET_BASE+MAIN_BUCKET_BYTES};
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);++i)if(!pointer_pair(edges[i]))return 1;
    printf("{\"success\":true,\"kind\":\"%s\",\"pointer_cases\":%" PRIu64 ",\"unique_keys\":%u,\"bucket_count\":%zu,\"index_bytes\":%zu,\"init_ns\":%" PRIu64 ",\"repeat_init_ns\":%" PRIu64 ",\"synthetic_pointers_executed\":false}\n",aliases?"shared-function-aliases":"unique-function-identities",pointer_cases,recomp_dispatch_count,(size_t)MAIN_BUCKET_COUNT,sizeof(main_bucket_index),bucket_init_ns,bucket_repeat_init_ns);return 0;
}
static void bucket_fp_get(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void bucket_fp_set(const uint64_t in[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(in[0]),"r"(in[1]):"memory");
#else
    _mm_setcsr((unsigned)in[0]);
#endif
}
#if !defined(_WIN32)
static void *bucket_worker(void *context){
    uintptr_t seed=(uintptr_t)context;uint64_t state=seed+1,original[2],wanted[2],after[2];bucket_fp_get(original);
    wanted[0]=(original[0]&~(UINT64_C(3)<<22))|(seed<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;bucket_fp_set(wanted);
    stack_lo=(uintptr_t)stack_words;stack_hi=stack_lo+sizeof(stack_words);
    for(unsigned i=0;i<2048;++i){uint64_t target=tables[0][random_step(&state)%counts[0]];
        if(i&1)++target;outcome a=invoke(0,target,i,0,0,0),b=invoke(1,target,i,0,0,0);
        if(memcmp(&a.cpu,&b.cpu,sizeof(CPU))||a.code!=b.code||a.body!=b.body||a.depth!=b.depth||a.calls!=b.calls||a.error!=2718||b.error!=2718)return(void*)1;
        unsigned aa[128],bb[128],na=0,nb=0;for(unsigned j=0;j<a.count;++j)if(a.trace[j]!=13&&a.trace[j]!=14)aa[na++]=a.trace[j];for(unsigned j=0;j<b.count;++j)if(b.trace[j]!=13&&b.trace[j]!=14)bb[nb++]=b.trace[j];
        if(na!=nb||memcmp(aa,bb,na*sizeof(*aa)))return(void*)1;
        bucket_fp_get(after);if(memcmp(after,wanted,sizeof(after)))return(void*)1;
    }bucket_fp_set(original);return NULL;
}
#endif
static void bucket_parallel(void){
#if !defined(_WIN32)
    pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,bucket_worker,(void*)i))exit(4);
    for(unsigned i=0;i<4;++i){void*result;if(pthread_join(threads[i],&result)||result)exit(4);}parallel_cases=8192;
#endif
}
static void bucket_fp_cases(void){
    uint64_t original[2],wanted[2],after[2];bucket_fp_get(original);
    for(unsigned i=0;i<32;++i){
#if defined(__aarch64__)
        wanted[0]=(original[0]&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=0x9fd5|((i&3u)<<13)|((i&4u)?64u:0u);wanted[1]=0;
#endif
        bucket_fp_set(wanted);compare(tables[0][i*997u%counts[0]]+(i&1u),i,0,0,0);bucket_fp_get(after);if(memcmp(after,wanted,sizeof(after)))++failures;
    }bucket_fp_set(original);
}
static void bucket_report(void){
    printf("{\"success\":%s,\"cases\":%" PRIu64 ",\"full_CPU_cases\":%" PRIu64 ",\"trace_checks\":%" PRIu64 ",\"parallel_cases\":%" PRIu64 ",\"failures\":%" PRIu64 ",\"all_main_entries\":%u,\"OpenAL_public_entries\":%u,\"OpenAL_typed_entries\":%u,\"baseline_init_ns\":%" PRIu64 ",\"candidate_init_ns\":%" PRIu64 ",\"repeat_init_ns\":%" PRIu64 ",\"index_bytes\":%zu,\"original_bytes_executed\":false,\"real_provider_API_bodies\":false,\"startup_or_FPS_claim\":false}\n",failures?"false":"true",case_count,case_count,trace_checks,parallel_cases,failures,counts[0],counts[5],counts[6],baseline_init_ns,bucket_init_ns,bucket_repeat_init_ns,sizeof(main_bucket_index));
}
static int bucket_negative(const char *name){
    if(!setjmp(fault)){
        if(!strcmp(name,"--bad-null"))recomp_dispatch_table[1].fn=NULL;
        else if(!strcmp(name,"--bad-order"))recomp_dispatch_table[1].va=recomp_dispatch_table[0].va;
        else if(!strcmp(name,"--bad-low"))recomp_dispatch_table[0].va=MAIN_BUCKET_BASE-1;
        else if(!strcmp(name,"--bad-high"))recomp_dispatch_table[recomp_dispatch_count-1].va=MAIN_BUCKET_BASE+MAIN_BUCKET_BYTES;
        else return 2;candidate_check_dispatch_table();return 99;
    }fprintf(stderr,"BUCKET_INIT_REJECT code=%d\n",fault_code);return fault_code;
}
static unsigned cache_slot(uint64_t target){return(unsigned)((target>>3)^(target>>13))&255;}
static int benchmark_eligible(uint64_t target){return eligible(target)&&target!=UINT64_C(0x140e02420)&&!member(7,target);}
static void bucket_bench(unsigned iterations){
    uint64_t input[5][1024],used[256]={0};unsigned lengths[5]={1,64,512,1024,64};
    input[0][0]=UINT64_C(0x140d871c0);unsigned n=0;
    for(uint32_t i=0;i<counts[0]&&n<64;++i){uint64_t target=tables[0][i];unsigned slot=cache_slot(target);if(benchmark_eligible(target)&&!used[slot]){input[1][n++]=target;used[slot]=1;}}if(n!=64)exit(4);
    uint64_t state=UINT64_C(0x256b0c37);for(unsigned kind=2;kind<4;++kind)for(unsigned i=0;i<lengths[kind];++i){uint64_t target;do{target=tables[0][random_step(&state)%counts[0]];}while(!benchmark_eligible(target));input[kind][i]=target;}
    n=0;for(uint32_t i=0;i<counts[0]&&n<64;++i){uint64_t target=tables[0][i];if(benchmark_eligible(target)&&cache_slot(target)==37)input[4][n++]=target;}if(n!=64)exit(4);
    const char*names[]={"hot1","hot64-distinct-cache-slots","hot512","spread1024","forced-collision64"};
    CPU c={0};baseline_budget=candidate_budget=0;baseline_trace=candidate_trace=0;
    printf("{\"success\":true,\"kind\":\"bucket_lookup_model_microbenchmark\",\"iterations_per_round\":%u,\"rounds\":7,\"index_bytes\":%zu,\"init_ns\":%" PRIu64 ",\"repeat_init_ns\":%" PRIu64 ",\"scenarios\":[",iterations,sizeof(main_bucket_index),bucket_init_ns,bucket_repeat_init_ns);
    for(unsigned scenario=0;scenario<5;++scenario){uint64_t times[2][7],checksums[2]={0};
        for(unsigned round=0;round<7;++round)for(unsigned turn=0;turn<2;++turn){unsigned variant=(round+turn)&1;c.rax=0;uint64_t expected=0;uint64_t begin=now_ns();
            for(unsigned i=0;i<iterations;++i){uint64_t target=input[scenario][i%lengths[scenario]];c.rsp=(uintptr_t)&stack_words[32];if(variant)candidate_dispatch(&c,target);else baseline_dispatch(&c,target);expected+=target+1;}
            times[variant][round]=now_ns()-begin;if(c.rax!=expected)exit(4);checksums[variant]^=c.rax;
        }
        if(checksums[0]!=checksums[1])exit(4);printf("%s{\"name\":\"%s\",\"baseline_ns\":[",scenario?",":"",names[scenario]);for(unsigned i=0;i<7;++i)printf("%s%" PRIu64,i?",":"",times[0][i]);printf("],\"candidate_ns\":[");for(unsigned i=0;i<7;++i)printf("%s%" PRIu64,i?",":"",times[1][i]);printf("],\"checksum\":%" PRIu64 "}",checksums[0]);
    }printf("],\"checksums_verified\":true,\"trace_compiled_out\":true,\"native_provider_bodies\":false,\"startup_FPS_claim\":false}\n");
}
