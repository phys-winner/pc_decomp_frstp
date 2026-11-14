/* Same extracted dispatch bodies in an actual dlopen DSO. Fixed guest entries
 * come from the clear-city profile; weights are synthetic, not sampled counts. */
static volatile unsigned bucket64_trash_sink;
static unsigned bucket64_body_kind(uint64_t target){
    fn_t fn=baseline_main_lookup(target);
    if(fn==canonical)return 1;if(fn==supplemental)return 2;if(fn==extra)return 3;
    if(fn==resize)return 4;if(fn==texture)return 5;if(fn==shader_body)return 6;
    exit(4);
}
static void bucket_bench(unsigned iterations){
    static const uint64_t renderer[]={
        UINT64_C(0x140e33250),UINT64_C(0x140e33410),UINT64_C(0x141023fb0),UINT64_C(0x141ae320a),
        UINT64_C(0x141ae3216),UINT64_C(0x140d9b2a0),UINT64_C(0x141021fd0),UINT64_C(0x141024013)};
    static const uint64_t main_entries[]={
        UINT64_C(0x140eb45f0),UINT64_C(0x140f8f489),UINT64_C(0x140eb4a3d),UINT64_C(0x140e52cd0),
        UINT64_C(0x140e26c40),UINT64_C(0x141c11c70),UINT64_C(0x140f4cba0),UINT64_C(0x140e27cf0)};
    const char *names[]={"renderer-hot-E33250","renderer-fixed8","main-sampled-entry8",
        "mixed-main512","spread-main1024","cold-spread-main8192","main-and-fallback64"};
    unsigned lengths[]={1,8,8,512,1024,8192,64};
    uint64_t (*input)[8192]=calloc(7,sizeof(*input)),(*sums)[8192]=calloc(7,sizeof(*sums));
    volatile unsigned char *trash=calloc(1,32u*1024u*1024u);if(!input||!sums||!trash)exit(4);
    input[0][0]=renderer[0];memcpy(input[1],renderer,sizeof(renderer));memcpy(input[2],main_entries,sizeof(main_entries));
    uint64_t state=UINT64_C(0x64256b0c37);
    for(unsigned k=3;k<7;++k)for(unsigned i=0;i<lengths[k];++i){
        uint64_t target;do{target=tables[0][random_step(&state)%counts[0]];}while(!benchmark_eligible(target));
        input[k][i]=target;
    }
    for(unsigned i=0;i<512;++i)if(i%4==0)input[3][i]=renderer[(i/4)%8];else if(i%4==1)input[3][i]=main_entries[(i/4)%8];
    for(unsigned i=0;i<64;++i){if(i%4==1)input[6][i]=tables[1][i%counts[1]];else if(i%4==2)input[6][i]=tables[2][i%counts[2]];}
    for(unsigned k=0;k<7;++k)for(unsigned i=0;i<lengths[k];++i){
        if(!eligible(input[k][i]))exit(4);sums[k][i]=input[k][i]+bucket64_body_kind(input[k][i]);
    }
    CPU c={0};baseline_budget=candidate_budget=0;baseline_trace=candidate_trace=0;
    printf("{\"success\":true,\"kind\":\"bucket64_vs_current256_DSO_model\",\"rounds\":7,\"baseline_index_bytes\":%zu,\"candidate_index_bytes\":%zu,\"baseline_init_ns\":%" PRIu64 ",\"candidate_init_ns\":%" PRIu64 ",\"scenarios\":[",sizeof(baseline_main_bucket_index),sizeof(main_bucket_index),baseline_init_ns,bucket_init_ns);
    for(unsigned scenario=0;scenario<7;++scenario){
        unsigned calls=scenario==5?lengths[scenario]:iterations;uint64_t times[2][7],checksums[2]={0},expected=0;
        for(unsigned i=0;i<calls;++i)expected+=sums[scenario][i%lengths[scenario]];
        for(unsigned round=0;round<7;++round)for(unsigned turn=0;turn<2;++turn){
            unsigned variant=(round+turn)&1;c.rax=0;
            if(scenario==5){for(unsigned i=0;i<32u*1024u*1024u;i+=64)trash[i]=(unsigned char)(i+round);bucket64_trash_sink=trash[(round+1)*64];}
            uint64_t begin=now_ns();
            for(unsigned i=0;i<calls;++i){uint64_t target=input[scenario][i%lengths[scenario]];c.rsp=(uintptr_t)&stack_words[32];if(variant)candidate_dispatch(&c,target);else baseline_dispatch(&c,target);}
            times[variant][round]=now_ns()-begin;if(c.rax!=expected)exit(4);checksums[variant]^=c.rax;
        }
        if(checksums[0]!=checksums[1])exit(4);
        printf("%s{\"name\":\"%s\",\"calls_per_round\":%u,\"distinct_sequence_length\":%u,\"baseline_ns\":[",scenario?",":"",names[scenario],calls,lengths[scenario]);
        for(unsigned i=0;i<7;++i)printf("%s%" PRIu64,i?",":"",times[0][i]);printf("],\"candidate_ns\":[");
        for(unsigned i=0;i<7;++i)printf("%s%" PRIu64,i?",":"",times[1][i]);printf("],\"checksum\":%" PRIu64 "}",checksums[0]);
    }
    printf("],\"checksums_verified\":true,\"trace_compiled_out\":true,\"native_provider_bodies\":false,\"synthetic_weights\":true,\"cold_eviction_bytes\":33554432,\"cold_eviction_outside_timing\":true,\"startup_FPS_claim\":false}\n");
    free((void*)trash);free(sums);free(input);
}
