/* Synthetic Win64 CPU frames exercise the real COM bridge on native ARM64.
 * This fixture implements only the documented pointer/allocator host contract;
 * it neither links the original x64 game nor calls D3D11 directly.
 */
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "bridge.h"
#include "win64.h"
#include "driver.h"
#include "nativeD3D11_shaders.h"

struct Region { uint64_t p; size_t size,allocation; bool writable; };
static std::vector<Region> regions;
static std::recursive_mutex region_mutex;
static bool accessible(uint64_t p,size_t n,bool write) {
    std::lock_guard lock(region_mutex);
    if (!n) return true;
    for (auto r:regions) if (p>=r.p && p-r.p<=r.size && n<=r.size-(p-r.p) && (!write || r.writable)) return true;
    return false;
}
extern "C" void* win64_pointer(uint64_t p,size_t n,int write) {
    if (!accessible(p,n,write!=0)) { fprintf(stderr,"fixture invalid guest range %llx+%zu write=%d\n",(unsigned long long)p,n,write); abort(); }
    return reinterpret_cast<void*>(p);
}
extern "C" void win64_region_add(uint64_t p,size_t n,int writable) { std::lock_guard lock(region_mutex); regions.push_back({p,n,0,writable!=0}); }
extern "C" void win64_region_remove(uint64_t p) {
    std::lock_guard lock(region_mutex);
    for (auto it=regions.begin();it!=regions.end();++it) if (it->p==p) { regions.erase(it); return; }
    abort();
}
extern "C" uint64_t win64_alloc(size_t n,int zero) {
    size_t page=size_t(sysconf(_SC_PAGESIZE)), bytes=(n+page-1)&~(page-1);
    if (!bytes) bytes=page;
    void* p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (p==MAP_FAILED) abort();
    if (zero) memset(p,0,n);
    uint64_t address=reinterpret_cast<uintptr_t>(p);
    { std::lock_guard lock(region_mutex); regions.push_back({address,n,bytes,true}); } return address;
}
extern "C" void win64_free(uint64_t p) {
    std::lock_guard lock(region_mutex);
    for (auto it=regions.begin();it!=regions.end();++it) if (it->p==p && it->allocation) {
        munmap(reinterpret_cast<void*>(p),it->allocation); regions.erase(it); return;
    }
    abort();
}
extern "C" const char* win64_string_a(uint64_t p) {
    for (size_t i=0;i<65536;++i) if (!*static_cast<const char*>(win64_pointer(p+i,1,0))) return reinterpret_cast<const char*>(p);
    abort();
}
extern "C" uint64_t win64_argument(CPU* c,unsigned i) {
    if (i<4) return i==0 ? c->rcx : i==1 ? c->rdx : i==2 ? c->r8 : c->r9;
    uint64_t value; memcpy(&value,win64_pointer(c->rsp+40+(i-4)*8,8,0),8); return value;
}
static void require(bool valid,const char* message) { if (!valid) throw std::runtime_error(message); }
template<class T> uint64_t put(const T& value) { uint64_t p=win64_alloc(sizeof(T),1); memcpy(win64_pointer(p,sizeof(T),1),&value,sizeof(T)); return p; }
static uint64_t read64(uint64_t p) { uint64_t v; memcpy(&v,win64_pointer(p,8,0),8); return v; }
static void write64(uint64_t p,uint64_t v) { memcpy(win64_pointer(p,8,1),&v,8); }
static thread_local CPU cpu;
static thread_local uint64_t stack;
static std::atomic<unsigned> calls{0};
static void frame(const std::vector<uint64_t>& args) {
    cpu={}; cpu.rsp=stack+256;
    uint64_t* registers[]={&cpu.rcx,&cpu.rdx,&cpu.r8,&cpu.r9};
    for (size_t i=0;i<args.size();++i) {
        if (i<4) *registers[i]=args[i]; else write64(cpu.rsp+40+(i-4)*8,args[i]);
    }
}
static uint64_t call(uint64_t object,unsigned slot,std::initializer_list<uint64_t> values={}) {
    std::vector<uint64_t> args{object}; args.insert(args.end(),values.begin(),values.end()); frame(args);
    CPU before=cpu; uint64_t target=read64(read64(object)+slot*8);
    require(win64_d3d11_dispatch(&cpu,target)==1,"COM method not handled");
    before.rax=cpu.rax;
    require(!memcmp(&cpu,&before,sizeof(cpu)),"bridge changed CPU except ABI result"); ++calls; return cpu.rax;
}
static void hr(uint64_t result,const char* step) { require(int32_t(result)>=0,step); }
static uint64_t output() { return win64_alloc(8,1); }
static thread_local uint32_t last_error;
extern "C" uint32_t win64_get_error() { return last_error; }
extern "C" void win64_set_error(uint32_t value) { last_error=value; }
static char photo_path[128];
static bool photo_exported=false,photo_open_denied=false,photo_export_denied=false;
extern "C" int win64_kernel_stream_open(uint64_t path,int wide,int writing,int* fd,void** token) {
    require(wide==1&&writing==1,"photo must use the secure wide writing contract");
    const char16_t expected[]=u"photo.png";
    require(!memcmp(win64_pointer(path,sizeof(expected),0),expected,sizeof(expected)),"photo filename ABI");
    if(photo_open_denied)return 13;
    strcpy(photo_path,"/data/local/tmp/frostpunk-photo-check-XXXXXX");
    *fd=mkstemp(photo_path);*token=nullptr;return *fd<0?13:0;
}
extern "C" void win64_kernel_stream_close(void*) {}
extern "C" int frostpunk_android_photo_saved(const char* path) {
    require(!strcmp(path,photo_path),"photo callback must report the resolved file");photo_exported=true;return !photo_export_denied;
}

