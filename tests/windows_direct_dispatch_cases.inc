/* Isolated ABI checks; the host and shader implementation are production bodies. */
static unsigned direct_static_cases,direct_native_cases,direct_self_cases,direct_shader_cases;
static void direct_forbidden(CPU *c) {(void)c;assert(!"excluded linked entry executed");}
static __declspec(noinline) uint64_t direct_native(uint64_t value) {
    assert(GetLastError()==0xe5ef1234);
    SetLastError(0xe5ef7654);return value+19;
}
static unsigned char shader_item[2048];
static void shader_append(CPU *c) {
    assert(active_cpu==c && call_depth==5 && GetLastError()==0xe5ef1234);
    returned(c,(uintptr_t)shader_item);SetLastError(0xe5ef7654);
}
static void direct_entry_cases(uint64_t *stack) {
    CPU outer={0};CPU *saved_cpu=active_cpu;int saved_depth=call_depth;
    unsigned mxcsr=_mm_getcsr();
    for(unsigned nested=0;nested<2;++nested)for(unsigned mode=0;mode<3;++mode) {
        CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);
        c.rcx=UINT64_C(0x123456789abcdef0);wr64(c.rsp,UINT64_C(0xe5efcafe));
        CPU expected=c;unsigned char expected_stack[128];
        memcpy(expected_stack,(void *)(uintptr_t)(c.rsp-64),sizeof(expected_stack));
        if(nested)wr64((uintptr_t)(expected_stack+56),UINT64_C(0xe5efd00d));
        expected.rax=(c.rcx^UINT64_C(0xe5ef1234))+(nested?7:0);
        expected.rsp+=8;expected.rip=UINT64_C(0xe5efcafe);
        active_cpu=&outer;call_depth=3;expected_static_depth=4+nested;direct_nested_mode=mode==1;
        SetLastError(0xe5ef1234);
        uint64_t target=GVA(nested?0x141c56240:0x141c56220);
        if(!mode)test_normal_entry(&c,target);
        else test_direct_entry(&c,target,mode==2?NULL:(nested?static_nested:static_leaf));
        assert(!memcmp(&c,&expected,sizeof(c)));
        assert(!memcmp(expected_stack,(void *)(uintptr_t)(expected.rsp-8-64),sizeof(expected_stack)));
        assert(GetLastError()==0xe5ef7654 && _mm_getcsr()==mxcsr);
        assert(active_cpu==&outer && call_depth==3 && last_target==GVA(0x141c56220));
        ++direct_static_cases;
    }
    direct_nested_mode=0;
    for(unsigned mode=0;mode<3;++mode) {
        CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);
        c.rcx=0xe5ef7654;wr64(c.rsp,UINT64_C(0xe5efcafe));CPU expected=c;
        expected.rax=1;expected.rsp+=8;expected.rip=UINT64_C(0xe5efcafe);
        self_test_mode=1;active_cpu=&outer;call_depth=3;SetLastError(0xe5ef1234);
        if(!mode)test_normal_entry(&c,(uintptr_t)game.base+80);
        else test_direct_entry(&c,(uintptr_t)game.base+80,mode==1?direct_forbidden:NULL);
        assert(!memcmp(&c,&expected,sizeof(c)) && GetLastError()==0xe5ef7654);
        assert(active_cpu==&outer && call_depth==3 && _mm_getcsr()==mxcsr);++direct_self_cases;
    }
    self_test_mode=0;
    CPU baseline={0};unsigned char baseline_stack[512];
    for(unsigned mode=0;mode<3;++mode) {
        memset(stack+4088,0x6b,sizeof(baseline_stack));
        CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);
        c.rcx=UINT64_C(0x123456789abcdef0);wr64(c.rsp,UINT64_C(0xe5efcafe));
        active_cpu=&outer;call_depth=3;SetLastError(0xe5ef1234);
        if(!mode)test_normal_entry(&c,(uintptr_t)direct_native);
        else test_direct_entry(&c,(uintptr_t)direct_native,mode==1?direct_forbidden:NULL);
        assert(c.rax==UINT64_C(0x123456789abcdef0)+19 && c.rsp==(uintptr_t)(stack+4097));
        assert(c.rip==UINT64_C(0xe5efcafe) && GetLastError()==0xe5ef7654);
        assert(active_cpu==&outer && call_depth==3 && _mm_getcsr()==mxcsr);
        if(!mode){baseline=c;memcpy(baseline_stack,stack+4088,sizeof(baseline_stack));}
        else {assert(!memcmp(&baseline,&c,sizeof(c)));assert(!memcmp(baseline_stack,stack+4088,sizeof(baseline_stack)));}
        ++direct_native_cases;
    }
    active_cpu=saved_cpu;call_depth=saved_depth;
}
static void direct_shader_cases_run(uint64_t *stack) {
    fp_image saved_game=game;int64_t saved_delta=g_image_delta;
    CPU outer={0};CPU *saved_cpu=active_cpu;int saved_depth=call_depth;
    unsigned mxcsr=_mm_getcsr();
    for(unsigned relocated=0;relocated<1;++relocated) {
        size_t size=0x1c60000;void *requested=relocated?NULL:(void *)(uintptr_t)UINT64_C(0x140000000);
        unsigned char *image=VirtualAlloc(requested,size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(image);
        fp_section section={0,(uint32_t)size,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ};
        game=(fp_image){0};game.base=image;game.size=size;game.preferred_base=UINT64_C(0x140000000);
        game.sections=&section;game.section_count=1;g_image_delta=(int64_t)(uintptr_t)image-INT64_C(0x140000000);
        assert(relocated?g_image_delta!=0:g_image_delta==0);
        memcpy(image+0x10596a9,gate_0,sizeof(gate_0));memcpy(image+0x10597e9,gate_1,sizeof(gate_1));
        assert(fp_shader_lookup_init(&game));direct_image_init();
        direct_entry_cases(stack);
        for(unsigned variant=0;variant<2;++variant) {
            uint64_t target=GVA(variant?0x1410597e9:0x1410596a9);
            assert(lookup(target)==(variant?frostpunk_shader_lookup_1410597e9:frostpunk_shader_lookup_1410596a9));
            CPU baseline={0};unsigned char baseline_stack[512];uint64_t output=0;
            for(unsigned mode=0;mode<3;++mode) {
                memset(stack+4088,0x6b,sizeof(baseline_stack));
                CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);
                c.r11=0;c.r14=(uintptr_t)&output;c.rbx=0;c.r15=0;
                wr64(c.rsp,UINT64_C(0xe5efcafe));output=0;
                active_cpu=&outer;call_depth=3;SetLastError(0xe5ef1234);
                if(!mode)test_normal_entry(&c,target);
                else test_direct_entry(&c,target,mode==1?direct_forbidden:NULL);
                assert(output==(uintptr_t)shader_item+(variant?0x280:0x200));
                assert(active_cpu==&outer && call_depth==3 && _mm_getcsr()==mxcsr);
                assert(GetLastError()==0xe5ef7654 && last_target==GVA(0x141058000));
                if(!mode){baseline=c;memcpy(baseline_stack,stack+4088,sizeof(baseline_stack));}
                else {assert(!memcmp(&baseline,&c,sizeof(c)));assert(!memcmp(baseline_stack,stack+4088,sizeof(baseline_stack)));}
                ++direct_shader_cases;
            }
        }
        direct_image_reset();assert(VirtualFree(image,0,MEM_RELEASE));
    }
    game=saved_game;g_image_delta=saved_delta;fp_shader_lookup_init(&game);direct_image_init();
    active_cpu=saved_cpu;call_depth=saved_depth;
}

