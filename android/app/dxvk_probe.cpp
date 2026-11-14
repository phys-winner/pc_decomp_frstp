/* Offline-lifted synthetic Win64 CPU -> real COM bridge -> DXVK -> Android WSI.
 * Only the small fixture uses this pointer/allocator implementation. No game
 * entry or original executable is included or executed. */
#include "window_probe.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <initializer_list>
extern "C" {
#include "../dxvk/bridge.h"
}
#include "../host/win64.h"
extern "C" {
void guest_create_lifted(CPU *);
void guest_draw_lifted(CPU *);
}

struct Region {uint64_t p;size_t size,allocation;bool writable;};
static std::vector<Region> regions;
static std::recursive_mutex region_mutex;
static bool accessible(uint64_t p,size_t size,bool write) {
    std::lock_guard lock(region_mutex);
    for(auto r:regions)if(p>=r.p&&p-r.p<=r.size&&size<=r.size-(p-r.p)&&(!write||r.writable))return true;
    return !size;
}
extern "C" void *win64_pointer(uint64_t p,size_t size,int write) {
    if(!accessible(p,size,write!=0)){fprintf(stderr,"APP_DXVK_FAIL invalid_guest_range %llx+%zu\n",(unsigned long long)p,size);abort();}
    return reinterpret_cast<void *>(p);
}
extern "C" void win64_region_add(uint64_t p,size_t size,int write){std::lock_guard lock(region_mutex);regions.push_back({p,size,0,write!=0});}
extern "C" void win64_region_remove(uint64_t p){std::lock_guard lock(region_mutex);for(auto i=regions.begin();i!=regions.end();++i)if(i->p==p){regions.erase(i);return;}abort();}
extern "C" uint64_t win64_alloc(size_t size,int zero) {
    size_t page=size_t(sysconf(_SC_PAGESIZE)),bytes=(size+page-1)&~(page-1);if(!bytes)bytes=page;
    void *p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)abort();
    if(zero)memset(p,0,size);uint64_t address=reinterpret_cast<uintptr_t>(p);
    std::lock_guard lock(region_mutex);regions.push_back({address,size,bytes,true});return address;
}
extern "C" void win64_free(uint64_t p){std::lock_guard lock(region_mutex);for(auto i=regions.begin();i!=regions.end();++i)if(i->p==p&&i->allocation){munmap(reinterpret_cast<void *>(p),i->allocation);regions.erase(i);return;}abort();}
extern "C" const char *win64_string_a(uint64_t p){for(size_t i=0;i<65536;++i)if(!*static_cast<const char *>(win64_pointer(p+i,1,0)))return reinterpret_cast<const char *>(p);abort();}
extern "C" uint64_t win64_argument(CPU *c,unsigned index){if(index<4)return index==0?c->rcx:index==1?c->rdx:index==2?c->r8:c->r9;uint64_t result;memcpy(&result,win64_pointer(c->rsp+40+(index-4)*8,8,0),8);return result;}
static uint64_t read64(uint64_t p){uint64_t value;memcpy(&value,win64_pointer(p,8,0),8);return value;}
static void write64(uint64_t p,uint64_t v){memcpy(win64_pointer(p,8,1),&v,8);}
template<class T>static uint64_t put(const T &v){uint64_t p=win64_alloc(sizeof(v),1);memcpy(win64_pointer(p,sizeof(v),1),&v,sizeof(v));return p;}
static void require(bool yes,const char *message){if(!yes)throw std::runtime_error(message);}
static constexpr uint64_t IMPORT=0xE5E30000,RETURN=0xE5E3FFF8,CANARY=0xA5C35A7E01234567;
static std::atomic<unsigned> calls{0};
static std::atomic<uint64_t> registered_hwnd{0},registered_generation{0};
static std::atomic<ANativeWindow *> registered_native{nullptr};
static std::atomic<unsigned> window_acquires{0},window_releases{0},foreign_rejections{0},lease_fixture_chains{0};
static std::atomic<bool> strict_fixture_passed{false};
static std::atomic<bool> factory2_lease_passed{false};
static std::atomic<unsigned> factory2_synthetic_calls{0};
static void *fixture_acquire(uint64_t hwnd,uint64_t *generation){
    if(hwnd!=registered_hwnd.load()){++foreign_rejections;return nullptr;}
    auto *native=registered_native.load();if(!native)return nullptr;
    ANativeWindow_acquire(native);if(generation)*generation=registered_generation.load();++window_acquires;return native;
}
static void fixture_release(void *native){++window_releases;ANativeWindow_release(static_cast<ANativeWindow *>(native));}
extern "C" void dispatch(CPU *c,uint64_t target) {
    uint64_t before=c->rsp;
    int handled=target==IMPORT?win64_d3d11_import(c,"d3d11.dll","D3D11CreateDeviceAndSwapChain"):win64_d3d11_dispatch(c,target);
    require(handled==1,"uncatalogued fixture target");require(c->rsp==before,"bridge changed guest RSP");
    c->rsp+=8;++calls;
}
extern "C" void dispatch_jmp(CPU *,uint64_t){fprintf(stderr,"APP_DXVK_FAIL unrepresented tail dispatch\n");abort();}

