/* Reuse the unchanged bridge fixture's guest memory and Win64 frame helpers. */
#define main unchanged_bridge_fixture_main
#define win64_pointer base_fixture_pointer
#include "nativeD3D11_bridge.cpp"
#undef win64_pointer
#undef main

static uint64_t watched_source;
static size_t watched_span, observed_span;
static size_t negative_size_t;

extern "C" void* win64_pointer(uint64_t p,size_t n,int write) {
    if (negative_size_t && p==reinterpret_cast<uintptr_t>(native_vs)) {
        fprintf(stderr,"SIZE_T_BOUNDARY requested=%zu expected=%zu\n",n,negative_size_t);
        if (n==negative_size_t && !accessible(p,n,write!=0)) _Exit(73);
        _Exit(74);
    }
    if (watched_source && p==watched_source && !write) {
        observed_span=n;
        if (n!=watched_span) {
            fprintf(stderr,"UINT_PITCH_BOUNDARY requested=%zu expected=%zu\n",n,watched_span);
            _Exit(75);
        }
    }
    return base_fixture_pointer(p,n,write);
}

static uint64_t uint_call(uint64_t object,unsigned slot,std::initializer_list<uint64_t> values={}) {
    std::vector<uint64_t> args{object};args.insert(args.end(),values.begin(),values.end());
    memset(&cpu,0xa5,sizeof(cpu));cpu.rsp=stack+256;
    uint64_t* registers[]={&cpu.rcx,&cpu.rdx,&cpu.r8,&cpu.r9};
    for(size_t i=0;i<args.size();++i) {
        if(i<4)*registers[i]=args[i];else write64(cpu.rsp+40+(i-4)*8,args[i]);
    }
    CPU before=cpu;unsigned char saved_stack[8192];memcpy(saved_stack,reinterpret_cast<void*>(stack),sizeof(saved_stack));
    require(win64_d3d11_dispatch(&cpu,read64(read64(object)+slot*8))==1,"UINT bridge method not handled");
    before.rax=cpu.rax;require(!memcmp(&cpu,&before,sizeof(cpu)),"UINT method changed CPU except RAX");
    require(!memcmp(saved_stack,reinterpret_cast<void*>(stack),sizeof(saved_stack)),"UINT method changed guest stack");
    ++calls;return cpu.rax;
}

static uint64_t guarded(size_t bytes) {
    uint64_t p=win64_alloc(bytes+64,0);memset(reinterpret_cast<void*>(p),0xa5,bytes+64);return p+32;
}
static void guard_check(uint64_t p,size_t bytes) {
    auto* b=reinterpret_cast<unsigned char*>(p);
    for(unsigned i=0;i<32;++i)require(b[int(i)-32]==0xa5&&b[bytes+i]==0xa5,"UINT buffer canary changed");
}

static size_t texture_case(uint64_t device,uint64_t context,bool volume,bool dirty) {
    const unsigned width=volume?7:1024,height=volume?4:128,depth=volume?3:1;
    const unsigned row_bytes=width*16,row_pitch=volume?160:49152,slice_pitch=volume?800:row_pitch*height;
    const size_t span=size_t(slice_pitch)*(depth-1)+size_t(row_pitch)*(height-1)+row_bytes;
    uint64_t source=guarded(span);memset(reinterpret_cast<void*>(source),0xcd,span);
    for(unsigned z=0;z<depth;++z)for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width*4;++x) {
        uint32_t value=0x3f000000u|((z*0x10001u+y*4099u+x*17u)&0x7fffffu);
        memcpy(reinterpret_cast<void*>(source+z*slice_pitch+y*row_pitch+x*4),&value,4);
    }
    std::vector<unsigned char> saved(span);memcpy(saved.data(),reinterpret_cast<void*>(source),span);
    uint64_t target_out=output(),staging_out=output();
    if(volume) {
        D3D11_TEXTURE3D_DESC td={};td.Width=width;td.Height=height;td.Depth=depth;td.MipLevels=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;
        hr(uint_call(device,6,{put(td),0,target_out}),"Create default 3D");
        td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(uint_call(device,6,{put(td),0,staging_out}),"Create staging 3D");
    } else {
        D3D11_TEXTURE2D_DESC td={};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;
        hr(uint_call(device,5,{put(td),0,target_out}),"Create default 2D");
        td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(uint_call(device,5,{put(td),0,staging_out}),"Create staging 2D");
    }
    const uint64_t row=uint64_t(row_pitch)|(dirty?(UINT64_C(0x33)<<32):0);
    const uint64_t slice=uint64_t(slice_pitch)|(dirty?(UINT64_C(0x87654321)<<32):0);
    watched_source=source;watched_span=span;observed_span=0;
    uint_call(context,48,{read64(target_out),0,0,source,row,slice});
    require(observed_span==span,"UpdateSubresource exact source span not observed");watched_source=0;
    uint_call(context,47,{read64(staging_out),read64(target_out)});
    uint64_t map_out=guarded(sizeof(D3D11_MAPPED_SUBRESOURCE));
    const uint64_t map_type=D3D11_MAP_READ|(dirty?(UINT64_C(0x99887766)<<32):0);
    hr(uint_call(context,14,{read64(staging_out),0,map_type,0,map_out}),"Map UINT pitch staging");
    auto mapped=*reinterpret_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(map_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
    guard_check(map_out,sizeof(mapped));require(mapped.RowPitch>=row_bytes,"Mapped row too short");
    require(!accessible(reinterpret_cast<uintptr_t>(mapped.pData),1,true),"READ enum high32 made guest map writable");
    if(volume)require(mapped.DepthPitch>=mapped.RowPitch*height,"Mapped slice too short");
    size_t checked=0;
    for(unsigned z=0;z<depth;++z)for(unsigned y=0;y<height;++y) {
        const void* actual=win64_pointer(reinterpret_cast<uintptr_t>(mapped.pData)+size_t(z)*mapped.DepthPitch+size_t(y)*mapped.RowPitch,row_bytes,0);
        require(!memcmp(actual,saved.data()+size_t(z)*slice_pitch+size_t(y)*row_pitch,row_bytes),"UINT pitch GPU readback differs");checked+=row_bytes;
    }
    uint_call(context,15,{read64(staging_out),0});
    require(!memcmp(reinterpret_cast<void*>(source),saved.data(),span),"UpdateSubresource changed source");guard_check(source,span);
    for(uint64_t out:{target_out,staging_out}){uint_call(read64(out),2);win64_free(out);}
    win64_free(map_out-32);win64_free(source-32);return checked;
}

