/* Boundary-only synthetic calls; no original image bytes are executed. */
static unsigned boundary_cases,boundary_expected_depth,boundary_leaf_calls;
static int boundary_nested;
static unsigned boundary_host_mxcsr;
static void boundary_leaf(CPU *c) {
    assert(active_cpu==c && call_depth==(int)boundary_expected_depth);
    assert(_mm_getcsr()==boundary_host_mxcsr);
    ++boundary_leaf_calls;
    if(boundary_nested) {
        CPU nested=*c;CPU expected=nested;boundary_nested=0;++boundary_expected_depth;
        expected.rax=UINT64_C(0x123456789abcdef0);expected.rsp+=8;expected.rip=rd64(c->rsp);
        dispatch_static(&nested,GVA(0x141c56220),boundary_leaf,0);
        assert(!memcmp(&nested,&expected,sizeof(nested)));
        --boundary_expected_depth;
        assert(active_cpu==c && call_depth==(int)boundary_expected_depth);
    }
    returned(c,UINT64_C(0x123456789abcdef0));SetLastError(0xe5ef7654);
}
__declspec(noinline) void boundary_probe_a(CPU *c,uint64_t target,void (*fn)(CPU *),int self) {
    dispatch_static(c,target,fn,self);
}
__declspec(noinline) void boundary_probe_b(CPU *c,uint64_t target,void (*fn)(CPU *),int self) {
    dispatch_static(c,target,fn,self);
}
static void boundary_one(unsigned char *area,uint64_t target,unsigned route,uint64_t context,int self,int nested) {
    CPU outer={0},c;memset(&c,0x5a,sizeof(c));
    c.rsp=(uintptr_t)(area+0x8000);c.rcx=context;c.r8=(uintptr_t)(area+0x3000);
    wr64(c.rsp,UINT64_C(0xe5efcafe));CPU expected=c;
    expected.rax=self?1:UINT64_C(0x123456789abcdef0);expected.rsp+=8;expected.rip=UINT64_C(0xe5efcafe);
    unsigned char stack_before[128];memcpy(stack_before,(void *)(uintptr_t)(c.rsp-64),128);
    active_cpu=&outer;call_depth=3;boundary_expected_depth=4;boundary_nested=nested;
    SetLastError(0xe5ef1234);errno=EDOM;
    if(route)boundary_probe_b(&c,target,boundary_leaf,self);else boundary_probe_a(&c,target,boundary_leaf,self);
    assert(!memcmp(&c,&expected,sizeof(c)));
    assert(!memcmp(stack_before,(void *)(uintptr_t)(expected.rsp-8-64),128));
    assert(active_cpu==&outer && call_depth==3 && _mm_getcsr()==boundary_host_mxcsr);
    assert(GetLastError()==(self?(DWORD)context:0xe5ef7654));
    ++boundary_cases;active_cpu=NULL;call_depth=0;
}
static int static_boundary_cases(const wchar_t *mode,unsigned char *area) {
    boundary_host_mxcsr=_mm_getcsr();
    if(wcsstr(mode,L"overflow")) {
        CPU c={0};c.rsp=(uintptr_t)(area+0x8000);wr64(c.rsp,UINT64_C(0xe5efcafe));
        call_depth=8192;trace_calls=wcsstr(mode,L"cold")!=NULL;
        boundary_probe_a(&c,UINT64_C(0x141c56220),boundary_leaf,0);
        assert(!"recursion limit accepted");return 1;
    }
    assert(!wcscmp(mode,L"--static-check"));
    uintptr_t context=(uintptr_t)(area+0x1000),application=(uintptr_t)(area+0x2000);
    uintptr_t config=(uintptr_t)(area+0x4000),settings=config+0xb0;
    uintptr_t options=(uintptr_t)(area+0x5000),raw=(uintptr_t)(area+0x3000);
    strcpy((char *)raw,"-gfxconfig -novideo");
    wr64(context+0x30,application);wr64(context+0x38,options);wr64(application,raw);wr64(application+0x28,config);
    wr8(options+0x20,1);wr32(settings+0x1c,4);wr8(settings+0x20,1);wr32(settings+0x28,2);
    wr8(settings+0x2c,1);wr32(settings+0x30,3);wr32(settings+0x34,1920);wr32(settings+0x38,1080);
    wr32(settings+0x3c,60);wr8(settings+0x90,1);wr32(settings+0x94,1280);wr32(settings+0x98,720);wr32(settings+0x9c,75);
    static const uint64_t targets[]={UINT64_C(0x141c56220),UINT64_C(0x141add740),UINT64_C(0x140e02420),UINT64_C(0x141adea60)};
    static const unsigned controls[]={0x1f80,0xffc0,0x3f};
    for(unsigned relocated=0;relocated<2;++relocated)for(unsigned route=0;route<2;++route)
    for(unsigned invalid=0;invalid<2;++invalid)for(unsigned kind=0;kind<4;++kind) {
        g_image_delta=relocated?INT64_C(0x40000000):0;trace_calls=route;
        boundary_host_mxcsr=controls[boundary_cases%3];_mm_setcsr(boundary_host_mxcsr);
        boundary_one(area,GVA(targets[kind]),route,invalid?1:context,0,kind==0);
    }
    game.base=(unsigned char *)(uintptr_t)UINT64_C(0x600000000);self_test_mode=1;
    for(unsigned route=0;route<2;++route) {
        trace_calls=route;boundary_one(area,(uintptr_t)game.base+80,route,0xe5ef7654,1,0);
    }
    self_test_mode=trace_calls=0;
    /* The last permitted depth must still run in both hot and cold routes. */
    for(unsigned cold=0;cold<2;++cold) {
        CPU c={0},outer={0};c.rsp=(uintptr_t)(area+0x8000);wr64(c.rsp,UINT64_C(0xe5efcafe));
        active_cpu=&outer;call_depth=8191;boundary_expected_depth=8192;trace_calls=cold;
        boundary_probe_a(&c,UINT64_C(0x141c56220),boundary_leaf,0);
        assert(call_depth==8191 && active_cpu==&outer && GetLastError()==0xe5ef7654);++boundary_cases;
    }
    printf("{\"success\":true,\"static_boundary_cases\":%u,\"leaf_calls\":%u}\n",boundary_cases,boundary_leaf_calls);
    return 0;
}