static unsigned completed_discards=0,completed_replays=0;
static void check_buffer_renames(uint64_t device,uint64_t context) {
    D3D11_BUFFER_DESC dynamic={};dynamic.ByteWidth=64;dynamic.Usage=D3D11_USAGE_DYNAMIC;
    dynamic.BindFlags=D3D11_BIND_CONSTANT_BUFFER;dynamic.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    D3D11_BUFFER_DESC staging=dynamic;staging.Usage=D3D11_USAGE_STAGING;
    staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    uint64_t buffer_out=output(),stages[8],mapped_out=put(D3D11_MAPPED_SUBRESOURCE());
    hr(call(device,3,{put(dynamic),0,buffer_out}),"rename dynamic buffer");
    for(auto& out:stages){out=output();hr(call(device,3,{put(staging),0,out}),"rename staging buffer");}
    auto write=[&](uint64_t ctx,uint64_t buffer,unsigned seed,bool pattern){
        hr(call(ctx,14,{buffer,0,D3D11_MAP_WRITE_DISCARD,0,mapped_out}),"rename WRITE_DISCARD");
        auto mapped=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(mapped_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
        for(unsigned i=0;i<64;++i)static_cast<unsigned char*>(mapped.pData)[i]=pattern?(seed*13+i*17)&255:seed;
        call(ctx,15,{buffer,0});
    };
    auto read=[&](uint64_t buffer,unsigned seed,bool pattern){
        hr(call(context,14,{buffer,0,D3D11_MAP_READ,0,mapped_out}),"rename staging READ");
        auto mapped=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(mapped_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
        for(unsigned i=0;i<64;++i){
            unsigned actual=static_cast<unsigned char*>(mapped.pData)[i],expected=pattern?((seed*13+i*17)&255):seed;
            if(actual!=expected)fprintf(stderr,"BUFFER_RENAME_MISMATCH calls=%u seed=%u byte=%u expected=%u actual=%u\n",calls.load(),seed,i,expected,actual);
            require(actual==expected,"renamed buffer slice lost or reused before GPU completion");
        }
        call(context,15,{buffer,0});
    };
    for(unsigned batch=0;batch<64;++batch){
        for(unsigned i=0;i<8;++i){
            write(context,read64(buffer_out),batch*8+i,true);
            call(context,47,{read64(stages[i]),read64(buffer_out)});
        }
        if(batch==63)call(read64(buffer_out),2);
        for(unsigned i=0;i<8;++i){read(read64(stages[i]),batch*8+i,true);++completed_discards;}
    }
    hr(call(device,3,{put(dynamic),0,buffer_out}),"deferred rename dynamic buffer");
    uint64_t deferred_out=output(),list_out=output();
    hr(call(device,27,{0,deferred_out}),"CreateDeferredContext");
    uint64_t deferred=read64(deferred_out),buffer=read64(buffer_out);
    write(deferred,buffer,0x6a,false);
    call(deferred,47,{read64(stages[0]),buffer});
    hr(call(deferred,114,{0,list_out}),"FinishCommandList");
    for(unsigned repeat=0;repeat<2;++repeat){
        write(context,buffer,0x95,false);call(context,47,{read64(stages[0]),buffer});
        read(read64(stages[0]),0x95,false);
        call(context,58,{read64(list_out),0});
        read(read64(stages[0]),0x6a,false);
        ++completed_replays;
    }
    call(read64(list_out),2);call(deferred,2);call(buffer,2);
    for(auto out:stages)call(read64(out),2);
}

#include "buffer_map_bridge.inc"

int main(int argc,char** argv) {
    bool success=false; std::string error; unsigned matched=0; bool identity=false,utf16=false,map_removed=false,float_abi=false;
    unsigned char pixels[64]={};
    bool concurrent_device=false,lifetime_retention=false,resource_maps=false,buffer_renames=false;
    bool dxgi_device2=false,latency_identity=false,latency_canary=false,factory2_identity=false,factory2_null_contract=false;
    uint32_t invalid_latency=0,null_latency_output=0;
    try {
        require(argc==3||argc==4,"usage: nativeD3D11_bridge runtimeDir driverDir/ [bookkeeping trap mode]");
        char detail[512]={}; auto gipa=frostpunk_gpu_open(argv[1],argv[2],detail,sizeof(detail)); require(gipa,detail);
        win64_d3d11_configure(gipa,nullptr);
        stack=win64_alloc(8192,1);
        D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,D3D_FEATURE_LEVEL_9_2,D3D_FEATURE_LEVEL_9_1};
        uint64_t level_in=put(levels),level_out=put(D3D_FEATURE_LEVEL());
        uint64_t device_out=output(),context_out=output();
        frame({0,D3D_DRIVER_TYPE_HARDWARE,0,D3D11_CREATE_DEVICE_BGRA_SUPPORT,level_in,6,D3D11_SDK_VERSION,device_out,level_out,context_out});
        uint64_t before=cpu.rsp;
        require(win64_d3d11_import(&cpu,"d3d11.dll","D3D11CreateDevice")==1,"CreateDevice import not handled");
        require(cpu.rsp==before,"import popped/changed guest RSP"); ++calls;
        hr(cpu.rax,"CreateDevice failed"); require(*static_cast<D3D_FEATURE_LEVEL*>(win64_pointer(level_out,4,0))==level,"FL11_0 mismatch");
        uint64_t device=read64(device_out),context=read64(context_out);
        if(argc==4)bookkeeping_trap(device,context,argv[3]);
        bookkeeping_maps(device,context);
        for (unsigned dimension=0;dimension<4;++dimension) {
            uint64_t resource_out=output();
            if (!dimension) {
                D3D11_BUFFER_DESC d={};d.ByteWidth=64;d.Usage=D3D11_USAGE_STAGING;
                d.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
                hr(call(device,3,{put(d),0,resource_out}),"resource Map buffer");
            } else if (dimension==1) {
                D3D11_TEXTURE1D_DESC d={};d.Width=8;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R32_UINT;
                d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
                hr(call(device,4,{put(d),0,resource_out}),"resource Map texture1D");
            } else if (dimension==2) {
                D3D11_TEXTURE2D_DESC d={};d.Width=8;d.Height=4;d.MipLevels=1;d.ArraySize=1;d.Format=DXGI_FORMAT_R32_UINT;
                d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
                hr(call(device,5,{put(d),0,resource_out}),"resource Map texture2D");
            } else {
                D3D11_TEXTURE3D_DESC d={};d.Width=4;d.Height=4;d.Depth=2;d.MipLevels=1;d.Format=DXGI_FORMAT_R32_UINT;
                d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
                hr(call(device,6,{put(d),0,resource_out}),"resource Map texture3D");
            }
            uint64_t typed=read64(resource_out);
            for (unsigned variant=0;variant<3;++variant) {
                uint64_t resource=typed,base_out=output();
                if (variant) {
                    hr(call(typed,0,{put(variant==1?__uuidof(ID3D11Resource):__uuidof(IUnknown)),base_out}),"resource Map base QI");
                    resource=read64(base_out);
                }
                uint64_t mapped_out=put(D3D11_MAPPED_SUBRESOURCE());
                hr(call(context,14,{resource,0,D3D11_MAP_WRITE,0,mapped_out}),"resource Map write");
                auto mapped=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(mapped_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
                unsigned char pattern[16];memset(pattern,0x31+dimension*3+variant,sizeof(pattern));
                memcpy(mapped.pData,pattern,sizeof(pattern));call(context,15,{resource,0});
                hr(call(context,14,{resource,0,D3D11_MAP_READ,0,mapped_out}),"resource Map read");
                mapped=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(mapped_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
                require(!memcmp(mapped.pData,pattern,sizeof(pattern)),"typed/base resource Map data mismatch");
                call(context,15,{resource,0});
                if (variant)call(resource,2);
            }
            call(typed,2);
        }
        resource_maps=true;
        check_buffer_renames(device,context);buffer_renames=true;
        uint64_t iid_unknown=put(__uuidof(IUnknown)),u1=output(),u2=output(),dev2=output();
        hr(call(device,0,{iid_unknown,u1}),"device QI IUnknown");
        call(context,3,{dev2}); require(read64(dev2)==device,"GetDevice proxy identity");
        hr(call(read64(dev2),0,{iid_unknown,u2}),"GetDevice QI IUnknown");
        identity=read64(u1)==read64(u2); require(identity,"canonical IUnknown identity mismatch");
        uint64_t canonical_unknown=read64(u1);
        call(read64(u1),2); call(read64(u2),2); call(read64(dev2),2);
        uint64_t dxgi2_out=output(),dxgi2_again=output(),dxgi1_out=output();
        uint64_t iid_dxgi2=put(__uuidof(IDXGIDevice2)),iid_dxgi1=put(__uuidof(IDXGIDevice1));
        hr(call(device,0,{iid_dxgi2,dxgi2_out}),"original game QI IDXGIDevice2");
        uint64_t dxgi2=read64(dxgi2_out);
        hr(call(dxgi2,0,{iid_dxgi2,dxgi2_again}),"repeat QI IDXGIDevice2");
        require(read64(dxgi2_again)==dxgi2,"canonical IDXGIDevice2 proxy identity");
        hr(call(dxgi2,0,{iid_dxgi1,dxgi1_out}),"Device2 QI IDXGIDevice1");
        uint64_t dxgi1=read64(dxgi1_out);
        hr(call(dxgi2,0,{iid_unknown,u1}),"Device2 canonical IUnknown");
        hr(call(dxgi1,0,{iid_unknown,u2}),"Device1 canonical IUnknown");
        latency_identity=read64(u1)==read64(u2)&&read64(u1)==canonical_unknown;
        require(latency_identity,"DXGI Device1/2 IUnknown identity");
        call(read64(u1),2);call(read64(u2),2);call(read64(dxgi2_again),2);
        uint64_t latency_out=win64_alloc(12,0);memset(win64_pointer(latency_out,12,1),0xA5,12);
        auto latency=[&](uint64_t object){hr(call(object,13,{latency_out}),"GetMaximumFrameLatency");
            uint32_t value;memcpy(&value,win64_pointer(latency_out,4,0),4);
            const auto *tail=static_cast<const unsigned char*>(win64_pointer(latency_out+4,8,0));
            for(unsigned i=0;i<8;++i)require(tail[i]==0xA5,"latency UINT output overrun");return value;};
        hr(call(dxgi2,12,{1}),"original game SetMaximumFrameLatency(1)");require(latency(dxgi2)==1,"Device2 latency1 readback");
        hr(call(dxgi1,12,{2}),"Device1 SetMaximumFrameLatency(2)");require(latency(dxgi2)==2,"inherited native shared latency state");
        hr(call(dxgi2,12,{0}),"zero selects native default latency");require(latency(dxgi1)==3,"DXVK native default latency3");
        hr(call(dxgi2,12,{16}),"maximum native valid latency16");require(latency(dxgi1)==16,"native latency16 readback");
        hr(call(dxgi2,12,{UINT64_C(0xA5A5A5A500000001)}),"Win64 low32 UINT latency");require(latency(dxgi1)==1,"Win64 low32 latency readback");
        invalid_latency=uint32_t(call(dxgi2,12,{17}));null_latency_output=uint32_t(call(dxgi1,13,{0}));
        require(invalid_latency==uint32_t(DXGI_ERROR_INVALID_CALL)&&null_latency_output==uint32_t(DXGI_ERROR_INVALID_CALL),"native latency invalid HRESULTs");
        require(latency(dxgi2)==1,"failed native setter changed latency");
        latency_canary=dxgi_device2=true;call(dxgi1,2);call(dxgi2,2);
        std::atomic<unsigned> failures{0};
        std::vector<std::thread> workers;
        for (unsigned worker=0;worker<4;++worker) workers.emplace_back([&,worker] {
            try {
                stack=win64_alloc(8192,1);
                GUID guid={0xdead1000u+worker,0x1234,0x5678,{1,2,3,4,5,6,7,8}};
                uint64_t guid_address=put(guid);
                for (unsigned iteration=0;iteration<16;++iteration) {
                    uint64_t unknown_out=output();
                    hr(call(device,0,{iid_unknown,unknown_out}),"concurrent QI IUnknown");
                    uint64_t unknown=read64(unknown_out); require(unknown==canonical_unknown,"concurrent canonical IUnknown identity");
                    call(unknown,1); call(unknown,2); call(unknown,2);
                    D3D11_BUFFER_DESC bd={}; bd.ByteWidth=16; bd.Usage=D3D11_USAGE_IMMUTABLE; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                    uint32_t data[4]={worker,iteration,0x12345678,0x87654321};
                    D3D11_SUBRESOURCE_DATA initial={reinterpret_cast<void*>(put(data)),16,16};
                    uint64_t buffer_out=output(); hr(call(device,3,{put(bd),put(initial),buffer_out}),"concurrent CreateBuffer");
                    uint64_t buffer=read64(buffer_out),desc_out=put(D3D11_BUFFER_DESC());
                    call(buffer,10,{desc_out}); require(static_cast<D3D11_BUFFER_DESC*>(win64_pointer(desc_out,sizeof(bd),0))->ByteWidth==16,"concurrent Buffer GetDesc");
                    call(buffer,2);
                    uint32_t marker=(worker<<16)|iteration; uint64_t marker_in=put(marker),marker_out=put(uint32_t()),size=put(UINT(4));
                    hr(call(device,35,{guid_address,4,marker_in}),"concurrent SetPrivateData");
                    hr(call(device,34,{guid_address,size,marker_out}),"concurrent GetPrivateData");
                    require(*static_cast<uint32_t*>(win64_pointer(marker_out,4,0))==marker,"concurrent private data mismatch");
                }
            } catch (...) { ++failures; }
        });
        for (auto& worker:workers) worker.join();
        concurrent_device=failures.load()==0; require(concurrent_device,"concurrent device COM fixture failed");
        // The native context owns this buffer after every guest ref drops.
        // A later Get must revive the same proxy/IUnknown, then actual native
        // destruction must remove both guest allocations.
        D3D11_BUFFER_DESC retained_desc={}; retained_desc.ByteWidth=16; retained_desc.Usage=D3D11_USAGE_DEFAULT;
        retained_desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        uint64_t retained_out=output(); hr(call(device,3,{put(retained_desc),0,retained_out}),"retained CreateBuffer");
        uint64_t retained=read64(retained_out),retained_unknown_out=output();
        hr(call(retained,0,{iid_unknown,retained_unknown_out}),"retained QI");
        uint64_t retained_unknown=read64(retained_unknown_out);
        call(context,7,{0,1,retained_out}); call(retained_unknown,2); call(retained,2);
        uint64_t restored_out=output(); call(context,72,{0,1,restored_out});
        require(read64(restored_out)==retained,"native-held buffer proxy identity changed");
        hr(call(retained,0,{iid_unknown,retained_unknown_out}),"restored QI");
        require(read64(retained_unknown_out)==retained_unknown,"native-held canonical IUnknown changed");
        call(retained_unknown,2); call(retained,2); call(context,109); call(context,110);
        // DXVK may retain command-stream private refs until Flush is consumed.
        for (unsigned wait=0;wait<1000 && accessible(retained,8,false);++wait) usleep(1000);
        lifetime_retention=!accessible(retained,8,false) && !accessible(retained_unknown,8,false);
        require(lifetime_retention,"actual native destruction left guest COM proxies allocated");
        uint64_t dxgi_out=output(),adapter_out=output();
        hr(call(device,0,{put(__uuidof(IDXGIDevice)),dxgi_out}),"QI IDXGIDevice");
        hr(call(read64(dxgi_out),7,{adapter_out}),"GetAdapter");
        uint64_t factory2_out=output(),factory1_out=output(),factory_unknown1=output(),factory_unknown2=output();
        uint64_t iid_factory2=put(__uuidof(IDXGIFactory2)),iid_factory1=put(__uuidof(IDXGIFactory1));
        hr(call(read64(adapter_out),6,{iid_factory2,factory2_out}),"original screen GetParent IDXGIFactory2");
        uint64_t factory2=read64(factory2_out);
        hr(call(factory2,0,{iid_factory1,factory1_out}),"Factory2 inherited Factory1");
        hr(call(factory2,0,{iid_unknown,factory_unknown1}),"Factory2 canonical IUnknown");
        hr(call(read64(factory1_out),0,{iid_unknown,factory_unknown2}),"Factory1 canonical IUnknown");
        require(read64(factory_unknown1)==read64(factory_unknown2),"Factory2/base canonical IUnknown mismatch");
        factory2_identity=true;
        DXGI_SWAP_CHAIN_DESC1 window_desc={};window_desc.Width=960;window_desc.Height=540;window_desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        window_desc.SampleDesc.Count=1;window_desc.BufferUsage=DXGI_USAGE_SHADER_INPUT|DXGI_USAGE_RENDER_TARGET_OUTPUT;window_desc.BufferCount=2;
        window_desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;window_desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        uint64_t window_desc_in=put(window_desc),null_chain=win64_alloc(24,0);memset(win64_pointer(null_chain,24,1),0xa5,24);
        uint32_t null_hwnd=uint32_t(call(factory2,15,{device,0,window_desc_in,0,0,null_chain+8}));
        require(null_hwnd==uint32_t(DXGI_ERROR_INVALID_CALL)&&!read64(null_chain+8),"Factory2 NULL HWND output/HRESULT");
        require(read64(null_chain)==UINT64_C(0xa5a5a5a5a5a5a5a5)&&read64(null_chain+16)==UINT64_C(0xa5a5a5a5a5a5a5a5),"Factory2 pointer output overrun");
        require(uint32_t(call(factory2,15,{device,0,window_desc_in,0,0,0}))==uint32_t(DXGI_ERROR_INVALID_CALL),"Factory2 NULL required output");
        require(uint32_t(call(factory2,15,{0,1,window_desc_in,0,0,null_chain+8}))==uint32_t(DXGI_ERROR_INVALID_CALL)&&!read64(null_chain+8),"Factory2 NULL required device");
        require(uint32_t(call(factory2,15,{device,1,0,0,0,null_chain+8}))==uint32_t(DXGI_ERROR_INVALID_CALL)&&!read64(null_chain+8),"Factory2 NULL required description");
        require(!memcmp(win64_pointer(window_desc_in,sizeof(window_desc),0),&window_desc,sizeof(window_desc)),"Factory2 guest description mutated");
        factory2_null_contract=true;
        for(uint64_t value:{read64(factory_unknown1),read64(factory_unknown2),read64(factory1_out),factory2})call(value,2);
        for(uint64_t memory:{factory2_out,factory1_out,factory_unknown1,factory_unknown2,iid_factory2,iid_factory1,window_desc_in,null_chain})win64_free(memory);
        uint64_t description=win64_alloc(320,0); memset(win64_pointer(description,320,1),0xa5,320);
        hr(call(read64(adapter_out),8,{description}),"GetDesc UTF16");
        auto* bytes=static_cast<unsigned char*>(win64_pointer(description,320,0));
        utf16=bytes[0]!=0 && bytes[1]==0;
        unsigned end=0;for(;end<256&&bytes[end];end+=2)utf16=utf16&&bytes[end+1]==0;
        utf16=utf16&&end<256&&bytes[end+1]==0;
        for (unsigned i=304;i<320;++i) utf16=utf16 && bytes[i]==0xa5;
        require(utf16,"GetDesc Win64 UTF16 size/canary mismatch");
        uint64_t support=put(UINT());uint32_t sample_format=DXGI_FORMAT_BC3_UNORM;
        if(int32_t(call(device,29,{sample_format,support}))<0||!(*static_cast<UINT*>(win64_pointer(support,4,0))&D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
            sample_format=DXGI_FORMAT_R8G8B8A8_UNORM;
        hr(call(device,29,{sample_format,support}),"CheckFormatSupport sampled texture");
        require((*static_cast<UINT*>(win64_pointer(support,4,0))&D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)!=0,"texture not sampleable");
        win64_region_add(reinterpret_cast<uintptr_t>(native_vs),sizeof(native_vs),0);
        win64_region_add(reinterpret_cast<uintptr_t>(native_ps),sizeof(native_ps),0);
        uint64_t vs_out=output(),ps_out=output();
        hr(call(device,12,{reinterpret_cast<uintptr_t>(native_vs),sizeof(native_vs),0,vs_out}),"CreateVS DXBC");
        hr(call(device,15,{reinterpret_cast<uintptr_t>(native_ps),sizeof(native_ps),0,ps_out}),"CreatePS DXBC");
        const unsigned char bc3[16]={255,0,0,0,0,0,0,0,0,248,224,7,0,0,0,0};
        unsigned char red[64];for(unsigned i=0;i<64;i+=4){red[i]=255;red[i+1]=red[i+2]=0;red[i+3]=255;}
        uint64_t bc3_data=sample_format==DXGI_FORMAT_BC3_UNORM?put(bc3):put(red);
        D3D11_SUBRESOURCE_DATA init={reinterpret_cast<void*>(bc3_data),16,sample_format==DXGI_FORMAT_BC3_UNORM?16u:64u};
        D3D11_TEXTURE2D_DESC td={}; td.Width=td.Height=4; td.MipLevels=td.ArraySize=1; td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT(sample_format); td.Usage=D3D11_USAGE_IMMUTABLE; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags=0;
        uint64_t texture_out=output(),srv_out=output(),target_out=output(),rtv_out=output(),staging_out=output();
        hr(call(device,5,{put(td),put(init),texture_out}),"CreateTexture2D BC3");
        hr(call(device,7,{read64(texture_out),0,srv_out}),"CreateSRV");
        // An actual depth clear/readback checks Win64 float argument XMM3.
        D3D11_TEXTURE2D_DESC dd=td; dd.Format=DXGI_FORMAT_D32_FLOAT; dd.Usage=D3D11_USAGE_DEFAULT;
        dd.BindFlags=D3D11_BIND_DEPTH_STENCIL; dd.MiscFlags=0;
        uint64_t depth_out=output(),dsv_out=output(),depth_staging_out=output();
        hr(call(device,5,{put(dd),0,depth_out}),"Create depth texture");
        hr(call(device,10,{read64(depth_out),0,dsv_out}),"Create DSV");
        frame({context,read64(dsv_out),D3D11_CLEAR_DEPTH,0xdeadbeef,0}); cpu.xmm[3].f32[0]=0.375f;
        before=cpu.rsp; require(win64_d3d11_dispatch(&cpu,read64(read64(context)+53*8))==1,"ClearDepthStencilView dispatch");
        require(before==cpu.rsp,"float call changed RSP"); ++calls;
        dd.Usage=D3D11_USAGE_STAGING; dd.BindFlags=0; dd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(call(device,5,{put(dd),0,depth_staging_out}),"Create depth staging");
        call(context,47,{read64(depth_staging_out),read64(depth_out)});
        uint64_t depth_map_out=put(D3D11_MAPPED_SUBRESOURCE());
        hr(call(context,14,{read64(depth_staging_out),0,D3D11_MAP_READ,0,depth_map_out}),"Map depth staging");
        auto depth_map=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(depth_map_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
        float_abi=true;
        for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
            float v; memcpy(&v,static_cast<const unsigned char*>(depth_map.pData)+y*depth_map.RowPitch+x*4,4);
            float_abi=float_abi && v==0.375f;
        }
        call(context,15,{read64(depth_staging_out),0});
        require(float_abi,"Win64 XMM3 float depth clear/readback mismatch");
        call(read64(dsv_out),2); call(read64(depth_staging_out),2);
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage=D3D11_USAGE_DEFAULT; td.BindFlags=D3D11_BIND_RENDER_TARGET; td.MiscFlags=0;
        hr(call(device,5,{put(td),0,target_out}),"CreateRT texture");
        hr(call(device,9,{read64(target_out),0,rtv_out}),"CreateRTV");
        td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        hr(call(device,5,{put(td),0,staging_out}),"Create staging");
        D3D11_SAMPLER_DESC sd={}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD=D3D11_FLOAT32_MAX; sd.MaxAnisotropy=1; sd.ComparisonFunc=D3D11_COMPARISON_NEVER;
        uint64_t sampler_out=output(); hr(call(device,23,{put(sd),sampler_out}),"Create sampler");
        D3D11_RASTERIZER_DESC rd={}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=true;
        uint64_t raster_out=output(); hr(call(device,22,{put(rd),raster_out}),"Create rasterizer");
        D3D11_VIEWPORT vp={0,0,4,4,0,1}; const float clear[4]={0,0,1,0};
        call(context,50,{read64(rtv_out),put(clear)});
        call(context,33,{1,rtv_out,0}); call(context,44,{1,put(vp)}); call(context,43,{read64(raster_out)});
        call(context,24,{D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST}); call(context,11,{read64(vs_out),0,0});
        call(context,9,{read64(ps_out),0,0}); call(context,8,{0,1,srv_out}); call(context,10,{0,1,sampler_out});
        call(context,13,{3,0}); call(context,47,{read64(staging_out),read64(target_out)});
        uint64_t mapped_out=put(D3D11_MAPPED_SUBRESOURCE());
        hr(call(context,14,{read64(staging_out),0,D3D11_MAP_READ,0,mapped_out}),"Map staging");
        auto mapped=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(mapped_out,sizeof(D3D11_MAPPED_SUBRESOURCE),0));
        uint64_t address=reinterpret_cast<uintptr_t>(mapped.pData);
        require(mapped.RowPitch>=16,"Map RowPitch");
        for (unsigned y=0;y<4;++y) {
            memcpy(pixels+16*y,reinterpret_cast<const void*>(address+y*mapped.RowPitch),16);
            for (unsigned x=0;x<4;++x) { auto* p=pixels+(y*4+x)*4; if(p[0]==255&&p[1]==0&&p[2]==0&&p[3]==255) ++matched; }
        }
        call(context,15,{read64(staging_out),0}); map_removed=true;
        for (unsigned repeat=0;repeat<64;++repeat) {
            call(context,13,{3,0});call(context,47,{read64(staging_out),read64(target_out)});
            hr(call(context,14,{read64(staging_out),0,D3D11_MAP_READ,0,mapped_out}),"CS completion readback");
            mapped=*static_cast<D3D11_MAPPED_SUBRESOURCE*>(win64_pointer(mapped_out,sizeof(mapped),0));
            for (unsigned y=0;y<4;++y)
                require(!memcmp(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,pixels+y*16,16),"CS completion published before draw/copy finished");
            call(context,15,{read64(staging_out),0});
        }
        const char16_t filename[]=u"photo.png";uint64_t photo_name=put(filename);
        auto save_photo=[&](uint64_t texture,uint32_t expected) {
            frame({context,texture,0,photo_name});CPU preserved=cpu;last_error=0x12345678;
            win64_d3d11_save_photo(&cpu);preserved.rax=cpu.rax;
            require(uint32_t(cpu.rax)==expected,"photo HRESULT mismatch");
            require(!memcmp(&cpu,&preserved,sizeof(cpu))&&last_error==0x12345678,"photo changed CPU or guest error state");
        };
        save_photo(read64(target_out),0);
        require(photo_exported,"successful photo did not notify the gallery");
        FILE* photo=fopen(photo_path,"rb");unsigned char header[24]={};
        require(photo&&fread(header,1,24,photo)==24,"photo PNG missing");fclose(photo);unlink(photo_path);
        const unsigned char signature[8]={137,80,78,71,13,10,26,10};
        require(!memcmp(header,signature,8)&&header[19]==4&&header[23]==4,"photo is not a 4x4 PNG");
        photo_export_denied=true;save_photo(read64(target_out),0x80004005u);
        require(photo_exported,"gallery failure did not attempt an export");
        photo=fopen(photo_path,"rb");require(photo!=nullptr,"gallery failure lost the original PNG");fclose(photo);unlink(photo_path);
        photo_export_denied=false;photo_exported=false;photo_open_denied=true;
        save_photo(read64(target_out),0x8007000du);require(!photo_exported,"failed photo notified the gallery");
        photo_open_denied=false;save_photo(read64(depth_out),0x80070032u);call(read64(depth_out),2);
        hr(call(device,39),"GetDeviceRemovedReason"); call(context,109); call(context,110);
        for (uint64_t out:{raster_out,sampler_out,staging_out,rtv_out,target_out,srv_out,texture_out,ps_out,vs_out,adapter_out,dxgi_out}) call(read64(out),2);
        call(context,2); call(device,2);
        require(matched==16,"Win64 COM BC3 draw/readback pixel mismatch"); success=true;
    } catch (const std::exception& e) { error=e.what(); }
    printf("{\"success\":%s,\"synthetic_win64_cpu_frames\":true,\"native_aarch64\":true,\"com_calls\":%u,\"concurrent_device_threads\":4,\"concurrent_device_iterations\":64,\"concurrent_device_passed\":%s,\"native_lifetime_retention_passed\":%s,\"canonical_iunknown\":%s,\"utf16_desc_canary\":%s,\"xmm_float_abi\":%s,\"staging_map_unmap\":%s,\"matched_pixels\":%u,\"pixels_rgba8\":[",
        success?"true":"false",calls.load(),concurrent_device?"true":"false",lifetime_retention?"true":"false",identity?"true":"false",utf16?"true":"false",float_abi?"true":"false",map_removed?"true":"false",matched);
    for(unsigned i=0;i<64;++i) printf("%s%u",i?",":"",pixels[i]);
    printf("],\"typed_base_unknown_resource_maps\":%s,\"dxgi_device2_latency_passed\":%s,\"dxgi_device12_identity\":%s,\"latency_uint32_canary\":%s,\"latency17_hresult\":%u,\"latency_NULL_output_hresult\":%u,\"factory2_inherited_identity\":%s,\"factory2_null_contract\":%s,\"full_CPU_except_rax_preserved\":true,\"error\":\"",resource_maps?"true":"false",dxgi_device2?"true":"false",latency_identity?"true":"false",latency_canary?"true":"false",invalid_latency,null_latency_output,factory2_identity?"true":"false",factory2_null_contract?"true":"false"); for(char ch:error) { if(ch=='\"'||ch=='\\') putchar('\\'); if(ch>=32)putchar(ch); }
    printf("\",\"buffer_map_bookkeeping_checks\":%u,\"buffer_rename_lifetime\":%s,\"buffer_discard_iterations\":%u,\"deferred_replays\":%u,\"game_rendered\":false,\"fps_measured\":false}\n",buffer_map_checks,buffer_renames?"true":"false",completed_discards,completed_replays); return success?0:1;
}