/* Forwarding observation only: every API still executes the pinned ICD. */
static PFN_vkGetInstanceProcAddr raw_gipa;
static PFN_vkGetDeviceProcAddr raw_gdpa;
static PFN_vkCreateSwapchainKHR raw_create;
static PFN_vkQueuePresentKHR raw_present;
static PFN_vkGetPhysicalDeviceProperties raw_properties;
static PFN_vkGetPhysicalDeviceProperties2 raw_properties2;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR raw_caps;
static PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR raw_caps2;
static char actual_device[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]={};
static uint32_t actual_api;
static std::atomic<unsigned> real_width{0},real_height{0},real_creates{0};
static std::atomic<unsigned> transform{0},supported_transforms{0},current_width{0},current_height{0},created_transform{0};
static std::atomic<uint64_t> real_presents{0};
static std::atomic<uint64_t> suboptimal_presents{0};
static std::atomic<int> last_present{VK_NOT_READY};
static VkResult VKAPI_CALL observed_create(VkDevice d,const VkSwapchainCreateInfoKHR *info,const VkAllocationCallbacks *a,VkSwapchainKHR *out){VkResult r=raw_create(d,info,a,out);if(r==VK_SUCCESS){real_width=info->imageExtent.width;real_height=info->imageExtent.height;created_transform=info->preTransform;++real_creates;}return r;}
static void remember_caps(const VkSurfaceCapabilitiesKHR &p){transform=p.currentTransform;supported_transforms=p.supportedTransforms;current_width=p.currentExtent.width;current_height=p.currentExtent.height;}
static VkResult VKAPI_CALL observed_caps(VkPhysicalDevice d,VkSurfaceKHR s,VkSurfaceCapabilitiesKHR *p){auto r=raw_caps(d,s,p);if(r==VK_SUCCESS)remember_caps(*p);return r;}
static VkResult VKAPI_CALL observed_caps2(VkPhysicalDevice d,const VkPhysicalDeviceSurfaceInfo2KHR *s,VkSurfaceCapabilities2KHR *p){auto r=raw_caps2(d,s,p);if(r==VK_SUCCESS)remember_caps(p->surfaceCapabilities);return r;}
static VkResult VKAPI_CALL observed_present(VkQueue q,const VkPresentInfoKHR *p){VkResult r=raw_present(q,p);last_present=r;if(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR)++real_presents;if(r==VK_SUBOPTIMAL_KHR)++suboptimal_presents;return r;}
static void VKAPI_CALL observed_properties(VkPhysicalDevice d,VkPhysicalDeviceProperties *p){raw_properties(d,p);snprintf(actual_device,sizeof(actual_device),"%s",p->deviceName);actual_api=p->apiVersion;}
static void VKAPI_CALL observed_properties2(VkPhysicalDevice d,VkPhysicalDeviceProperties2 *p){raw_properties2(d,p);snprintf(actual_device,sizeof(actual_device),"%s",p->properties.deviceName);actual_api=p->properties.apiVersion;}
static PFN_vkVoidFunction wrap_proc(const char *name,PFN_vkVoidFunction p){if(!p)return p;
    if(!strcmp(name,"vkGetPhysicalDeviceProperties")){raw_properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_properties);}
    if(!strcmp(name,"vkGetPhysicalDeviceProperties2")||!strcmp(name,"vkGetPhysicalDeviceProperties2KHR")){raw_properties2=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_properties2);}
    if(!strcmp(name,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR")){raw_caps=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_caps);}
    if(!strcmp(name,"vkGetPhysicalDeviceSurfaceCapabilities2KHR")){raw_caps2=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_caps2);}
    if(!strcmp(name,"vkCreateSwapchainKHR")){raw_create=reinterpret_cast<PFN_vkCreateSwapchainKHR>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_create);}
    if(!strcmp(name,"vkQueuePresentKHR")){raw_present=reinterpret_cast<PFN_vkQueuePresentKHR>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_present);}return p;}