static int direct_range_ran;
static unsigned direct_range_fast_calls;
static void direct_range_linked(CPU *c) {++direct_range_fast_calls;static_leaf(c);}
static void direct_range_image(void) {
    void *image=VirtualAlloc((void *)(uintptr_t)UINT64_C(0x140000000),0x1c60000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(image==(void *)(uintptr_t)UINT64_C(0x140000000));
    fp_section sections[]={{0x1c56000,0x100,IMAGE_SCN_MEM_READ},
        {0x1c56220,0x20,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ},
        {0x1c56240,0x10,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ}};
    game=(fp_image){0};game.base=image;game.size=0x1c60000;game.preferred_base=(uintptr_t)image;
    game.sections=malloc(sizeof(sections));assert(game.sections);memcpy(game.sections,sections,sizeof(sections));
    game.section_count=3;g_image_delta=0;self_test_mode=0;
}
static void direct_range_probe(uint64_t *stack,int nested,int forbidden) {
    CPU outer={0},c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(stack+4096);
    c.rcx=UINT64_C(0x123456789abcdef0);wr64(c.rsp,UINT64_C(0xe5efcafe));
    CPU expected=c;expected.rax=(c.rcx^UINT64_C(0xe5ef1234))+(nested?7:0);
    expected.rsp+=8;expected.rip=UINT64_C(0xe5efcafe);
    CPU *saved=active_cpu;int depth=call_depth;unsigned mxcsr=_mm_getcsr();
    active_cpu=&outer;call_depth=3;expected_static_depth=4+nested;direct_nested_mode=1;
    unsigned calls=direct_range_fast_calls;SetLastError(0xe5ef1234);
    test_direct_entry(&c,GVA(nested?0x141c56240:0x141c56220),forbidden?direct_forbidden:(nested?static_nested:direct_range_linked));
    assert(direct_range_fast_calls==calls+(!forbidden && !nested));
    assert(!memcmp(&c,&expected,sizeof(c)) && GetLastError()==0xe5ef7654 && _mm_getcsr()==mxcsr);
    assert(active_cpu==&outer && call_depth==3 && last_target==GVA(0x141c56220));
    active_cpu=saved;call_depth=depth;direct_nested_mode=0;
}
static void direct_range_cases(uint64_t *stack) {
    fp_image saved_game=game;int64_t saved_delta=g_image_delta;
    direct_image_reset();direct_range_image();
    assert(!direct_code_contains(UINT64_C(0x141c56220)));direct_range_probe(stack,0,1);
    SetLastError(0xe5ef1234);direct_image_init();assert(GetLastError()==0xe5ef1234);
    assert(direct_code_begin==UINT64_C(0x141c56220) && direct_code_size==0x20);
    assert(direct_code_contains(direct_code_begin) && direct_code_contains(direct_code_begin+direct_code_size-1));
    assert(!direct_code_contains(direct_code_begin-1) && !direct_code_contains(direct_code_begin+direct_code_size));
    assert(!direct_code_contains(0) && !direct_code_contains(UINT64_MAX));
    direct_entry_cases(stack);direct_range_probe(stack,0,0);direct_range_probe(stack,1,1);
    g_image_delta=16;direct_range_probe(stack,0,1);g_image_delta=0;
    direct_image_reset();assert(!direct_code_contains(UINT64_C(0x141c56220)));direct_range_probe(stack,0,1);
    fp_unload_image(&game);assert(!game.base);direct_image_init();assert(!direct_code_size);
    direct_range_image();game.sections[1].rva+=0x100;direct_image_init();
    assert(!direct_code_contains(UINT64_C(0x141c56220)) && direct_code_contains(UINT64_C(0x141c56320)));
    direct_image_reset();game.sections[1].size=0;game.sections[2].size=0;direct_image_init();assert(!direct_code_size);
    direct_image_reset();fp_unload_image(&game);
    game=saved_game;g_image_delta=saved_delta;direct_image_init();direct_range_ran=1;
}
static void direct_range_reject(const wchar_t *mode) {
    direct_image_reset();direct_range_image();direct_image_init();uint64_t target;
    if(!wcscmp(mode,L"--range-before"))target=direct_code_begin-1;
    else if(!wcscmp(mode,L"--range-gap"))target=UINT64_C(0x141c56180);
    else if(!wcscmp(mode,L"--range-data"))target=UINT64_C(0x141c56010);
    else if(!wcscmp(mode,L"--range-header"))target=UINT64_C(0x140000000);
    else {assert(!wcscmp(mode,L"--range-after"));target=UINT64_C(0x141c56250);}
    assert(!fp_image_code(&game,target) && !direct_code_contains(target));
    CPU c={0};uint64_t ret=0xe5efcafe;c.rsp=(uintptr_t)&ret;
    test_direct_entry(&c,target,direct_forbidden);assert(!"non-code range entry accepted");
}

/* Admission is a subset of the loader classifier, using immutable metadata. */
static void direct_range_metadata_cases(void) {
    fp_image saved=game;int64_t delta=g_image_delta;
    fp_section sections[]={{0x1000,0x1cc6000,IMAGE_SCN_MEM_EXECUTE},
        {0x4506000,0x32518,IMAGE_SCN_MEM_EXECUTE}};
    direct_image_reset();game=(fp_image){0};game.base=(void *)(uintptr_t)UINT64_C(0x140000000);
    game.size=0x4539000;game.sections=sections;game.section_count=2;direct_image_init();
    assert(direct_code_begin==UINT64_C(0x140001000) && direct_code_size==0x1cc6000);
    uint64_t edges[]={UINT64_C(0x140000fff),UINT64_C(0x140001000),UINT64_C(0x141cc6fff),
        UINT64_C(0x141cc7000),UINT64_C(0x144505fff),UINT64_C(0x144506000),
        UINT64_C(0x144538517),UINT64_C(0x144538518),0,UINT64_MAX};
    for(unsigned i=0;i<sizeof(edges)/sizeof(edges[0]);++i) {
        int primary=edges[i]>=UINT64_C(0x140001000) && edges[i]<UINT64_C(0x141cc7000);
        int other=edges[i]>=UINT64_C(0x144506000) && edges[i]<UINT64_C(0x144538518);
        assert(direct_code_contains(edges[i])==primary);
        assert(fp_image_code(&game,edges[i])==(primary||other));
    }
    direct_image_reset();sections[0]=(fp_section){7,3,IMAGE_SCN_MEM_EXECUTE};
    sections[1]=(fp_section){20,2,IMAGE_SCN_MEM_EXECUTE};game.size=32;direct_image_init();
    for(uint64_t i=0;i<34;++i)assert(direct_code_contains((uintptr_t)game.base+i)==(i>=7&&i<10));
    direct_image_reset();sections[0].size=0;sections[1].size=0;direct_image_init();assert(!direct_code_size);
    direct_image_reset();sections[0]=(fp_section){33,1,IMAGE_SCN_MEM_EXECUTE};direct_image_init();assert(!direct_code_size);
    direct_image_reset();sections[0]=(fp_section){31,2,IMAGE_SCN_MEM_EXECUTE};direct_image_init();assert(!direct_code_size);
    direct_image_reset();sections[0]=(fp_section){0,32,IMAGE_SCN_MEM_READ};direct_image_init();assert(!direct_code_size);
    direct_image_reset();sections[0].characteristics=IMAGE_SCN_MEM_EXECUTE;game.section_count=0;
    direct_image_init();assert(!direct_code_size);
    direct_image_reset();game.section_count=2;game.sections=NULL;direct_image_init();assert(!direct_code_size);
    direct_image_reset();game.sections=sections;game.base=NULL;direct_image_init();assert(!direct_code_size);
    direct_image_reset();game.base=(void *)(uintptr_t)(UINT64_MAX-15);direct_image_init();assert(!direct_code_size);
    direct_image_reset();game=saved;g_image_delta=delta;direct_image_init();
}

#if TEST_FIXED_GVA
/* Four exact entry bodies cover both predecessor paths of two generated leaves. */
static unsigned fixed_gva_cases,fixed_gva_scope_probes;
static void fixed_gva_reject(const wchar_t *mode) {
    int positive=!wcscmp(mode,L"--gva-delta-positive");
    assert(positive || !wcscmp(mode,L"--gva-delta-negative"));
    game.preferred_base=UINT64_C(0x140000000);
    game.base=(void *)(uintptr_t)(game.preferred_base+(positive?0x10000:-INT64_C(0x10000)));
    fixed_gva_actual_startup_gate();
    fputs("FIXED_GVA_ENTRY_REACHED\n",stderr);assert(!"nonzero delta admitted");
}
static void fixed_gva_cases_run(uint64_t *stack) {
    int64_t saved_delta=g_image_delta;fp_image saved_game=game;
    unsigned saved_mxcsr=_mm_getcsr();DWORD saved_error=GetLastError();
    const uint64_t address=UINT64_C(0x142B4BF70);
    int64_t deltas[]={-INT64_C(0x10000),0,INT64_C(0x10000)};
    for(unsigned i=0;i<3;++i) {
        g_image_delta=deltas[i];
        assert(GVA(address)==address+deltas[i]);++fixed_gva_scope_probes;
        assert(test_main_GVA_probe(address)==address);++fixed_gva_scope_probes;
    }
    game.preferred_base=UINT64_C(0x140000000);game.base=(void *)(uintptr_t)game.preferred_base;
    fixed_gva_actual_startup_gate();assert(!g_image_delta);
    unsigned char *reserve=VirtualAlloc((void *)(uintptr_t)UINT64_C(0x142B40000),0x10000,MEM_RESERVE,PAGE_NOACCESS);
    assert(reserve==(void *)(uintptr_t)UINT64_C(0x142B40000));
    unsigned char *globals=VirtualAlloc((void *)(uintptr_t)UINT64_C(0x142B4B000),8192,MEM_COMMIT,PAGE_READWRITE);
    assert(globals==(void *)(uintptr_t)UINT64_C(0x142B4B000));
    void (*dynamic[])(CPU *)={fixed_gva_dynamic_0,fixed_gva_dynamic_1,fixed_gva_dynamic_2,fixed_gva_dynamic_3};
    void (*fixed[])(CPU *)={fixed_gva_fixed_0,fixed_gva_fixed_1,fixed_gva_fixed_2,fixed_gva_fixed_3};
    uint32_t values[]={0,1,2,3,0x7fffffff,0x80000000,0xfffffffe,0xffffffff};
    unsigned controls[]={0,0x1f80,0x5fc1,0xffbf};
    uint64_t final_rips[]={UINT64_C(0x140F6D9F5),UINT64_C(0x140F6D9F3),UINT64_C(0x140F6EDE3),UINT64_C(0x140F6EDE1)};
    unsigned char input[2048],expected_input[2048],initial_globals[8192],expected_globals[8192],initial_stack[512],expected_stack[512];
    memset(input,0xa7,sizeof(input));
    for(unsigned entry=0;entry<4;++entry)for(unsigned variant=0;variant<8;++variant) {
        for(unsigned i=0;i<sizeof(initial_globals);++i)initial_globals[i]=(unsigned char)(i*17+variant);
        memset(initial_stack,0x6b,sizeof(initial_stack));
        wr32((uintptr_t)input+0x568,values[7-variant]);
        memcpy(expected_input,input,sizeof(expected_input));
        wr32((uintptr_t)initial_globals+0xf70,values[variant]);
        CPU initial;memset(&initial,0x5a,sizeof(initial));
        initial.rsp=(uintptr_t)(stack+4096);initial.rax=initial.rbx=(uintptr_t)input;
        initial.rsi=UINT64_C(0x89abcdef76543210);initial.r12=UINT64_C(0x33445566fedcba98);
        initial.r13=UINT64_C(0x1122334487654321);
        initial.cf=variant&1;initial.zf=(variant>>1)&1;initial.af=(variant>>2)&1;
        initial.sf=initial.of=initial.pf=1;initial.mxcsr=0xe5ef1234;
        CPU expected={0};unsigned control=controls[variant%4];
        for(unsigned mode=0;mode<2;++mode) {
            memcpy(globals,initial_globals,sizeof(initial_globals));
            memcpy((void *)(uintptr_t)(initial.rsp-128),initial_stack,sizeof(initial_stack));
            CPU c=initial;_mm_setcsr(control);SetLastError(0xe5ef1234+variant);
            (mode?fixed[entry]:dynamic[entry])(&c);
            assert(GetLastError()==0xe5ef1234+variant && _mm_getcsr()==control);
            assert(!memcmp(input,expected_input,sizeof(input)));
            assert(c.rsp==initial.rsp+0x50 && c.rip==final_rips[entry]);
            assert(rd32(address)==((values[variant]-1)&1));
            assert(rd32(UINT64_C(0x142B4C018))==(entry==0||entry==2?values[7-variant]:(entry==1?(uint32_t)initial.rsi:0)));
            assert(rd32(UINT64_C(0x142B4C01C))==(uint32_t)(entry<2?initial.r13:initial.r12));
            assert(rd32(UINT64_C(0x142B4C028))==(entry<2?(uint32_t)initial.rsi:0));
            if(!mode) {
                expected=c;memcpy(expected_globals,globals,sizeof(expected_globals));
                memcpy(expected_stack,(void *)(uintptr_t)(initial.rsp-128),sizeof(expected_stack));
            } else {
                assert(!memcmp(&c,&expected,sizeof(c)));
                assert(!memcmp(globals,expected_globals,sizeof(expected_globals)));
                assert(!memcmp((void *)(uintptr_t)(initial.rsp-128),expected_stack,sizeof(expected_stack)));
            }
        }
        assert(rd32((uintptr_t)input+0x568)==values[7-variant]);++fixed_gva_cases;
    }
    assert(VirtualFree(reserve,0,MEM_RELEASE));game=saved_game;g_image_delta=saved_delta;
    _mm_setcsr(saved_mxcsr);SetLastError(saved_error);
}
#endif