int main(int argc,char** argv) {
    bool success=false;std::string error;size_t readback=0;unsigned texture_cases=0;
    try {
        require(argc==3||(argc==4&&std::string(argv[3])=="--invalid-size-t"),"usage: nativeD3D11_uint32 runtimeDir driverDir/ [--invalid-size-t]");
        static_assert((UINT64_C(0x330000c000)*127+0x4000)==UINT64_C(0x194d005f8000));
        static_assert((UINT64_C(0xc000)*127+0x4000)==UINT64_C(0x5f8000));
        char detail[512]={};auto gipa=frostpunk_gpu_open(argv[1],argv[2],detail,sizeof(detail));require(gipa,detail);
        win64_d3d11_configure(gipa,nullptr);stack=win64_alloc(8192,1);
        D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;uint64_t device_out=output(),context_out=output(),level_out=put(D3D_FEATURE_LEVEL());
        frame({0,D3D_DRIVER_TYPE_HARDWARE,0,D3D11_CREATE_DEVICE_BGRA_SUPPORT,put(level),(UINT64_C(0x9a)<<32)|1,D3D11_SDK_VERSION,device_out,level_out,context_out});
        require(win64_d3d11_import(&cpu,"d3d11.dll","D3D11CreateDevice")==1,"CreateDevice UINT fixture import");hr(cpu.rax,"CreateDevice UINT fixture");
        uint64_t device=read64(device_out),context=read64(context_out);
        if(argc==4) {
            win64_region_add(reinterpret_cast<uintptr_t>(native_vs),sizeof(native_vs),0);
            negative_size_t=(UINT64_C(1)<<32)|sizeof(native_vs);
            uint_call(device,12,{reinterpret_cast<uintptr_t>(native_vs),negative_size_t,0,output()});
            throw std::runtime_error("SIZE_T unexpectedly accepted/truncated");
        }
        for(bool volume:{false,true})for(bool dirty:{false,true}){readback+=texture_case(device,context,volume,dirty);++texture_cases;}
        const uint64_t poison=UINT64_C(0x7654321000000001);
        uint64_t array=guarded(8);write64(array,0);
        uint_call(context,25,{0,poison,array});
        write64(array,UINT64_C(0xabababababababab));uint_call(context,84,{0,poison,array});
        require(read64(array)==0,"Shader resource UINT count output");guard_check(array,8);
        uint64_t feature=guarded(sizeof(D3D11_FEATURE_DATA_THREADING));
        hr(uint_call(device,33,{D3D11_FEATURE_THREADING,feature,(UINT64_C(0xaabbccdd)<<32)|sizeof(D3D11_FEATURE_DATA_THREADING)}),"CheckFeatureSupport UINT bytes");
        guard_check(feature,sizeof(D3D11_FEATURE_DATA_THREADING));
        uint_call(context,109);uint_call(context,110);hr(uint_call(device,39),"Device removed after UINT tests");
        uint_call(context,2);uint_call(device,2);success=true;
    } catch(const std::exception& e){error=e.what();}
    printf("{\"success\":%s,\"synthetic_win64_cpu_frames\":true,\"native_aarch64\":true,\"texture_cases\":%u,\"gpu_readback_bytes\":%zu,\"raw_bad_span\":27818509434880,\"expected_2d_source_span\":6258688,\"uint_array_set_get\":%s,\"uint_feature_data_size\":%s,\"full_cpu_except_rax\":%s,\"guest_stack_and_buffer_canaries\":%s,\"com_calls\":%u,\"game_rendered\":false,\"fps_measured\":false,\"error\":\"",success?"true":"false",texture_cases,readback,success?"true":"false",success?"true":"false",success?"true":"false",success?"true":"false",calls.load());
    for(char ch:error){if(ch=='"'||ch=='\\')putchar('\\');if(ch>=32)putchar(ch);}printf("\"}\n");return success?0:1;
}