static PFN_vkVoidFunction VKAPI_CALL observed_gdpa(VkDevice device,const char *name){return wrap_proc(name,raw_gdpa(device,name));}
static PFN_vkVoidFunction VKAPI_CALL observed_gipa(VkInstance instance,const char *name){auto p=raw_gipa(instance,name);if(p&&!strcmp(name,"vkGetDeviceProcAddr")){raw_gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(p);return reinterpret_cast<PFN_vkVoidFunction>(observed_gdpa);}return wrap_proc(name,p);}

struct GuestState {uint64_t create,level_in,desc,chain_out,device_out,level_out,context_out,iid_texture,buffer_out,rtv_out,color,desc_out;
    uint64_t create_hr,getbuffer_hr,rtv_hr,getdesc_hr,present_hr,completed;uint64_t boxes[4],pixels[4];};
static_assert(sizeof(GuestState)==208&&offsetof(GuestState,completed)==136&&offsetof(GuestState,boxes)==144&&offsetof(GuestState,pixels)==176);
struct window_probe {ANativeWindow *window=nullptr;uint64_t state=0,stack=0,hwnd=0;CPU cpu={};uint64_t frames=0,first_real_present=0,first_suboptimal=0;
    bool rsp=true,canary=true,factory2_setup=false,factory2_cpu=true,factory2_stack=true;
    uint32_t factory2_create_hr=0,factory2_association_hr=0;
    char details[2304]={},name[128]="";};
static GuestState &state(window_probe *p){return *static_cast<GuestState *>(win64_pointer(p->state,sizeof(GuestState),1));}
static void run_guest(window_probe *p,void (*function)(CPU *)) {
    p->cpu={};p->cpu.rcx=p->state;p->cpu.rsp=p->stack+4096;write64(p->cpu.rsp,RETURN);
    p->cpu.rbx=0x123456789ABCDEF0;p->cpu.r12=CANARY;
    function(&p->cpu);p->rsp=p->rsp&&p->cpu.rsp==p->stack+4104&&p->cpu.rbx==0x123456789ABCDEF0&&p->cpu.r12==CANARY;
    p->canary=p->canary&&read64(p->stack)==CANARY&&read64(p->stack+8184)==CANARY;
    require(p->rsp&&p->canary,"lifted fixture RSP/register/stack canary changed");
}
static uint64_t call(window_probe *p,uint64_t object,unsigned slot,uint64_t a=0,uint64_t b=0,uint64_t d=0){CPU c={};c.rcx=object;c.rdx=a;c.r8=b;c.r9=d;c.rsp=p->stack+4096;uint64_t before=c.rsp;require(win64_d3d11_dispatch(&c,read64(read64(object)+slot*8))==1,"cleanup COM target");require(c.rsp==before,"cleanup bridge changed RSP");return c.rax;}
/* Synthetic CPU setup only. Rendering/Present still runs the unchanged,
 * offline-lifted guest_draw assembly on the returned actual SwapChain1. */
