/* Included before the extracted entries for capture state, then after load(). */
#if defined(DIRECT_EARLY)
static LOCAL fn_t captured_function;
static LOCAL int direct_pointer_mode,direct_policy,active_variant;
static LOCAL unsigned recursive_limit,recursive_remaining;
static void direct_route(CPU *c,uint64_t target);
static void capture_call(fn_t function,CPU *c){captured_function=function;if(!direct_pointer_mode)function(c);}
#else
static int direct_eligible(uint64_t t){return eligible(t)&&!member(7,t);}
static fn_t linked_for(uint64_t t){
    if(!direct_eligible(t))return NULL;
    uint32_t lo=0,hi=recomp_dispatch_count;while(lo<hi){uint32_t mid=lo+(hi-lo)/2;if(recomp_dispatch_table[mid].va<t)lo=mid+1;else hi=mid;}
    return lo<recomp_dispatch_count&&recomp_dispatch_table[lo].va==t?recomp_dispatch_table[lo].fn:NULL;
}
static void direct_route(CPU *c,uint64_t target){
    fn_t linked=direct_policy==1?NULL:linked_for(target);
    if(direct_policy==2&&(!eligible(target)||member(7,target)))linked=canonical;
    candidate_fp_dispatch_main(c,target,linked);
}
static void body(CPU *c,unsigned kind){
    record(20+kind);selected_body=kind;c->rax+=c->rip+kind;
    if(kind==1&&recursive_remaining){
        unsigned remaining=recursive_remaining--;uint64_t return_rip=UINT64_C(0x140001111)+remaining;
        push64(c,return_rip);
        if(!active_variant)baseline_dispatch(c,tables[0][(remaining*997u)%counts[0]]);
        else if(remaining&1)direct_route(c,tables[0][(remaining*997u)%counts[0]]);
        else candidate_dispatch(c,tables[0][(remaining*997u)%counts[0]]);
        c->rip=return_rip;
    }
    c->rsp+=8;
}
static void fp_get(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void fp_set(const uint64_t in[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(in[0]),"r"(in[1]):"memory");
#else
    _mm_setcsr((unsigned)in[0]);
#endif
}
static uint64_t step(uint64_t *state){*state^=*state<<13;*state^=*state>>7;*state^=*state<<17;return *state;}
static void initialize(void){baseline_check_dispatch_table();candidate_check_dispatch_table();}
static uint64_t pointer_cases,parallel_cases;
static unsigned worker_failures[4];
static int same_outcome(const outcome *a,const outcome *b){return !memcmp(&a->cpu,&b->cpu,sizeof(CPU))&&a->code==b->code&&a->body==b->body&&a->depth==b->depth&&a->calls==b->calls&&a->error==b->error&&a->called==b->called&&a->count==b->count&&!memcmp(a->trace,b->trace,a->count*sizeof(*a->trace));}
static int pointers(int aliases){
    direct_pointer_mode=1;
    for(uint32_t i=0;i<counts[0];++i){uintptr_t token=UINT64_C(0x10000000)+(aliases?i%257u:i)*16u;memcpy(&recomp_dispatch_table[i].fn,&token,sizeof(token));}
    for(unsigned cold=0;cold<2;++cold){
        if(cold)initialize();unsigned count=cold?counts[0]:64;
        for(unsigned i=0;i<count;++i){uint64_t target=tables[0][cold?i:i*997u%counts[0]];
            for(int neighbor=-1;neighbor<=1;++neighbor){outcome a=invoke(0,target+neighbor,i,0,0,0),b=invoke(1,target+neighbor,i,0,0,0);++pointer_cases;if(!same_outcome(&a,&b))return 1;}
        }
    }
    printf("{\"success\":true,\"pointer_cases\":%"PRIu64",\"aliases\":%s,\"synthetic_pointers_executed\":false}\n",pointer_cases,aliases?"true":"false");return 0;
}
static void *worker(void *opaque){
    uintptr_t seed=(uintptr_t)opaque;uint64_t original[2],wanted[2],after[2],state=seed+1;fp_get(original);
#if defined(__aarch64__)
    wanted[0]=(original[0]&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|(seed<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
    wanted[0]=0x9fd5|((seed&3u)<<13);wanted[1]=0;
#endif
    fp_set(wanted);stack_lo=(uintptr_t)stack_words;stack_hi=stack_lo+sizeof(stack_words);
    for(unsigned i=0;i<2048;++i){uint64_t target=tables[0][step(&state)%counts[0]]+(i&1u);direct_policy=(int)(i%3);
        outcome a=invoke(0,target,i,0,0,0),b=invoke(1,target,i,0,0,0);if(!same_outcome(&a,&b)){
            fprintf(stderr,"DIRECT_PARALLEL_FAIL seed=%zu i=%u target=%"PRIx64" CPU=%d calls=%"PRIu64"/%"PRIu64" depth=%u/%u fn=%p/%p trace=%u/%u error=%d/%d\n",(size_t)seed,i,target,memcmp(&a.cpu,&b.cpu,sizeof(CPU)),a.calls,b.calls,a.depth,b.depth,(void*)a.called,(void*)b.called,a.count,b.count,a.error,b.error);worker_failures[seed]=1;return NULL;}
        fp_get(after);if(memcmp(after,wanted,sizeof(after))){fprintf(stderr,"DIRECT_PARALLEL_FP seed=%zu i=%u wanted=%"PRIx64"/%"PRIx64" actual=%"PRIx64"/%"PRIx64"\n",(size_t)seed,i,wanted[0],wanted[1],after[0],after[1]);worker_failures[seed]=1;return NULL;}
    }fp_set(original);return NULL;
}
static void parallel(void){pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))exit(4);
    for(unsigned i=0;i<4;++i)if(pthread_join(threads[i],NULL)||worker_failures[i])exit(4);parallel_cases=8192;}
static void fp_cases(void){
    uint64_t original[2],wanted[2],after[2];fp_get(original);
    for(unsigned i=0;i<32;++i){
#if defined(__aarch64__)
        wanted[0]=(original[0]&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=0x9fd5|((i&3u)<<13)|((i&4u)?64u:0u);wanted[1]=0;
#endif
        fp_set(wanted);direct_policy=(int)(i%3);compare(tables[0][i*997u%counts[0]],i,0,0,0);fp_get(after);if(memcmp(after,wanted,sizeof(after)))++failures;
    }fp_set(original);direct_policy=0;
}
static void benchmark(unsigned iterations){
    static const uint64_t renderer[]={UINT64_C(0x140e33250),UINT64_C(0x140e33410),UINT64_C(0x141023fb0),UINT64_C(0x141ae320a),UINT64_C(0x141ae3216),UINT64_C(0x140d9b2a0),UINT64_C(0x141021fd0),UINT64_C(0x141024013)};
    static const uint64_t main_entries[]={UINT64_C(0x140eb45f0),UINT64_C(0x140f8f489),UINT64_C(0x140eb4a3d),UINT64_C(0x140e52cd0),UINT64_C(0x140e26c40),UINT64_C(0x141c11c70),UINT64_C(0x140f4cba0),UINT64_C(0x140e27cf0)};
    const char *names[]={"constant-hot1","renderer-fixed8","main-fixed8","mixed-main1024","legacy-variable-main8","cold-spread8192"};
    unsigned lengths[]={1,8,8,1024,8,8192};uint64_t (*input)[8192]=calloc(6,sizeof(*input));fn_t (*linked)[8192]=calloc(6,sizeof(*linked));
    volatile unsigned char *trash=calloc(1,33554432);if(!input||!linked||!trash)exit(4);
    input[0][0]=renderer[0];memcpy(input[1],renderer,sizeof(renderer));memcpy(input[2],main_entries,sizeof(main_entries));memcpy(input[4],renderer,sizeof(renderer));
    uint64_t state=UINT64_C(0xd1ec741);for(unsigned kind=0;kind<6;++kind)for(unsigned i=0;i<lengths[kind];++i){
        if(kind==3||kind==5){do{input[kind][i]=tables[0][step(&state)%counts[0]];}while(!direct_eligible(input[kind][i]));}
        linked[kind][i]=linked_for(input[kind][i]);if(linked[kind][i]!=canonical)exit(4);
    }
    CPU c={0};baseline_budget=candidate_budget=0;baseline_trace=candidate_trace=0;
    printf("{\"success\":true,\"rounds\":7,\"precomputed_bindings_outside_timing\":true,\"synthetic_weights\":true,\"scenarios\":[");
    for(unsigned kind=0;kind<6;++kind){unsigned calls=kind==5?lengths[kind]:iterations;uint64_t times[2][7],checksums[2]={0},expected=0;
        for(unsigned i=0;i<calls;++i)expected+=input[kind][i%lengths[kind]]+1;
        for(unsigned trial=0;trial<7;++trial)for(unsigned order=0;order<2;++order){unsigned variant=(trial+order)&1;c.rax=0;
            if(kind==5)for(unsigned i=0;i<33554432;i+=64)trash[i]=(unsigned char)(i+trial+variant);
            uint64_t start=now_ns();for(unsigned i=0;i<calls;++i){unsigned at=i%lengths[kind];c.rsp=(uintptr_t)&stack_words[32];
                if(!variant)baseline_dispatch(&c,input[kind][at]);else if(kind==4)candidate_dispatch(&c,input[kind][at]);else candidate_fp_dispatch_main(&c,input[kind][at],linked[kind][at]);}
            times[variant][trial]=now_ns()-start;if(c.rax!=expected)exit(4);checksums[variant]^=c.rax;
        }
        if(checksums[0]!=checksums[1])exit(4);
        printf("%s{\"name\":\"%s\",\"calls_per_round\":%u,\"baseline_ns\":[",kind?",":"",names[kind],calls);
        for(unsigned i=0;i<7;++i)printf("%s%"PRIu64,i?",":"",times[0][i]);printf("],\"candidate_ns\":[");for(unsigned i=0;i<7;++i)printf("%s%"PRIu64,i?",":"",times[1][i]);printf("],\"checksum\":%"PRIu64"}",checksums[0]);
    }
    printf("],\"checksums_verified\":true,\"game_gain_proven\":false}\n");free((void*)trash);free(linked);free(input);
}
int main(int argc,char **argv){
    if(argc<2||!load(argv[1]))return 2;
    if(argc==3&&!strcmp(argv[2],"--pointers"))return pointers(0);
    if(argc==3&&!strcmp(argv[2],"--aliases"))return pointers(1);
    for(unsigned i=0;i<64;++i)compare(tables[0][i*997u%counts[0]],i,0,0,0);
    initialize();
#if defined(FP_DISPATCH_BENCHMARK)
    unsigned iterations=argc==3?(unsigned)strtoul(argv[2],NULL,10):200000;if(!iterations||iterations>10000000)return 2;benchmark(iterations);return 0;
#else
    for(unsigned i=0;i<counts[0];++i){compare(tables[0][i],i,0,0,0);compare(tables[0][i]-1,i,0,0,0);compare(tables[0][i]+1,i,0,0,0);}
    uint64_t state=UINT64_C(0xd1ec741);for(unsigned i=0;i<65536;++i)compare(MAIN_BASE+step(&state)%MAIN_BUCKET_BYTES,i,0,0,0);
    for(unsigned k=1;k<5;++k)for(unsigned i=0;i<counts[k];++i)compare(tables[k][i],i,0,0,0);
    for(direct_policy=0;direct_policy<3;++direct_policy){
        for(audio_mode=0;audio_mode<3;++audio_mode)for(unsigned i=0;i<counts[5];++i)compare(tables[5][i],i,0,0,0);audio_mode=0;
        for(unsigned ready=0;ready<2;++ready){manual_ready=(int)ready;for(unsigned i=0;i<counts[7];++i)for(unsigned j=0;j<32;++j)compare(tables[7][i],j,0,0,0);}manual_ready=1;
        for(unsigned i=0;i<128;++i)compare(tables[0][i*997u%counts[0]],i,0,0,0);
    }direct_policy=0;
    direct_policy=2;for(audio_mode=0;audio_mode<3;++audio_mode)for(unsigned i=0;i<counts[0];++i)
        if(tables[0][i]>=OPENAL_MIN&&tables[0][i]<=OPENAL_MAX)compare(tables[0][i],i,0,0,0);
    direct_policy=0;audio_mode=0;
    const uint64_t spaces[][2]={{HLE_BASE,HLE_COUNT*8},{0xe5e40000,2048*8},{0xe5e60000,92*8},{0xe5e80000,185*8},{0xe5ea0000,22*8},{0xe5f00000,54*256*8},{0xe6000000,7*256*8},{0xe7000000,11*8},{0xe7000100,32*8}};
    for(direct_policy=0;direct_policy<3;++direct_policy)for(unsigned i=0;i<sizeof(spaces)/sizeof(*spaces);++i){uint64_t base=spaces[i][0],bytes=spaces[i][1];
        for(unsigned j=0;j<bytes/8;++j)compare(base+j*8,j,0,0,0);compare(base-1,i,0,0,0);compare(base+1,i,0,0,0);compare(base+bytes,i,0,0,0);}
    direct_policy=0;payload_ready=dependency_ready=0;compare(tables[3][0],1,0,0,0);compare(tables[4][0],2,0,0,0);payload_ready=dependency_ready=1;
    import_enabled=0;compare(HLE_BASE,1,0,0,0);import_enabled=1;
    for(unsigned depth=0;depth<=5;++depth){recursive_limit=depth;for(unsigned policy=0;policy<3;++policy){direct_policy=(int)policy;
        compare(tables[0][0],depth,0,0,0);compare(tables[0][0],depth,4094,0,0);compare(tables[0][0],depth,0,2,0);compare(tables[0][0],depth,0,0,1);}}
    recursive_limit=0;direct_policy=0;
    compare(tables[0][0],1,4096,0,0);compare(tables[0][0],1,0,1,0);compare(tables[0][0],1,0,0,1);
    fp_cases();parallel();
    printf("{\"success\":%s,\"full_CPU_cases\":%"PRIu64",\"ordered_trace_cases\":%"PRIu64",\"parallel_cases\":%"PRIu64",\"FP_modes\":32,\"failures\":%"PRIu64",\"main_entries\":%u,\"manual_adapters\":5,\"OpenAL_entries\":%u,\"generic_and_known_entry_tested\":true,\"recursive_mixed_calls\":true,\"game_gain_proven\":false}\n",failures?"false":"true",case_count,trace_checks,parallel_cases,failures,counts[0],counts[5]);return failures?1:0;
#endif
}
#endif
