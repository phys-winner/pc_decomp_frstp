static unsigned buffer_map_checks;

static uint64_t bookkeeping_buffer(uint64_t device,bool dynamic) {
    D3D11_BUFFER_DESC desc={};desc.ByteWidth=64;
    desc.Usage=dynamic?D3D11_USAGE_DYNAMIC:D3D11_USAGE_STAGING;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE|(dynamic?0:D3D11_CPU_ACCESS_READ);
    desc.BindFlags=dynamic?D3D11_BIND_VERTEX_BUFFER:0;
    uint64_t out=output();hr(call(device,3,{put(desc),0,out}),"bookkeeping CreateBuffer");
    return read64(out);
}

static uint64_t bookkeeping_context(uint64_t device) {
    uint64_t out=output();hr(call(device,27,{0,out}),"bookkeeping CreateDeferredContext");return read64(out);
}

static void bookkeeping_trap(uint64_t device,uint64_t context,const char* mode) {
    uint64_t buffer=bookkeeping_buffer(device,true),mapped=put(D3D11_MAPPED_SUBRESOURCE());
    if(!strcmp(mode,"--failed-map")) {
        call(buffer,2);
        D3D11_BUFFER_DESC desc={};desc.ByteWidth=64;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        uint64_t out=output();hr(call(device,3,{put(desc),0,out}),"unmappable CreateBuffer");buffer=read64(out);
        require(int32_t(call(context,14,{buffer,0,D3D11_MAP_READ,0,mapped}))<0,"expected failed Map");
        call(context,15,{buffer,0});
    } else if(!strcmp(mode,"--extra-unmap")) {
        hr(call(context,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"trap Map");
        call(context,15,{buffer,0});call(context,15,{buffer,0});
    } else if(!strcmp(mode,"--release-mapped")) {
        hr(call(context,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"trap Map");call(buffer,2);
    } else if(!strcmp(mode,"--release-one-of-two")) {
        uint64_t a=bookkeeping_context(device),b=bookkeeping_context(device);
        hr(call(a,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"first deferred Map");
        hr(call(b,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"second deferred Map");
        call(b,15,{buffer,0});call(buffer,2);
    } else require(false,"unknown bookkeeping trap mode");
    require(false,"expected bookkeeping trap did not occur");
}

static void bookkeeping_maps(uint64_t device,uint64_t context) {
    uint64_t buffer=bookkeeping_buffer(device,false),base_out=output(),unknown_out=output();
    hr(call(buffer,0,{put(__uuidof(ID3D11Resource)),base_out}),"bookkeeping Resource alias");
    hr(call(buffer,0,{put(__uuidof(IUnknown)),unknown_out}),"bookkeeping IUnknown alias");
    uint64_t base=read64(base_out),unknown=read64(unknown_out),mapped=put(D3D11_MAPPED_SUBRESOURCE());
    for(auto pair:{std::make_pair(buffer,base),std::make_pair(base,buffer),std::make_pair(buffer,unknown)}) {
        hr(call(context,14,{pair.first,0,D3D11_MAP_WRITE,0,mapped}),"cross-alias Map");
        call(context,15,{pair.second,0});++buffer_map_checks;
    }
    hr(call(context,14,{buffer,0,D3D11_MAP_WRITE,0,mapped}),"alias retention Map");
    call(buffer,2);call(context,15,{base,0});
    uint64_t typed_out=output();hr(call(base,0,{put(__uuidof(ID3D11Buffer)),typed_out}),"restore typed alias");
    require(read64(typed_out)==buffer,"mapped typed proxy identity changed");++buffer_map_checks;
    hr(call(context,14,{buffer,0,D3D11_MAP_WRITE,0,mapped}),"rehash Map");
    std::vector<uint64_t> extras;
    for(unsigned i=0;i<256;++i)extras.push_back(bookkeeping_buffer(device,true));
    call(context,15,{unknown,0});for(auto extra:extras)call(extra,2);++buffer_map_checks;
    call(buffer,2);call(base,2);call(unknown,2);

    buffer=bookkeeping_buffer(device,true);
    hr(call(buffer,0,{put(__uuidof(ID3D11Resource)),base_out}),"dynamic Resource alias");base=read64(base_out);
    hr(call(buffer,0,{put(__uuidof(IUnknown)),unknown_out}),"dynamic IUnknown alias");unknown=read64(unknown_out);
    uint64_t a=bookkeeping_context(device),b=bookkeeping_context(device);
    for(unsigned order=0;order<2;++order) {
        hr(call(a,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"simultaneous typed Map");
        hr(call(b,14,{order?base:buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"simultaneous alias Map");
        if(order){call(a,15,{unknown,0});call(b,15,{buffer,0});}
        else{call(b,15,{base,0});call(a,15,{unknown,0});}
        ++buffer_map_checks;
    }
    call(a,2);call(b,2);
    std::vector<uint64_t> contexts;
    for(unsigned i=0;i<4;++i)contexts.push_back(bookkeeping_context(device));
    std::vector<std::thread> workers;
    for(unsigned t=0;t<4;++t)workers.emplace_back([&,t]{
        stack=win64_alloc(8192,1);uint64_t out=put(D3D11_MAPPED_SUBRESOURCE());
        for(unsigned i=0;i<64;++i){
            hr(call(contexts[t],14,{i&1?base:buffer,0,D3D11_MAP_WRITE_DISCARD,0,out}),"concurrent deferred Map");
            call(contexts[t],15,{i&1?buffer:unknown,0});
        }
        win64_free(out);win64_free(stack);stack=0;
    });
    for(auto& worker:workers)worker.join();buffer_map_checks+=256;
    for(auto ctx:contexts)call(ctx,2);
    a=bookkeeping_context(device);
    std::thread([&]{stack=win64_alloc(8192,1);hr(call(a,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped}),"handoff Map");win64_free(stack);stack=0;}).join();
    std::thread([&]{stack=win64_alloc(8192,1);call(a,15,{base,0});win64_free(stack);stack=0;}).join();
    ++buffer_map_checks;call(a,2);call(buffer,2);call(base,2);call(unknown,2);
}