static uint64_t call_full(window_probe *p,uint64_t object,unsigned slot,std::initializer_list<uint64_t> arguments){
    require(arguments.size()<=6,"synthetic COM argument limit");uint64_t a[6]={};unsigned n=0;for(auto v:arguments)a[n++]=v;
    CPU c;memset(&c,0xa5,sizeof(c));c.mxcsr=0x1f80;c.rcx=object;c.rdx=a[0];c.r8=a[1];c.r9=a[2];
    const uint64_t base=p->stack+2048;c.rsp=base+8;uint64_t frame[10];for(unsigned i=0;i<10;++i)frame[i]=CANARY^(uint64_t(i)*0x01010101);
    frame[1]=RETURN;frame[6]=a[3];frame[7]=a[4];frame[8]=a[5];memcpy(win64_pointer(base,sizeof(frame),1),frame,sizeof(frame));
    CPU before;memcpy(&before,&c,sizeof(c));require(win64_d3d11_dispatch(&c,read64(read64(object)+slot*8))==1,"Factory2 synthetic COM target");
    uint64_t result=c.rax;c.rax=before.rax;p->factory2_cpu=p->factory2_cpu&&!memcmp(&c,&before,sizeof(c));
    p->factory2_stack=p->factory2_stack&&!memcmp(win64_pointer(base,sizeof(frame),0),frame,sizeof(frame));
    require(p->factory2_cpu&&p->factory2_stack,"Factory2 full CPU/RSP/shadow32/three stack args/guards");++factory2_synthetic_calls;return result;
}
static uint64_t output_slot(){uint64_t out=win64_alloc(24,0);memset(win64_pointer(out,24,1),0xa5,24);write64(out,0);return out;}
static uint64_t output_value(uint64_t out){auto *b=static_cast<const unsigned char *>(win64_pointer(out,24,0));for(unsigned i=8;i<24;++i)require(b[i]==0xa5,"Factory2 interface output canary");return read64(out);}
static uint64_t query(window_probe *p,uint64_t object,const GUID &iid){uint64_t key=put(iid),out=output_slot();require(call_full(p,object,0,{key,out})==0,"Factory2/interface QI HRESULT");uint64_t value=output_value(out);require(value!=0,"Factory2/interface QI NULL");win64_free(out);win64_free(key);return value;}
static void identity_test(window_probe *p,uint64_t object,const GUID &base,const GUID &derived){
    uint64_t b=query(p,object,base),self=query(p,object,derived);require(self==object,"derived COM canonical typed identity");
    uint64_t u=query(p,object,__uuidof(IUnknown)),ub=query(p,b,__uuidof(IUnknown)),us=query(p,self,__uuidof(IUnknown));
    require(u==ub&&u==us,"base/derived canonical IUnknown identity");for(auto x:{u,ub,us,b,self})call_full(p,x,2,{});
}
static void retire_chain(window_probe *p){auto &s=state(p);for(auto out:{s.rtv_out,s.buffer_out})if(read64(out)){call(p,read64(out),2);write64(out,0);}uint64_t context=read64(s.context_out);call(p,context,109);call(p,context,110);if(read64(s.chain_out)){call(p,read64(s.chain_out),2);write64(s.chain_out,0);}}
static void factory2_create(window_probe *p,bool checks){
    auto &s=state(p);uint64_t device=read64(s.device_out);require(device&&read64(s.context_out)&&!read64(s.chain_out),"Factory2 keeps existing actual device/context only");
    uint64_t d2=query(p,device,__uuidof(IDXGIDevice2)),adapter_out=output_slot();require(call_full(p,d2,7,{adapter_out})==0,"IDXGIDevice2 GetAdapter");uint64_t adapter=output_value(adapter_out);require(adapter,"GetAdapter NULL");
    uint64_t iid=put(__uuidof(IDXGIFactory2)),factory_out=output_slot();require(call_full(p,adapter,6,{iid,factory_out})==0,"Adapter GetParent Factory2");uint64_t factory=output_value(factory_out);require(factory,"GetParent Factory2 NULL");
    if(checks)identity_test(p,factory,__uuidof(IDXGIFactory),__uuidof(IDXGIFactory2));
    static_assert(sizeof(DXGI_SWAP_CHAIN_DESC1)==48,"original Win64 DESC1 size");DXGI_SWAP_CHAIN_DESC1 desc={};desc.Width=960;desc.Height=540;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=0x30;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    uint64_t storage=win64_alloc(80,0);memset(win64_pointer(storage,80,1),0xa5,80);memcpy(win64_pointer(storage+16,48,1),&desc,48);unsigned char original[80];memcpy(original,win64_pointer(storage,80,0),80);
    uint64_t chain_output=output_slot();
    if(checks){unsigned creates=real_creates.load();for(uint64_t hwnd:{UINT64_C(0),p->hwnd+8}){write64(chain_output,CANARY);uint64_t hr=call_full(p,factory,15,{device,hwnd,storage+16,0,0,chain_output});require(hr==uint32_t(DXGI_ERROR_INVALID_CALL)&&!output_value(chain_output)&&!read64(s.chain_out)&&real_creates.load()==creates,"Factory2 NULL/foreign HWND must fail before native create");}}
    write64(chain_output,0);p->factory2_create_hr=uint32_t(call_full(p,factory,15,{device,p->hwnd,storage+16,0,0,chain_output}));require(p->factory2_create_hr==0&&output_value(chain_output),"Factory2 CreateSwapChainForHwnd original DESC1");write64(s.chain_out,output_value(chain_output));
    require(!memcmp(original,win64_pointer(storage,80,0),80),"Factory2 input DESC1 and guard canaries unchanged");uint64_t chain=read64(s.chain_out);
    if(checks){identity_test(p,chain,__uuidof(IDXGISwapChain),__uuidof(IDXGISwapChain1));uint64_t iid_device=put(__uuidof(ID3D11Device)),out=output_slot();require(call_full(p,chain,7,{iid_device,out})==0&&output_value(out)==device,"SwapChain1 inherited GetDevice identity");call_full(p,device,2,{});win64_free(out);win64_free(iid_device);}
    /* The game ignores this HRESULT; preserve and report native result. */
    p->factory2_association_hr=uint32_t(call_full(p,factory,8,{p->hwnd,1}));
    s.getbuffer_hr=call_full(p,chain,9,{0,s.iid_texture,s.buffer_out});require(s.getbuffer_hr==0&&read64(s.buffer_out),"SwapChain1 inherited GetBuffer");
    s.rtv_hr=call_full(p,device,9,{read64(s.buffer_out),0,s.rtv_out});require(s.rtv_hr==0&&read64(s.rtv_out),"Factory2 backbuffer RTV");
    memset(win64_pointer(s.desc_out,sizeof(DXGI_SWAP_CHAIN_DESC)+16,1),0xa5,sizeof(DXGI_SWAP_CHAIN_DESC)+16);s.getdesc_hr=call_full(p,chain,12,{s.desc_out});
    auto *returned=static_cast<DXGI_SWAP_CHAIN_DESC *>(win64_pointer(s.desc_out,sizeof(DXGI_SWAP_CHAIN_DESC)+16,0));require(!s.getdesc_hr&&returned->OutputWindow==reinterpret_cast<HWND>(p->hwnd)&&returned->BufferDesc.Width==960&&returned->BufferDesc.Height==540&&returned->BufferUsage==0x30&&returned->SwapEffect==DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,"SwapChain1 inherited GetDesc guest HWND/original descriptor");
    auto *bytes=reinterpret_cast<unsigned char *>(returned);for(unsigned i=sizeof(*returned);i<sizeof(*returned)+16;++i)require(bytes[i]==0xa5,"SwapChain1 GetDesc canary");
    for(auto object:{factory,adapter,d2})call_full(p,object,2,{});for(auto address:{adapter_out,factory_out,iid,storage,chain_output})win64_free(address);
    s.create_hr=p->factory2_create_hr;s.completed=1;p->factory2_setup=true;
}
static void strict_lease_test(window_probe *p,bool factory2=false){
    auto &s=state(p);uint64_t device=read64(s.device_out),context=read64(s.context_out),chain=read64(s.chain_out);
    GUID key={0x628405fa,0x7c55,0x4ce7,{0x8a,0xfb,0x54,0x10,0xb0,0xd3,0x57,0x92}};
    uint64_t guid=put(key),size=put(UINT(8)),unknown_out=put(uint64_t()),restored_out=put(uint64_t()),iid=factory2?put(__uuidof(IDXGISwapChain1)):put(__uuidof(IDXGISwapChain));
    require(int32_t(call(p,device,36,guid,chain))>=0,"native device retains swapchain via private interface");
    for(uint64_t out:{s.rtv_out,s.buffer_out}){call(p,read64(out),2);write64(out,0);}
    call(p,context,109);call(p,context,110);call(p,chain,2);write64(s.chain_out,0);
    require(window_acquires.load()==window_releases.load()+1,"surface lease released while native swapchain still alive");
    require(int32_t(call(p,device,34,guid,size,unknown_out))>=0,"GetPrivateData restores native-held interface");
    uint64_t unknown=read64(unknown_out);require(unknown,"missing native-held swapchain interface");
    require(int32_t(call(p,unknown,0,iid,restored_out))>=0&&read64(restored_out)==chain,"native-held canonical swapchain guest identity changed");
    require(int32_t(call(p,chain,12,s.desc_out))>=0,"native-held swapchain GetDesc");
    auto *description=static_cast<DXGI_SWAP_CHAIN_DESC *>(win64_pointer(s.desc_out,sizeof(DXGI_SWAP_CHAIN_DESC),0));
    require(description->OutputWindow==reinterpret_cast<HWND>(p->hwnd),"native-held GetDesc leaked native window pointer");
    call(p,chain,2);call(p,unknown,2);
    require(window_acquires.load()==window_releases.load()+1,"guest revival/release prematurely released surface lease");
    require(int32_t(call(p,device,36,guid,0))>=0,"remove last native swapchain hold");
    for(unsigned wait=0;wait<1000&&window_acquires.load()!=window_releases.load();++wait)usleep(1000);
    require(window_acquires.load()==window_releases.load()&&!accessible(chain,8,false),"native swapchain destruction did not release surface lease/proxy");
    if(!factory2){call(p,context,2);call(p,device,2);write64(s.context_out,0);write64(s.device_out,0);}
    for(uint64_t address:{guid,size,unknown_out,restored_out,iid})win64_free(address);
    ++lease_fixture_chains;s.completed=0;if(factory2){factory2_lease_passed=true;factory2_create(p,false);}else{strict_fixture_passed=true;run_guest(p,guest_create_lifted);}require(s.completed==1,"create render swapchain after lease fixture");
}
extern "C" void window_probe_stop(window_probe *p){if(!p)return;
    try{if(p->state){auto &s=state(p);uint64_t context=read64(s.context_out);if(context){call(p,context,109);call(p,context,110);}
        for(uint64_t output:{s.rtv_out,s.buffer_out,s.chain_out,s.context_out,s.device_out})if(output&&read64(output))call(p,read64(output),2);
        for(uint64_t address:{s.level_in,s.desc,s.chain_out,s.device_out,s.level_out,s.context_out,s.iid_texture,s.buffer_out,s.rtv_out,s.color,s.desc_out})if(address)win64_free(address);
        for(unsigned i=0;i<4;++i){if(s.boxes[i])win64_free(s.boxes[i]);if(s.pixels[i])win64_free(s.pixels[i]);}
        win64_free(p->state);}if(p->stack)win64_free(p->stack);if(p->hwnd)win64_free(p->hwnd);}catch(const std::exception &e){fprintf(stderr,"APP_DXVK cleanup: %s\n",e.what());}
    if(p->window)ANativeWindow_release(p->window);delete p;}
extern "C" window_probe *window_probe_start(ANativeWindow *window,PFN_vkGetInstanceProcAddr provider,char *error,size_t size){
    window_probe *p=new window_probe();try{require(window&&provider,"missing actual Android window/provider");p->window=window;ANativeWindow_acquire(window);
        require(!raw_gipa||raw_gipa==provider,"changing underlying Vulkan driver");raw_gipa=provider;win64_d3d11_configure(observed_gipa,window);win64_d3d11_set_display_mode(960,540,60);p->first_real_present=real_presents;p->first_suboptimal=suboptimal_presents;
        p->stack=win64_alloc(8192,1);write64(p->stack,CANARY);write64(p->stack+8184,CANARY);
        /* This test registry owns one opaque HWND for the acquired real surface.
         * Production uses the same resolver ABI with the USER32 HWND registry. */
        /* Zero dimensions must resolve to the explicit logical window extent,
         * while Vulkan presentation still observes the physical Android view. */
        p->hwnd=put(CANARY);DXGI_SWAP_CHAIN_DESC desc={};desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.OutputWindow=reinterpret_cast<HWND>(p->hwnd);desc.Windowed=1;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        GuestState s={};s.create=IMPORT;s.level_in=put(D3D_FEATURE_LEVEL_11_0);s.desc=put(desc);s.chain_out=put(uint64_t());s.device_out=put(uint64_t());s.level_out=put(uint32_t());s.context_out=put(uint64_t());
        s.iid_texture=put(__uuidof(ID3D11Texture2D));s.buffer_out=put(uint64_t());s.rtv_out=put(uint64_t());float color[4]={0.2f,0.2f,0.8f,1};s.color=put(color);
        s.desc_out=win64_alloc(sizeof(DXGI_SWAP_CHAIN_DESC)+16,0);memset(win64_pointer(s.desc_out,sizeof(DXGI_SWAP_CHAIN_DESC)+16,1),0xA5,sizeof(DXGI_SWAP_CHAIN_DESC)+16);
        const uint32_t colors[4]={0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFF00FFFF};
        for(unsigned i=0;i<4;++i){D3D11_BOX box={i%2?832u:0u,i/2?412u:0u,0,i%2?960u:128u,i/2?540u:128u,1};s.boxes[i]=put(box);s.pixels[i]=win64_alloc(128*128*4,0);
            auto *pixel=static_cast<uint32_t *>(win64_pointer(s.pixels[i],128*128*4,1));for(unsigned n=0;n<128*128;++n)pixel[n]=colors[i];}
        p->state=put(s);registered_hwnd=p->hwnd;registered_native=window;++registered_generation;
        win64_d3d11_set_window_resolver(fixture_acquire,fixture_release);
        unsigned native_before=real_creates.load();auto *input_desc=static_cast<DXGI_SWAP_CHAIN_DESC *>(win64_pointer(s.desc,sizeof(desc),1));
        for(uint64_t invalid:{p->hwnd+8,UINT64_C(0)}){input_desc->OutputWindow=reinterpret_cast<HWND>(invalid);run_guest(p,guest_create_lifted);auto &failed=state(p);
            require(failed.create_hr==uint32_t(DXGI_ERROR_INVALID_CALL)&&!failed.completed&&!read64(s.chain_out)&&!read64(s.device_out)&&!read64(s.context_out),"foreign/NULL HWND accepted or failed outputs not cleared");
            require(real_creates.load()==native_before,"foreign HWND reached native swapchain creation");}
        input_desc->OutputWindow=reinterpret_cast<HWND>(p->hwnd);run_guest(p,guest_create_lifted);s=state(p);
        if(!s.completed){snprintf(error,size,"Win64 D3D11 create=%08x buffer=%08x rtv=%08x desc=%08x",unsigned(s.create_hr),unsigned(s.getbuffer_hr),unsigned(s.rtv_hr),unsigned(s.getdesc_hr));throw std::runtime_error(error);}
        require(*static_cast<uint32_t *>(win64_pointer(s.level_out,4,0))==D3D_FEATURE_LEVEL_11_0,"feature level 11_0 mismatch");
        auto *returned=static_cast<DXGI_SWAP_CHAIN_DESC *>(win64_pointer(s.desc_out,sizeof(desc)+16,0));
        require(returned->BufferDesc.Width==960&&returned->BufferDesc.Height==540&&returned->OutputWindow==reinterpret_cast<HWND>(p->hwnd),"Win64 swapchain GetDesc extent/HWND mismatch");
        uint32_t logical_width=0,logical_height=0,logical_refresh=0;
        require(frostpunk_dxvk_get_display_mode(&logical_width,&logical_height,&logical_refresh)==1&&logical_width==960&&logical_height==540&&logical_refresh==60,"explicit logical display callback mismatch");
        require(!input_desc->BufferDesc.Width&&!input_desc->BufferDesc.Height,"zero-size guest swapchain descriptor changed");
        auto *bytes=reinterpret_cast<unsigned char *>(returned);for(unsigned i=sizeof(desc);i<sizeof(desc)+16;++i)require(bytes[i]==0xA5,"GetDesc Win64 output canary");
        if(!strict_fixture_passed.load())strict_lease_test(p);
        retire_chain(p);factory2_create(p,true);if(!factory2_lease_passed.load())strict_lease_test(p,true);
        require(actual_api>=VK_API_VERSION_1_3&&actual_device[0],"real native Vulkan1.3 device properties not observed");snprintf(p->name,sizeof(p->name),"%s",actual_device);
        uint64_t support=put(uint32_t());uint64_t result=call(p,read64(s.device_out),29,DXGI_FORMAT_BC3_UNORM,support);
        bool bc3=int32_t(result)>=0&&(*static_cast<uint32_t *>(win64_pointer(support,4,0))&D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);win64_free(support);require(bc3,"actual DXVK BC3 shader sampling format support absent");
        return p;
    }catch(const std::exception &e){snprintf(error,size,"%s",e.what());window_probe_stop(p);return nullptr;}}
extern "C" int window_probe_frame(window_probe *p,float x,float y){try{auto &s=state(p);float color[4]={x,y,0.8f,1};memcpy(win64_pointer(s.color,sizeof(color),1),color,sizeof(color));
    run_guest(p,guest_draw_lifted);if(int32_t(s.present_hr)<0)return int32_t(s.present_hr);if(s.present_hr!=0)return 0;++p->frames;return 1;
    }catch(const std::exception &e){fprintf(stderr,"APP_DXVK_FAIL %s\n",e.what());return -1;}}
extern "C" const char *window_probe_device(window_probe *p){return p?p->name:"";}
extern "C" uint64_t window_probe_frames(window_probe *p){return p?p->frames:0;}
extern "C" void window_probe_extent(window_probe *p,uint32_t *w,uint32_t *h){*w=p?real_width.load():0;*h=p?real_height.load():0;}
extern "C" const char *window_probe_details(window_probe *p){if(!p)return "null";auto &s=state(p);
    auto &desc=*static_cast<DXGI_SWAP_CHAIN_DESC *>(win64_pointer(s.desc_out,sizeof(DXGI_SWAP_CHAIN_DESC),0));
    snprintf(p->details,sizeof(p->details),"{\"factory2_setup_synthetic_CPU_frames\":true,\"factory2_swapchain1_offline_draw\":%s,\"factory2_original_desc1\":%s,\"factory2_create_hresult\":%u,\"factory2_association_hresult\":%u,\"factory2_full_cpu_stack_canaries\":%s,\"factory2_native_held_lease_lifetime_pass\":%s,\"factory2_synthetic_setup_calls\":%u,\"zero_size_descriptor_scope\":\"legacy_regression\",\"offline_lifted_x64_fixture\":true,\"render_cpu_frames_synthesized_at_runtime\":false,\"lease_fixture_synthetic_CPU_frames\":true,\"explicit_guest_display\":true,\"zero_size_swapchain_descriptor\":true,\"guest_display_width\":960,\"guest_display_height\":540,\"guest_refresh_hz\":60,\"guest_calls\":%u,\"create_hresult\":%u,\"present_hresult\":%u,\"getdesc_canary\":true,\"rsp_and_nonvolatile_canary\":%s,\"stack_canary\":%s,\"dxgi_backbuffer_width\":%u,\"dxgi_backbuffer_height\":%u,\"native_vk_api\":%u,\"BC3_sample_supported\":true,\"surface_current_transform\":%u,\"surface_supported_transforms\":%u,\"surface_current_width\":%u,\"surface_current_height\":%u,\"swapchain_pre_transform\":%u,\"native_vk_swapchains_created\":%u,\"native_vk_presents\":%llu,\"native_last_vk_present\":%d,\"native_suboptimal_presents\":%llu,\"strict_resolver_foreign_rejections\":%u,\"surface_acquires\":%u,\"surface_releases\":%u,\"native_held_lease_lifetime_pass\":%s,\"lease_fixture_swapchains\":%u,\"production_USER32_registry_fixture\":false}",
        p->frames?"true":"false",p->factory2_setup?"true":"false",p->factory2_create_hr,p->factory2_association_hr,p->factory2_cpu&&p->factory2_stack?"true":"false",factory2_lease_passed.load()?"true":"false",factory2_synthetic_calls.load(),calls.load(),unsigned(s.create_hr),unsigned(s.present_hr),p->rsp?"true":"false",p->canary?"true":"false",desc.BufferDesc.Width,desc.BufferDesc.Height,actual_api,transform.load(),supported_transforms.load(),current_width.load(),current_height.load(),created_transform.load(),real_creates.load(),(unsigned long long)(real_presents.load()-p->first_real_present),last_present.load(),(unsigned long long)(suboptimal_presents.load()-p->first_suboptimal),foreign_rejections.load(),window_acquires.load(),window_releases.load(),strict_fixture_passed?"true":"false",lease_fixture_chains.load());return p->details;}
