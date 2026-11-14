/* Win64 guest COM -> native AAPCS64 DXVK. Guest data is RW/NX; only typed
 * compiled C++ functions execute. Unsupported slots abort with exact names.
 */
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
#include <cstdio>
#include <cerrno>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "win64.h"
#include "../host/texture_formats.h"
#include "bridge.h"
#include "bridge_metadata.h"
#include "buffer_map_count.h"
#include "meshoptimizer.h"
extern "C" {
#include "win64_kernel.h"
}
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../reference/goldberg/stb/stb_image_write.h"
extern "C" int frostpunk_android_photo_saved(const char*) __attribute__((weak));
extern "C" void frostpunk_android_frame_pace(void) __attribute__((weak));
extern "C" int frostpunk_dxvk_mobile_effects(void) __attribute__((weak));
extern "C" int frostpunk_mobile_graphics_pc(unsigned group) __attribute__((weak));  /* port_settings.h, FP_GFX_* */
extern "C" void frostpunk_thread_placement_frame(void) __attribute__((weak));

namespace {
constexpr uint64_t SENTINEL = 0xE5F00000;
constexpr unsigned STRIDE = 256;
std::atomic<PFN_vkGetInstanceProcAddr> provider{nullptr};
std::atomic<ANativeWindow*> window{nullptr};
std::atomic<uint64_t> display_extent{0};
std::atomic<uint32_t> display_refresh{0};
win64_d3d11_window_acquire_fn acquire_guest_window=nullptr;
win64_d3d11_window_release_fn release_guest_window=nullptr;
std::recursive_mutex mutex;
struct WindowLease {
    ANativeWindow *native;
    uint64_t guest,generation;
    win64_d3d11_window_release_fn release;
    ~WindowLease(){if(native)release(native);}
};
struct Proxy {
    uint64_t guest;
    IUnknown* native;
    IUnknown* identity;
    Kind kind;
    uint32_t refs;
    BufferMapCount* buffer_maps;
};
struct Identity {
    size_t refs=0;
    BufferMapCount buffer_maps;
    std::vector<uint64_t> proxies;
    std::unordered_map<std::string,bool> private_types;
    std::shared_ptr<std::recursive_mutex> private_mutex=std::make_shared<std::recursive_mutex>();
};
std::unordered_map<uint64_t, std::unique_ptr<Proxy>> proxies;
std::unordered_map<IUnknown*, Identity> identities;
std::array<uint64_t, unsigned(Kind::Count)> vtables = {};
bool identity_has_mapping(IUnknown* identity);
std::unique_ptr<WindowLease> identity_cleanup(IUnknown* identity);
void attach_lifetime_marker(IUnknown* object,IUnknown* identity);
const GUID LIFETIME_KEY={0xc24431d8,0x7933,0x46cb,{0x98,0xe7,0x1e,0x93,0xa5,0x40,0x18,0x53}};

[[noreturn]] void trap(const char* reason, Kind kind = Kind::IUnknown, unsigned slot = 0) {
    fprintf(stderr, "UNSUPPORTED_D3D11: %s interface=%s slot=%u method=%s\n", reason,
        kind_names[unsigned(kind)], slot, slot < slot_counts[unsigned(kind)] ? slot_names[unsigned(kind)][slot] : "out_of_range");
    abort();
}
size_t multiply(size_t a, size_t b) {
    if (b && a > SIZE_MAX / b) trap("size overflow");
    return a*b;
}
template<class T> T* ptr(uint64_t p, size_t count = 1, bool write = !std::is_const_v<T>) {
    if (!p) return nullptr;
    return static_cast<T*>(win64_pointer(p, multiply(sizeof(T), count), write));
}
template<class T> T& ref(uint64_t address) { T* p=ptr<T>(address); if(!p) trap("null required descriptor/reference"); return *p; }
/* Every COM call reads its arguments here: register arguments inline; stack arguments keep
 * win64_argument (and its guest-pointer check in checked builds). */
inline uint64_t arg(CPU* c, unsigned n) {
    switch (n) { case 0: return c->rcx; case 1: return c->rdx; case 2: return c->r8; case 3: return c->r9; default: break; }
    return win64_argument(c, n);
}
uint64_t read64(uint64_t p) { uint64_t v; memcpy(&v, win64_pointer(p, 8, 0), 8); return v; }
void write64(uint64_t p, uint64_t v) { memcpy(win64_pointer(p, 8, 1), &v, 8); }
/* A guest proxy object is [vtable][Proxy*][tag]; the guest only reads the
 * vtable. Proxy records live until the native object is destroyed, after the
 * last guest reference, so calls resolve them without the registry lock. */
constexpr uint64_t PROXY_TAG = UINT64_C(0x3152425958525050);
Proxy& proxy(uint64_t p) {
    uint64_t slots[3] = {};
    if (p) memcpy(slots, reinterpret_cast<const void*>(p), sizeof(slots));
    Proxy* record = reinterpret_cast<Proxy*>(slots[1]);
    if (!p || slots[2] != PROXY_TAG || !record || record->guest != p || !record->refs) trap("invalid/released guest COM object");
    return *record;
}
template<class T, class = void> struct HasKind : std::false_type {};
template<class T> struct HasKind<T, std::void_t<decltype(KindOf<T>::value)>> : std::true_type {};
/* Resource base interfaces share the typed proxy's inheritance path. */
template<class T> T* exact_native(const Proxy& record) {
    if constexpr (HasKind<T>::value) {
        if (record.kind == KindOf<T>::value) return static_cast<T*>(record.native);
    }
    if constexpr (std::is_same_v<T, ID3D11Resource>) {
        switch (record.kind) {
        case Kind::ID3D11Buffer: return static_cast<ID3D11Buffer*>(record.native);
        case Kind::ID3D11Texture1D: return static_cast<ID3D11Texture1D*>(record.native);
        case Kind::ID3D11Texture2D: return static_cast<ID3D11Texture2D*>(record.native);
        case Kind::ID3D11Texture3D: return static_cast<ID3D11Texture3D*>(record.native);
        default: break;
        }
    }
    return nullptr;
}
uint64_t wrap(IUnknown* object, Kind kind) {
    if (!object) return 0;
    std::lock_guard lock(mutex);
    if (kind == Kind::Count) trap("unrepresented native interface");
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity))) || !identity)
        trap("native COM identity failure");
    identity->Release();
    auto insertion=identities.try_emplace(identity);
    auto& owner=insertion.first->second;
    if (insertion.second) attach_lifetime_marker(object,identity);
    for (uint64_t p : owner.proxies) {
        Proxy& old = *proxies.at(p);
        if (old.kind == kind && old.native == object) { ++old.refs; ++owner.refs; return p; }
    }
    unsigned k = unsigned(kind);
    if (!vtables[k]) {
        vtables[k] = win64_alloc(slot_counts[k]*8, 1);
        for (unsigned slot = 0; slot < slot_counts[k]; ++slot)
            write64(vtables[k] + slot*8, SENTINEL + (k*STRIDE + slot)*8);
    }
    uint64_t guest = win64_alloc(24, 1);
    write64(guest, vtables[k]);
    auto record = std::make_unique<Proxy>(Proxy{guest, object, identity, kind, 1, &owner.buffer_maps});
    write64(guest + 8, reinterpret_cast<uint64_t>(record.get()));
    write64(guest + 16, PROXY_TAG);
    proxies.emplace(guest, std::move(record));
    owner.proxies.push_back(guest); ++owner.refs;
    return guest;
}
template<class T> uint64_t wrap(T* p) { return wrap(static_cast<IUnknown*>(p), KindOf<T>::value); }
template<class T> struct Native {
    T* p = nullptr;
    bool owned = false;
    explicit Native(uint64_t guest) {
        if (!guest) return;
        Proxy& record = proxy(guest);
        if ((p = exact_native<T>(record))) return;
        if (FAILED(record.native->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&p))))
            trap("guest object does not implement required interface");
        owned = true;
    }
    Native(const Native&) = delete;
    ~Native() { if (owned && p) p->Release(); }
    T* operator->() const { return p; }
};
using Counts = std::array<size_t, 16>;
Counts counts(std::initializer_list<std::pair<unsigned, size_t>> changes = {}) {
    Counts result; result.fill(1);
    for (auto change : changes) result.at(change.first) = change.second;
    return result;
}
template<class A> class Argument {
    using Raw = std::remove_reference_t<A>;
    using Pointed = std::remove_pointer_t<Raw>;
    using Inner = std::remove_cv_t<Pointed>;
    using Element = std::remove_cv_t<std::remove_pointer_t<Inner>>;
    using Storage = std::conditional_t<std::is_reference_v<A>, std::add_pointer_t<Raw>, Raw>;
    Storage value{};
    uint64_t guest = 0;
    std::vector<IUnknown*> held;
    std::vector<Element*> interfaces;
    std::array<Element*, 16> inline_items{};
    size_t item_count = 0;
public:
    Argument(const Argument&) = delete;
    // Inline array storage is self-referential: re-point it after a move.
    Argument(Argument&& o) noexcept
    : value(o.value), guest(o.guest), held(std::move(o.held)), interfaces(std::move(o.interfaces)),
      inline_items(o.inline_items), item_count(o.item_count) {
        o.held.clear();
        if constexpr (std::is_pointer_v<A> && std::is_pointer_v<Inner> && std::is_base_of_v<IUnknown, Element>) {
            if (item_count && item_count <= inline_items.size()) value = inline_items.data();
        }
    }
    Argument(CPU* cpu, unsigned index, size_t count) {
        uint64_t bits = arg(cpu, index); guest = bits;
        if constexpr (std::is_reference_v<A>) {
            value = ptr<Raw>(bits);
            if (!value) trap("null required reference argument");
        } else if constexpr (std::is_pointer_v<A> && std::is_base_of_v<IUnknown, std::remove_cv_t<Pointed>>) {
            if (bits) {
                using T = std::remove_cv_t<Pointed>;
                Proxy& record = proxy(bits);
                T* p = exact_native<T>(record);
                if (!p) {
                    if (FAILED(record.native->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&p)))) trap("wrong COM input type");
                    held.push_back(p);
                }
                value = p;
            }
        } else if constexpr (std::is_pointer_v<A> && std::is_pointer_v<Inner> && std::is_base_of_v<IUnknown, Element>) {
            if (bits) {
                if (count > 4096) trap("COM array too large");
                ptr<uint64_t>(bits, count, !std::is_const_v<Pointed>);
                Element** items = count <= inline_items.size() ? inline_items.data() : (interfaces.resize(count, nullptr), interfaces.data());
                std::fill(items, items + count, nullptr);
                item_count = count;
                if constexpr (std::is_const_v<Pointed>) {
                    for (size_t i = 0; i < count; ++i) {
                        uint64_t p = read64(bits+i*8);
                        if (!p) continue;
                        Proxy& record = proxy(p);
                        if ((items[i] = exact_native<Element>(record))) continue;
                        if (FAILED(record.native->QueryInterface(__uuidof(Element), reinterpret_cast<void**>(&items[i])))) trap("wrong COM array type");
                        if (items[i]) held.push_back(items[i]);
                    }
                }
                value = items;
            }
        } else if constexpr (std::is_pointer_v<A>) {
            if constexpr (std::is_void_v<std::remove_cv_t<Pointed>>) {
                value = bits ? win64_pointer(bits, count, !std::is_const_v<Pointed>) : nullptr;
            } else if constexpr (std::is_same_v<A, const char*>) {
                value = bits ? win64_string_a(bits) : nullptr;
            } else value = ptr<Pointed>(bits, count);
        } else if constexpr (std::is_same_v<A, float>) {
            uint32_t v = index < 4 ? cpu->xmm[index].u32[0] : uint32_t(bits);
            memcpy(&value, &v, 4);
        } else value = static_cast<A>(bits);
    }
    ~Argument() { for (IUnknown* p : held) p->Release(); }
    A get() { if constexpr (std::is_reference_v<A>) return *value; else return value; }
    void commit() {
        if constexpr (std::is_pointer_v<A> && std::is_pointer_v<Inner> && std::is_base_of_v<IUnknown, Element> && !std::is_const_v<Pointed>) {
            if (guest) {
                Element** items = item_count <= inline_items.size() ? inline_items.data() : interfaces.data();
                for (size_t i = 0; i < item_count; ++i) write64(guest+i*8, wrap(items[i]));
            }
        }
    }
};
/* Bounded native D3D descriptor observation only. The existing bridge mutex
 * protects fixed storage. Full tables stop observing; no per-draw hooks.
 */
struct GfxDiagnosticErrors {
    int error=errno;
    fenv_t environment;
    bool saved=fegetenv(&environment)==0;
    ~GfxDiagnosticErrors() { if(saved)fesetenv(&environment);errno=error; }
};
static_assert(sizeof(D3D11_TEXTURE2D_DESC)==44,
    "D3D11 diagnostic descriptor layout");
std::array<D3D11_TEXTURE2D_DESC,64> gfx_textures;
std::array<D3D11_TEXTURE2D_DESC,16> gfx_backbuffers;
std::array<UINT,16> gfx_backbuffer_indices;
std::atomic<unsigned> gfx_texture_count{0},gfx_backbuffer_count{0};
void gfx_log_texture(const char* source,unsigned sequence,UINT index,const D3D11_TEXTURE2D_DESC& d) {
    fprintf(stderr,"BRIDGE_GFX_TEXTURE source=%s sequence=%u index=%u width=%u height=%u mips=%u layers=%u format=0x%08x samples=%u sample_quality=%u usage=%u bind=0x%08x cpu_access=0x%08x misc=0x%08x\n",
        source,sequence,index,d.Width,d.Height,d.MipLevels,d.ArraySize,unsigned(d.Format),
        d.SampleDesc.Count,d.SampleDesc.Quality,unsigned(d.Usage),d.BindFlags,d.CPUAccessFlags,d.MiscFlags);
}
void gfx_observe_created_texture(ID3D11Texture2D* texture) {
    if(!texture || gfx_texture_count.load(std::memory_order_relaxed)>=gfx_textures.size())return;
    GfxDiagnosticErrors errors;
    D3D11_TEXTURE2D_DESC d={};texture->GetDesc(&d);
    if(!(d.BindFlags&(D3D11_BIND_RENDER_TARGET|D3D11_BIND_DEPTH_STENCIL)))return;
    std::lock_guard lock(mutex);
    unsigned n=gfx_texture_count.load(std::memory_order_relaxed);
    for(unsigned i=0;i<n;++i)if(!memcmp(&gfx_textures[i],&d,sizeof(d)))return;
    if(n==gfx_textures.size())return;
    gfx_textures[n]=d;gfx_texture_count.store(n+1,std::memory_order_relaxed);
    gfx_log_texture("CreateTexture2D",n+1,0,d);
    if(n+1==gfx_textures.size())fprintf(stderr,"BRIDGE_GFX_LIMIT source=CreateTexture2D cap=64\n");
}
void gfx_observe_backbuffer(IUnknown* object,UINT index) {
    if(!object || gfx_backbuffer_count.load(std::memory_order_relaxed)>=gfx_backbuffers.size())return;
    GfxDiagnosticErrors errors;
    ID3D11Texture2D* texture=nullptr;
    HRESULT hr=object->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture));
    if(FAILED(hr)||!texture)return;
    D3D11_TEXTURE2D_DESC d={};texture->GetDesc(&d);texture->Release();
    std::lock_guard lock(mutex);
    unsigned n=gfx_backbuffer_count.load(std::memory_order_relaxed);
    for(unsigned i=0;i<n;++i)if(gfx_backbuffer_indices[i]==index&&!memcmp(&gfx_backbuffers[i],&d,sizeof(d)))return;
    if(n==gfx_backbuffers.size())return;
    gfx_backbuffers[n]=d;gfx_backbuffer_indices[n]=index;gfx_backbuffer_count.store(n+1,std::memory_order_relaxed);
    gfx_log_texture("GetBuffer",n+1,index,d);
    if(n+1==gfx_backbuffers.size())fprintf(stderr,"BRIDGE_GFX_LIMIT source=GetBuffer cap=16\n");
}

template<class T, class R, class... A, size_t... I>
void invoke_impl(CPU* cpu, T* object, R (T::*method)(A...), const Counts& n, std::index_sequence<I...>) {
    std::tuple<Argument<A>...> arguments{Argument<A>(cpu, I+1, n[I+1])...};
    if constexpr (std::is_void_v<R>) (object->*method)(std::get<I>(arguments).get()...);
    else {
        R result = (object->*method)(std::get<I>(arguments).get()...);
        if constexpr (std::is_same_v<R, float>) cpu->xmm[0].f32[0] = result;
        else if constexpr (sizeof(R) <= 4) cpu->rax = uint32_t(result);
        else cpu->rax = uint64_t(result);
        if constexpr (std::is_same_v<decltype(method),decltype(&ID3D11Device::CreateTexture2D)>) {
            if(method==&ID3D11Device::CreateTexture2D && SUCCEEDED(result)) {
                auto output=std::get<2>(arguments).get();
                if(output)gfx_observe_created_texture(*output);
            }
        }
    }
    (std::get<I>(arguments).commit(), ...);
}
template<class T, class R, class... A>
void invoke(CPU* cpu, R (T::*method)(A...), Counts n = counts()) {
    Native<T> object(arg(cpu, 0));
    if (!object.p) trap("null method this pointer");
    invoke_impl(cpu, object.p, method, n, std::index_sequence_for<A...>());
}
IUnknown* identity_of(uint64_t guest) { std::lock_guard lock(mutex); return proxy(guest).identity; }
void unknown(CPU* c, unsigned slot) {
    IUnknown* release_object=nullptr;
    {
        std::lock_guard lock(mutex);
        Proxy& p=proxy(arg(c,0));
        if (slot==0) {
            const IID& iid=ref<const IID>(arg(c,1)); write64(arg(c,2),0);
            IUnknown* result=nullptr;
            HRESULT hr=p.native->QueryInterface(iid,reinterpret_cast<void**>(&result));
            if (SUCCEEDED(hr) && result) {
                Kind kind=kind_for_iid(iid);
                if (kind==Kind::Count) { result->Release(); trap("native QueryInterface succeeded for unsupported IID",p.kind,slot); }
                write64(arg(c,2),wrap(result,kind));
            }
            c->rax=uint32_t(hr);
        } else if (slot==1) {
            c->rax=p.native->AddRef(); ++p.refs; ++identities[p.identity].refs;
        } else {
            auto identity=p.identity;
            if (identities[identity].refs==1 && identity_has_mapping(identity))
                trap("final guest Release while native resource is still mapped",p.kind,slot);
            release_object=p.native; --p.refs;
            auto& owner=identities[identity];
            --owner.refs;
        }
    }
    // Final native Release may destroy a device or wait for its GPU work.
    // Guest metadata is already detached; no registry lock spans this call.
    if (release_object) c->rax=release_object->Release();
}
std::string guid_key(const GUID& guid) { return std::string(reinterpret_cast<const char*>(&guid), sizeof(guid)); }
template<class T> void private_data(CPU* c, unsigned operation) {
    Native<T> object(arg(c,0));
    const GUID& guid=ref<const GUID>(arg(c,1));
    if (guid==LIFETIME_KEY) trap("reserved native COM lifetime metadata");
    IUnknown* identity=identity_of(arg(c,0));
    std::string key=guid_key(guid);
    std::shared_ptr<std::recursive_mutex> private_mutex;
    { std::lock_guard lock(mutex); private_mutex=identities.at(identity).private_mutex; }
    // Serialize representation metadata with the matching native private-data
    // operation, without blocking unrelated objects or GPU Map/Draw calls.
    std::lock_guard private_lock(*private_mutex);
    if (operation==1 || operation==2) {
        if (operation==1) invoke(c,&T::SetPrivateData,counts({{3,uint32_t(arg(c,2))}}));
        else invoke(c,&T::SetPrivateDataInterface);
        if (SUCCEEDED(HRESULT(c->rax))) {
            std::lock_guard lock(mutex);
            identities.at(identity).private_types[key]=operation==2;
        }
    } else {
        UINT* size=ptr<UINT>(arg(c,2));
        if (!size) trap("GetPrivateData null size");
        bool found=false,is_interface=false;
        {
            std::lock_guard lock(mutex);
            const auto& known=identities.at(identity).private_types;
            auto it=known.find(key); found=it!=known.end();
            if (found) is_interface=it->second;
        }
        if (found && is_interface) {
            IUnknown* value=nullptr; UINT native_size=sizeof(value);
            void* output=arg(c,3)?win64_pointer(arg(c,3),*size,1):nullptr;
            if (!output || *size<8) c->rax=uint32_t(object->GetPrivateData(guid,size,nullptr));
            else {
                HRESULT hr=object->GetPrivateData(guid,&native_size,&value); *size=native_size;
                if (SUCCEEDED(hr)) write64(arg(c,3),wrap(value,Kind::IUnknown));
                c->rax=uint32_t(hr);
            }
        } else if (!found) {
            UINT native_size=0; HRESULT hr=object->GetPrivateData(guid,&native_size,nullptr);
            if (hr==DXGI_ERROR_NOT_FOUND) { *size=native_size; c->rax=uint32_t(hr); }
            else trap("untracked native private data representation");
        } else invoke(c,&T::GetPrivateData,counts({{3,*size}}));
    }
}

struct Extent { size_t row_bytes, rows, depth; unsigned width=0,height=0,block=1,block_bytes=1; };
Extent extent(DXGI_FORMAT format, unsigned width, unsigned height, unsigned depth) {
    unsigned f = unsigned(format), bytes = 0, block = 1;
    if (f-FP_ASTC4_DXGI_TYPELESS<3) { block=4;bytes=16; }
    else if (f-FP_ASTC6_DXGI_TYPELESS<3) { block=6;bytes=16; }
    else if (f >= 1 && f <= 4) bytes = 16;
    else if (f >= 5 && f <= 8) bytes = 12;
    else if (f >= 9 && f <= 22) bytes = 8;
    else if ((f >= 23 && f <= 47) || f == 67 || (f >= 87 && f <= 93)) bytes = 4;
    else if (f >= 48 && f <= 59) bytes = 2;
    else if (f >= 60 && f <= 65) bytes = 1;
    else if (f==85 || f==86) bytes=2;
    else if (f >= 70 && f <= 99) {
        block = 4; bytes = (f <= 72 || (f >= 79 && f <= 81)) ? 8 : 16;
        if ((f >= 85 && f <= 93)) trap("unhandled packed pixel format");
    } else trap("unhandled texture upload/map format");
    return {multiply((size_t(width) + block-1)/block, bytes), (size_t(height) + block-1)/block, depth, width,height,block,bytes};
}
Extent resource_extent(uint64_t guest, unsigned subresource) {
    Native<ID3D11Resource> resource(guest);
    D3D11_RESOURCE_DIMENSION dimension; resource->GetType(&dimension);
    unsigned mip = 0;
    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        Native<ID3D11Buffer> p(guest);
        if (!p.p || subresource) trap("invalid buffer/subresource");
        D3D11_BUFFER_DESC d; p->GetDesc(&d); return {d.ByteWidth, 1, 1,d.ByteWidth,1,1,1};
    }
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        Native<ID3D11Texture2D> p(guest);
        if (!p.p) trap("texture2D query failed");
        D3D11_TEXTURE2D_DESC d; p->GetDesc(&d);
        if (!d.MipLevels || subresource >= uint64_t(d.MipLevels)*d.ArraySize) trap("texture2D subresource out of range");
        mip = subresource%d.MipLevels;
        return extent(d.Format, std::max(1u, d.Width>>mip), std::max(1u, d.Height>>mip), 1);
    }
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        Native<ID3D11Texture1D> p(guest);
        if (!p.p) trap("texture1D query failed");
        D3D11_TEXTURE1D_DESC d; p->GetDesc(&d);
        if (!d.MipLevels || subresource >= uint64_t(d.MipLevels)*d.ArraySize) trap("texture1D subresource out of range");
        mip = subresource%d.MipLevels;
        return extent(d.Format, std::max(1u,d.Width>>mip), 1, 1);
    }
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        Native<ID3D11Texture3D> p(guest);
        if (!p.p) trap("texture3D query failed");
        D3D11_TEXTURE3D_DESC d; p->GetDesc(&d);
        if (subresource >= d.MipLevels) trap("texture3D subresource out of range");
        return extent(d.Format, std::max(1u,d.Width>>subresource), std::max(1u,d.Height>>subresource), std::max(1u,d.Depth>>subresource));
    }
    trap("unknown resource dimension");
}
size_t pitched_bytes(Extent e, size_t row_pitch, size_t slice_pitch) {
    if (!e.rows || !e.depth || row_pitch < e.row_bytes) trap("invalid resource pitch");
    size_t plane = multiply(row_pitch, e.rows-1);
    if (plane>SIZE_MAX-e.row_bytes) trap("resource plane overflow");
    plane += e.row_bytes;
    if (e.depth > 1 && slice_pitch < plane) trap("invalid resource slice pitch");
    size_t result = multiply(slice_pitch, e.depth-1);
    if (result > SIZE_MAX-plane) trap("resource size overflow");
    return result + plane;
}
void validate_initial_data(const D3D11_SUBRESOURCE_DATA* data, size_t count, const std::vector<Extent>& extents) {
    if (!data) return;
    for (size_t i = 0; i < count; ++i) {
        Extent e = extents.at(i);
        size_t row = e.rows == 1 ? e.row_bytes : data[i].SysMemPitch;
        win64_pointer(reinterpret_cast<uintptr_t>(data[i].pSysMem), pitched_bytes(e, row, data[i].SysMemSlicePitch), 0);
    }
}
unsigned full_mips(unsigned width, unsigned height = 1, unsigned depth = 1) {
    unsigned n = std::max(width, std::max(height,depth)), result = 0;
    while (n) { ++result; n >>= 1; }
    return result;
}
void validate_texture_data(CPU* c, unsigned dimension) {
    if (!arg(c,2)) return;
    unsigned levels, layers = 1, width, height = 1, depth = 1; DXGI_FORMAT format;
    if (dimension == 1) { auto& d = ref<const D3D11_TEXTURE1D_DESC>(arg(c,1)); levels=d.MipLevels; layers=d.ArraySize; width=d.Width; format=d.Format; }
    else if (dimension == 2) { auto& d = ref<const D3D11_TEXTURE2D_DESC>(arg(c,1)); levels=d.MipLevels; layers=d.ArraySize; width=d.Width; height=d.Height; format=d.Format; }
    else { auto& d = ref<const D3D11_TEXTURE3D_DESC>(arg(c,1)); levels=d.MipLevels; width=d.Width; height=d.Height; depth=d.Depth; format=d.Format; }
    if (!width || !height || !depth || !layers) trap("invalid texture dimensions");
    unsigned maximum = full_mips(width,height,depth);
    if (!levels) levels = maximum;
    if (levels > maximum || layers > 2048) trap("invalid texture mip/array count");
    size_t count = multiply(levels,layers);
    auto* data = ptr<const D3D11_SUBRESOURCE_DATA>(arg(c,2),count);
    std::vector<Extent> extents;
    for (unsigned layer=0; layer<layers; ++layer) for (unsigned mip=0; mip<levels; ++mip)
        extents.push_back(extent(format,std::max(1u,width>>mip),std::max(1u,height>>mip),std::max(1u,depth>>mip)));
    validate_initial_data(data,count,extents);
}
struct Mapping { IUnknown* identity; unsigned subresource; uint64_t pointer; };
std::vector<Mapping> mappings;
/* Tiny staging readbacks (the 1x1 auto-exposure luminance) are copied and
 * mapped for READ in the same frame, which drains the GPU mid-frame. Copies go
 * round-robin into private staging textures and Map returns the copy issued
 * two frames earlier, which the GPU has normally finished. The first copies
 * keep the exact wait. */
constexpr unsigned READBACK_RING = 3;
struct Readback {
    ID3D11Texture2D* ring[READBACK_RING] = {};
    D3D11_TEXTURE2D_DESC desc = {};
    unsigned next = 0, issued = 0;
    int mapped = -1;
};
std::unordered_map<IUnknown*, Readback> readbacks;
std::atomic<int> readbacks_mapped{0};
bool tiny_readback(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC* out) {
    D3D11_RESOURCE_DIMENSION dim; resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) || !texture) return false;
    D3D11_TEXTURE2D_DESC d = {}; texture->GetDesc(&d); texture->Release();
    if (out) *out = d;
    return d.Usage == D3D11_USAGE_STAGING && (d.CPUAccessFlags & D3D11_CPU_ACCESS_READ) && d.Width * d.Height <= 16
        && d.MipLevels == 1 && d.ArraySize == 1 && d.SampleDesc.Count == 1;
}
bool readback_copy(CPU* c) {
    Native<ID3D11DeviceContext> context(arg(c,0));
    Native<ID3D11Resource> dst(arg(c,1)), src(arg(c,2));
    D3D11_TEXTURE2D_DESC d;
    if (!context.p || !dst.p || !src.p || !tiny_readback(dst.p, &d)) return false;
    Readback* r;
    { std::lock_guard lock(mutex); r = &readbacks[proxy(arg(c,1)).identity]; }
    if (r->ring[0] && memcmp(&r->desc, &d, sizeof(d))) {
        if (r->mapped >= 0) return false;
        for (auto& slot : r->ring) { slot->Release(); slot = nullptr; }
        r->next = r->issued = 0;
    }
    if (!r->ring[0]) {
        r->desc = d;
        ID3D11Device* device = nullptr; context->GetDevice(&device);
        if (!device) return false;
        for (auto& slot : r->ring)
            if (FAILED(device->CreateTexture2D(&d, nullptr, &slot))) { device->Release(); trap("readback ring allocation"); }
        device->Release();
    }
    context->CopyResource(r->ring[r->next], src.p);
    r->next = (r->next + 1) % READBACK_RING; ++r->issued;
    return true;
}
bool readback_map(CPU* c) {
    if (uint32_t(arg(c,3)) != D3D11_MAP_READ) return false;
    Readback* r;
    { std::lock_guard lock(mutex); auto it = readbacks.find(proxy(arg(c,1)).identity); if (it == readbacks.end()) return false; r = &it->second; }
    if (!r->issued || r->mapped >= 0 || arg(c,2)) return false;
    Native<ID3D11DeviceContext> context(arg(c,0));
    unsigned index = r->issued >= READBACK_RING ? r->next : (r->next + READBACK_RING - 1) % READBACK_RING;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(r->ring[index], 0, D3D11_MAP_READ, uint32_t(arg(c,4)), &mapped);
    c->rax = uint32_t(hr);
    if (SUCCEEDED(hr)) { r->mapped = int(index); ++readbacks_mapped; ref<D3D11_MAPPED_SUBRESOURCE>(arg(c,5)) = mapped; }
    return true;
}
bool readback_unmap(CPU* c) {
    if (!readbacks_mapped.load(std::memory_order_relaxed)) return false;
    Readback* r;
    { std::lock_guard lock(mutex); auto it = readbacks.find(proxy(arg(c,1)).identity); if (it == readbacks.end() || it->second.mapped < 0) return false; r = &it->second; }
    Native<ID3D11DeviceContext> context(arg(c,0));
    context->Unmap(r->ring[r->mapped], 0); r->mapped = -1; --readbacks_mapped;
    return true;
}
void map_resource(CPU* c) {
    if (readback_map(c)) return;
    Native<ID3D11Resource> resource(arg(c,1));
    if (!resource.p) trap("Map null resource");
    invoke(c,&ID3D11DeviceContext::Map);
    if (SUCCEEDED(HRESULT(c->rax))) {
        auto& result = ref<D3D11_MAPPED_SUBRESOURCE>(arg(c,5));
        if (!result.pData) trap("Map succeeded without data");
        Proxy& record=proxy(arg(c,1));
        unsigned subresource=unsigned(arg(c,2));
        /* A buffer has one row of ByteWidth bytes: nothing to check about its pitches.  This is
         * the path of every per-draw constant and vertex upload. */
        if(record.kind==Kind::ID3D11Buffer && !subresource && record.buffer_maps->add())return;
        Extent e = resource_extent(arg(c,1),unsigned(arg(c,2)));
        size_t row = e.rows==1 ? e.row_bytes : result.RowPitch;
        size_t bytes = pitched_bytes(e,row,result.DepthPitch);
        uint64_t address = reinterpret_cast<uintptr_t>(result.pData);
        (void)bytes;
        std::lock_guard lock(mutex);
        mappings.push_back({record.identity,subresource,address});
    }
}
void unmap_resource(CPU* c) {
    if (readback_unmap(c)) return;
    Native<ID3D11Resource> resource(arg(c,1));
    Proxy& record=proxy(arg(c,1));
    unsigned subresource=unsigned(arg(c,2));
    bool found=!subresource && record.buffer_maps->remove();
    if(!found) {
        std::lock_guard lock(mutex);
        IUnknown* identity=record.identity;
        for (auto it=mappings.begin();it!=mappings.end();++it) if (it->identity==identity && it->subresource==subresource) {
            mappings.erase(it); found=true; break;
        }
    }
    if (!found) trap("Unmap without a successful tracked Map");
    invoke(c,&ID3D11DeviceContext::Unmap);
}
void update_resource(CPU* c) {
    Native<ID3D11Resource> resource(arg(c,1));
    if (!resource.p) trap("UpdateSubresource null resource");
    Extent e = resource_extent(arg(c,1),unsigned(arg(c,2)));
    if (arg(c,3)) {
        const auto& box=ref<const D3D11_BOX>(arg(c,3));
        if (box.left>e.width || box.right>e.width || box.top>e.height || box.bottom>e.height || box.front>e.depth || box.back>e.depth)
            trap("UpdateSubresource box outside resource");
        if (box.left>=box.right || box.top>=box.bottom || box.front>=box.back) {
            invoke(c,&ID3D11DeviceContext::UpdateSubresource,counts({{4,0}})); return;
        }
        if (box.left%e.block || box.top%e.block || (box.right%e.block && box.right!=e.width) || (box.bottom%e.block && box.bottom!=e.height))
            trap("UpdateSubresource compressed box alignment");
        e.row_bytes=multiply((box.right-box.left+e.block-1)/e.block,e.block_bytes);
        e.rows=(box.bottom-box.top+e.block-1)/e.block; e.depth=box.back-box.front;
    }
    // Win64 UINT stack arguments leave their upper slot bytes unspecified.
    size_t row = e.rows==1 ? e.row_bytes : uint32_t(arg(c,5));
    size_t bytes = pitched_bytes(e,row,uint32_t(arg(c,6)));
    invoke(c,&ID3D11DeviceContext::UpdateSubresource,counts({{4,bytes}}));
}
unsigned context_slot(const char* name) {
    unsigned k=unsigned(Kind::ID3D11DeviceContext);
    for(unsigned i=0;i<slot_counts[k];++i)if(!strcmp(slot_names[k][i],name))return i;
    trap("unknown ID3D11DeviceContext method name");
}
#include "mobile_geometry.inc"
/* Measurement only (debug.fp.degenerate=N, picture is wrong on purpose): in every static 16-bit
 * index buffer of 1000 triangles or more only one triangle in N keeps its indices, the others
 * become degenerate (a,a,a).  Together with debug.fp.knockout this separates the cost a tile-based
 * GPU pays for shading vertices from the cost of walking primitives that are rejected at once. */
unsigned buffer_degenerate() {
    static const unsigned keep=[]{char value[PROP_VALUE_MAX]={0};
        return __system_property_get("debug.fp.degenerate",value)>0?unsigned(strtoul(value,nullptr,0)):0u;}();
    return keep;
}
bool buffer_experiment(CPU* c,const D3D11_BUFFER_DESC* desc,const D3D11_SUBRESOURCE_DATA* data) {
    unsigned keep=buffer_degenerate();
    if(keep<2||!(desc->BindFlags&D3D11_BIND_INDEX_BUFFER)||desc->Usage==D3D11_USAGE_DYNAMIC||desc->ByteWidth<6000||desc->ByteWidth%2)return false;
    uint16_t* indices=static_cast<uint16_t*>(win64_pointer(reinterpret_cast<uintptr_t>(data->pSysMem),desc->ByteWidth,1));
    size_t triangles=desc->ByteWidth/6;
    std::vector<uint16_t> saved(indices,indices+triangles*3);
    for(size_t t=0;t<triangles;++t)if(t%keep){indices[t*3+1]=indices[t*3];indices[t*3+2]=indices[t*3];}
    invoke(c,&ID3D11Device::CreateBuffer);
    memcpy(indices,saved.data(),saved.size()*2);
    return true;
}
void create_resource(CPU* c, unsigned slot) {
    switch (slot) {
    case 3: {
        auto* desc = ptr<const D3D11_BUFFER_DESC>(arg(c,1));
        auto* data = ptr<const D3D11_SUBRESOURCE_DATA>(arg(c,2));
        if (data) {
            if (!desc || !desc->ByteWidth) trap("invalid buffer upload description");
            win64_pointer(reinterpret_cast<uintptr_t>(data->pSysMem),desc->ByteWidth,0);
        }
        if (data && buffer_experiment(c,desc,data)) break;
        geometry::BufferCreation creation(desc,data);
        invoke(c,&ID3D11Device::CreateBuffer);
        creation.created(c,desc,data); break;
    }
    case 4: validate_texture_data(c,1); invoke(c,&ID3D11Device::CreateTexture1D); break;
    case 5: validate_texture_data(c,2); invoke(c,&ID3D11Device::CreateTexture2D); break;
    case 6: validate_texture_data(c,3); invoke(c,&ID3D11Device::CreateTexture3D); break;
    case 11: {
        size_t count = uint32_t(arg(c,2));
        auto* desc = ptr<const D3D11_INPUT_ELEMENT_DESC>(arg(c,1),count);
        if (count && !desc) trap("null input layout descriptors");
        for (size_t i=0; i<count; ++i) if (desc[i].SemanticName) win64_string_a(reinterpret_cast<uintptr_t>(desc[i].SemanticName));
        invoke(c,&ID3D11Device::CreateInputLayout,counts({{1,count},{3,arg(c,4)}}));
        geometry::layout_created(c,desc,count); break;
    }
    case 14: {
        size_t count = uint32_t(arg(c,4));
        auto* desc = ptr<const D3D11_SO_DECLARATION_ENTRY>(arg(c,3),count);
        if (count && !desc) trap("null stream output descriptors");
        for (size_t i=0; i<count; ++i) if (desc[i].SemanticName) win64_string_a(reinterpret_cast<uintptr_t>(desc[i].SemanticName));
        invoke(c,&ID3D11Device::CreateGeometryShaderWithStreamOutput,counts({{1,arg(c,2)},{3,count},{5,uint32_t(arg(c,6))}})); break;
    }
    default: trap("unknown resource creation slot");
    }
}
struct GuestAdapterDesc {
    uint16_t description[128]; uint32_t vendor,device,subsystem,revision;
    uint64_t video,system,shared; LUID luid;
};
struct GuestAdapterDesc1 { GuestAdapterDesc base; uint32_t flags; uint32_t padding; };
struct GuestOutputDesc { uint16_t name[32]; RECT coordinates; BOOL attached; DXGI_MODE_ROTATION rotation; uint64_t monitor; };
static_assert(sizeof(GuestAdapterDesc)==304 && sizeof(GuestAdapterDesc1)==312 && sizeof(GuestOutputDesc)==96);
template<size_t N> void utf16(uint16_t (&out)[N], const WCHAR* in) {
    size_t n=0;
    while (*in && n+1<N) {
        uint32_t ch=uint32_t(*in++);
        if (ch<=0xffff) out[n++]=uint16_t(ch);
        else if (ch<=0x10ffff && n+2<N) { ch-=0x10000; out[n++]=uint16_t(0xd800+(ch>>10)); out[n++]=uint16_t(0xdc00+(ch&1023)); }
        else out[n++]=0xfffd;
    }
    out[n]=0;
}
template<class D> void adapter_desc(GuestAdapterDesc& out, const D& in) {
    utf16(out.description,in.Description);
    out.vendor=in.VendorId; out.device=in.DeviceId; out.subsystem=in.SubSysId; out.revision=in.Revision;
    out.video=in.DedicatedVideoMemory; out.system=in.DedicatedSystemMemory; out.shared=in.SharedSystemMemory; out.luid=in.AdapterLuid;
}
struct WindowBinding {uint64_t guest;std::unique_ptr<WindowLease> lease;};
std::unordered_map<IUnknown*, WindowBinding> swapchain_windows;
bool identity_has_mapping(IUnknown* identity) {
    auto owner=identities.find(identity);
    return (owner!=identities.end() && owner->second.buffer_maps.any()) ||
        std::any_of(mappings.begin(),mappings.end(),[&](const Mapping& m){return m.identity==identity;});
}
std::unique_ptr<WindowLease> identity_cleanup(IUnknown* identity) {
    geometry::forget(identity);
    auto it=swapchain_windows.find(identity);
    if(it==swapchain_windows.end())return {};
    auto lease=std::move(it->second.lease);swapchain_windows.erase(it);return lease;
}
/* The native context can retain an object after every guest ref is released.
 * A marker stored on the native object preserves canonical guest addresses
 * until the actual native lifetime ends. It holds no reference back to the
 * object, so it cannot create a resource/device reference cycle. */
class LifetimeMarker final : public IUnknown {
    std::atomic<ULONG> refs{1};
    IUnknown* identity;
public:
    explicit LifetimeMarker(IUnknown* key):identity(key) {}
    HRESULT QueryInterface(REFIID iid,void** result) override {
        if (!result) return E_POINTER;
        *result=nullptr;
        if (!(iid==__uuidof(IUnknown))) return E_NOINTERFACE;
        *result=static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override {
        ULONG remaining=--refs;
        if (!remaining) {
            std::unique_ptr<WindowLease> window_lease;
            {
                std::lock_guard lock(mutex);
                auto it=identities.find(identity);
                if (it!=identities.end()) {
                    if (it->second.refs || identity_has_mapping(identity)) trap("native destruction with live guest COM references/maps");
                    window_lease=identity_cleanup(identity);
                    for (uint64_t p:it->second.proxies) { win64_free(p); proxies.erase(p); }
                    identities.erase(it);
                }
            }
            /* Host callbacks never run under the bridge metadata mutex. */
            window_lease.reset();
            delete this;
        }
        return remaining;
    }
};
void attach_lifetime_marker(IUnknown* object,IUnknown* identity) {
    auto* marker=new LifetimeMarker(identity);
    HRESULT hr=E_NOINTERFACE;
    ID3D11Device* device=nullptr;
    ID3D11DeviceChild* child=nullptr;
    IDXGIObject* dxgi=nullptr;
    if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11Device),reinterpret_cast<void**>(&device)))) {
        hr=device->SetPrivateDataInterface(LIFETIME_KEY,marker); device->Release();
    } else if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11DeviceChild),reinterpret_cast<void**>(&child)))) {
        hr=child->SetPrivateDataInterface(LIFETIME_KEY,marker); child->Release();
    } else if (SUCCEEDED(object->QueryInterface(__uuidof(IDXGIObject),reinterpret_cast<void**>(&dxgi)))) {
        hr=dxgi->SetPrivateDataInterface(LIFETIME_KEY,marker); dxgi->Release();
    }
    if (FAILED(hr)) trap("native COM object lacks required lifetime attachment");
    marker->Release();
}
uint64_t associated_window = 0;
void dxgi_description(CPU* c, Kind kind, bool version1 = false) {
    HRESULT hr;
    if (kind==Kind::IDXGIOutput) {
        Native<IDXGIOutput> object(arg(c,0)); DXGI_OUTPUT_DESC desc = {};
        hr=object->GetDesc(&desc);
        if (SUCCEEDED(hr)) { GuestOutputDesc out={}; utf16(out.name,desc.DeviceName); out.coordinates=desc.DesktopCoordinates;
            out.attached=desc.AttachedToDesktop; out.rotation=desc.Rotation; out.monitor=reinterpret_cast<uintptr_t>(desc.Monitor);
            memcpy(win64_pointer(arg(c,1),sizeof(out),1),&out,sizeof(out)); }
    } else if (kind==Kind::IDXGISwapChain || kind==Kind::IDXGISwapChain1) {
        Native<IDXGISwapChain> object(arg(c,0)); DXGI_SWAP_CHAIN_DESC desc = {};
        hr=object->GetDesc(&desc);
        if (SUCCEEDED(hr)) { std::lock_guard lock(mutex); auto it=swapchain_windows.find(proxy(arg(c,0)).identity);
            if(it==swapchain_windows.end())trap("swapchain lacks its guest HWND binding");desc.OutputWindow=reinterpret_cast<HWND>(it->second.guest);
            memcpy(win64_pointer(arg(c,1),sizeof(desc),1),&desc,sizeof(desc)); }
    } else if (version1) {
        Native<IDXGIAdapter1> object(arg(c,0)); DXGI_ADAPTER_DESC1 desc={};
        hr=object->GetDesc1(&desc);
        if (SUCCEEDED(hr)) { GuestAdapterDesc1 out={}; adapter_desc(out.base,desc); out.flags=desc.Flags;
            memcpy(win64_pointer(arg(c,1),sizeof(out),1),&out,sizeof(out)); }
    } else {
        Native<IDXGIAdapter> object(arg(c,0)); DXGI_ADAPTER_DESC desc={};
        hr=object->GetDesc(&desc);
        if (SUCCEEDED(hr)) { GuestAdapterDesc out={}; adapter_desc(out,desc);
            memcpy(win64_pointer(arg(c,1),sizeof(out),1),&out,sizeof(out)); }
    }
    c->rax=uint32_t(hr);
}
template<class F> void iid_output(CPU* c, unsigned iid_arg, unsigned out_arg, F function) {
    const IID* iid=ptr<const IID>(arg(c,iid_arg));
    if (!iid) trap("null IID argument");
    write64(arg(c,out_arg),0);
    IUnknown* result=nullptr;
    HRESULT hr=function(*iid,reinterpret_cast<void**>(&result));
    if (SUCCEEDED(hr) && result) {
        Kind kind=kind_for_iid(*iid);
        if (kind==Kind::Count) { result->Release(); trap("successful native IID output lacks guest representation"); }
        write64(arg(c,out_arg),wrap(result,kind));
    }
    c->rax=uint32_t(hr);
}
void dxgi_parent(CPU* c) {
    Native<IDXGIObject> object(arg(c,0));
    iid_output(c,1,2,[&](REFIID iid,void** p) { return object->GetParent(iid,p); });
}
void dxgi_get_device(CPU* c) {
    Native<IDXGIDeviceSubObject> object(arg(c,0));
    iid_output(c,1,2,[&](REFIID iid,void** p) { return object->GetDevice(iid,p); });
}
void dxgi_get_buffer(CPU* c) {
    Native<IDXGISwapChain> object(arg(c,0));
    iid_output(c,2,3,[&](REFIID iid,void** p) {
        UINT index=unsigned(arg(c,1));HRESULT hr=object->GetBuffer(index,iid,p);
        if(SUCCEEDED(hr)&&*p)gfx_observe_backbuffer(static_cast<IUnknown*>(*p),index);
        return hr;
    });
}
void direct_window_release(void *p){ANativeWindow_release(static_cast<ANativeWindow *>(p));}
std::unique_ptr<WindowLease> resolve_window(uint64_t guest) {
    if(!guest)return {};
    win64_d3d11_window_acquire_fn acquire;
    win64_d3d11_window_release_fn release;
    {std::lock_guard lock(mutex);acquire=acquire_guest_window;release=release_guest_window;}
    uint64_t generation=0;ANativeWindow *native=nullptr;
    if(acquire)native=static_cast<ANativeWindow *>(acquire(guest,&generation));
    else{native=window.load(std::memory_order_acquire);if(native)ANativeWindow_acquire(native);release=direct_window_release;}
    if(!native)return {};
    return std::unique_ptr<WindowLease>(new WindowLease{native,guest,generation,release});
}
bool swapchain_desc(uint64_t address,DXGI_SWAP_CHAIN_DESC &desc,std::unique_ptr<WindowLease> &lease) {
    auto* p=ptr<const DXGI_SWAP_CHAIN_DESC>(address);
    if (!p) trap("null swapchain description");
    desc=*p;lease=resolve_window(reinterpret_cast<uintptr_t>(desc.OutputWindow));
    if(!lease)return false;
    desc.OutputWindow=lease->native;return true;
}
void dxgi_create_swapchain(CPU* c) {
    Native<IDXGIFactory> factory(arg(c,0)); Native<IUnknown> device(arg(c,1));
    write64(arg(c,3),0);
    DXGI_SWAP_CHAIN_DESC desc={};std::unique_ptr<WindowLease> lease;
    if(!swapchain_desc(arg(c,2),desc,lease)){c->rax=uint32_t(DXGI_ERROR_INVALID_CALL);return;}
    IDXGISwapChain* result=nullptr;
    HRESULT hr=factory->CreateSwapChain(device.p,&desc,&result);
    if (SUCCEEDED(hr) && result) {
        uint64_t guest=wrap(result); write64(arg(c,3),guest);
        std::lock_guard lock(mutex);
        uint64_t hwnd=lease->guest;swapchain_windows.emplace(proxy(guest).identity,WindowBinding{hwnd,std::move(lease)});
    }
    c->rax=uint32_t(hr);
}
static_assert(sizeof(DXGI_SWAP_CHAIN_DESC1)==48 && offsetof(DXGI_SWAP_CHAIN_DESC1,SampleDesc)==16
    && offsetof(DXGI_SWAP_CHAIN_DESC1,BufferUsage)==24 && offsetof(DXGI_SWAP_CHAIN_DESC1,Flags)==44,
    "Win64/native DXGI_SWAP_CHAIN_DESC1 layout");
static_assert(sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC)==20 && offsetof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC,Windowed)==16,
    "Win64/native DXGI_SWAP_CHAIN_FULLSCREEN_DESC layout");
void dxgi_create_swapchain_for_hwnd(CPU* c) {
    Native<IDXGIFactory2> factory(arg(c,0));
    if(!arg(c,6)){c->rax=uint32_t(DXGI_ERROR_INVALID_CALL);return;}
    write64(arg(c,6),0);
    if(!arg(c,1) || !arg(c,2) || !arg(c,3)){c->rax=uint32_t(DXGI_ERROR_INVALID_CALL);return;}
    Native<IUnknown> device(arg(c,1));
    auto* input=ptr<const DXGI_SWAP_CHAIN_DESC1>(arg(c,3));
    DXGI_SWAP_CHAIN_DESC1 desc=*input;
    auto* fullscreen=ptr<const DXGI_SWAP_CHAIN_FULLSCREEN_DESC>(arg(c,4));
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_copy={};
    if(fullscreen){fullscreen_copy=*fullscreen;fullscreen=&fullscreen_copy;}
    auto lease=resolve_window(arg(c,2));
    if(!lease){c->rax=uint32_t(DXGI_ERROR_INVALID_CALL);return;}
    Native<IDXGIOutput> restrict_output(arg(c,5));
    IDXGISwapChain1* result=nullptr;
    HRESULT hr=factory->CreateSwapChainForHwnd(device.p,lease->native,&desc,fullscreen,restrict_output.p,&result);
    if(SUCCEEDED(hr) && result){
        uint64_t guest=wrap(result);write64(arg(c,6),guest);
        std::lock_guard lock(mutex);uint64_t hwnd=lease->guest;
        swapchain_windows.emplace(proxy(guest).identity,WindowBinding{hwnd,std::move(lease)});
    }
    c->rax=uint32_t(hr);
}
void dxgi_window_association(CPU* c) {
    auto lease=arg(c,1)?resolve_window(arg(c,1)):nullptr;
    if(arg(c,1)&&!lease){c->rax=uint32_t(DXGI_ERROR_INVALID_CALL);return;}
    Native<IDXGIFactory> factory(arg(c,0));
    HRESULT hr=factory->MakeWindowAssociation(lease?lease->native:nullptr,unsigned(arg(c,2)));
    if (SUCCEEDED(hr)) { std::lock_guard lock(mutex); associated_window=arg(c,1); }
    c->rax=uint32_t(hr);
}
void dxgi_get_window_association(CPU* c) {
    Native<IDXGIFactory> factory(arg(c,0)); HWND native=nullptr;
    HRESULT hr=factory->GetWindowAssociation(&native);
    if (SUCCEEDED(hr)) { std::lock_guard lock(mutex); write64(arg(c,1),native ? associated_window : 0); }
    c->rax=uint32_t(hr);
}
using CreateDevice = HRESULT (*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,
                                ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
using CreateDeviceSwapchain = HRESULT (*)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,
    const DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
void* load_library(const char* name) {
    if (!provider.load(std::memory_order_acquire)) trap("D3D import before native provider configure");
    void* library=dlopen(name,RTLD_NOW|RTLD_LOCAL);
    if (!library) { fprintf(stderr,"DXVK dlopen: %s\n",dlerror()); trap("missing native DXVK shared library"); }
    return library;
}
void create_device(CPU* c, bool swapchain) {
    static void* lib=load_library("libdxvk_d3d11.so");
    Native<IDXGIAdapter> adapter(arg(c,0));
    auto type=static_cast<D3D_DRIVER_TYPE>(arg(c,1));
    if (type!=D3D_DRIVER_TYPE_HARDWARE && type!=D3D_DRIVER_TYPE_UNKNOWN) trap("only native hardware/explicit-adapter D3D driver accepted");
    if (arg(c,2)) trap("software rasterizer module unsupported");
    auto* levels=ptr<const D3D_FEATURE_LEVEL>(arg(c,4),uint32_t(arg(c,5)));
    ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr; IDXGISwapChain* chain=nullptr;
    D3D_FEATURE_LEVEL chosen=D3D_FEATURE_LEVEL();
    unsigned device_out=swapchain ? 9 : 7, feature_out=swapchain ? 10 : 8, context_out=swapchain ? 11 : 9;
    if (arg(c,device_out)) write64(arg(c,device_out),0);
    if (arg(c,context_out)) write64(arg(c,context_out),0);
    if (arg(c,feature_out)) *ptr<D3D_FEATURE_LEVEL>(arg(c,feature_out))=D3D_FEATURE_LEVEL();
    HRESULT hr;
    if (swapchain) {
        auto function=reinterpret_cast<CreateDeviceSwapchain>(dlsym(lib,"D3D11CreateDeviceAndSwapChain"));
        if (!function) trap("D3D11CreateDeviceAndSwapChain export absent");
        if (arg(c,8)) write64(arg(c,8),0);
        DXGI_SWAP_CHAIN_DESC desc={};std::unique_ptr<WindowLease> lease;
        if(!swapchain_desc(arg(c,7),desc,lease)){c->rax=uint32_t(DXGI_ERROR_INVALID_CALL);return;}
        hr=function(adapter.p,type,nullptr,unsigned(arg(c,3)),levels,unsigned(arg(c,5)),unsigned(arg(c,6)),
            &desc,arg(c,8) ? &chain : nullptr,arg(c,device_out) ? &device : nullptr,
            arg(c,feature_out) ? &chosen : nullptr,arg(c,context_out) ? &context : nullptr);
        if(chain){uint64_t guest=wrap(chain);write64(arg(c,8),guest);std::lock_guard lock(mutex);uint64_t hwnd=lease->guest;
            swapchain_windows.emplace(proxy(guest).identity,WindowBinding{hwnd,std::move(lease)});}
    } else {
        auto function=reinterpret_cast<CreateDevice>(dlsym(lib,"D3D11CreateDevice"));
        if (!function) trap("D3D11CreateDevice export absent");
        hr=function(adapter.p,type,nullptr,unsigned(arg(c,3)),levels,unsigned(arg(c,5)),unsigned(arg(c,6)),
            arg(c,device_out) ? &device : nullptr,arg(c,feature_out) ? &chosen : nullptr,arg(c,context_out) ? &context : nullptr);
    }
    if (device) write64(arg(c,device_out),wrap(device));
    if (context) write64(arg(c,context_out),wrap(context));
    if (arg(c,feature_out)) *ptr<D3D_FEATURE_LEVEL>(arg(c,feature_out))=chosen;
    c->rax=uint32_t(hr);
}
void create_factory(CPU* c, bool flags) {
    static void* lib=load_library("libdxvk_dxgi.so");
    using Function=HRESULT (*)(REFIID,void**);
    using Function2=HRESULT (*)(UINT,REFIID,void**);
    if (flags) {
        auto function=reinterpret_cast<Function2>(dlsym(lib,"CreateDXGIFactory2"));
        if (!function) trap("CreateDXGIFactory2 export absent");
        iid_output(c,1,2,[&](REFIID iid,void** p){ return function(unsigned(arg(c,0)),iid,p); });
    } else {
        auto function=reinterpret_cast<Function>(dlsym(lib,"CreateDXGIFactory1"));
        if (!function) trap("CreateDXGIFactory1 export absent");
        iid_output(c,0,1,[&](REFIID iid,void** p){ return function(iid,p); });
    }
}
} // namespace

extern "C" PFN_vkGetInstanceProcAddr frostpunk_dxvk_get_instance_proc_addr() { return provider.load(std::memory_order_acquire); }
extern "C" __attribute__((visibility("default"))) ANativeWindow* frostpunk_dxvk_acquire_window(void* original, uint64_t* generation) {
    uint64_t guest=0;
    win64_d3d11_window_acquire_fn acquire=nullptr;
    {
        std::lock_guard lock(mutex);
        for(const auto& entry:swapchain_windows)
            if(entry.second.lease->native==original){guest=entry.second.guest;break;}
        acquire=acquire_guest_window;
    }
    if(guest && acquire)return static_cast<ANativeWindow*>(acquire(guest,generation));
    // Initial CreateSurface runs before the swapchain's binding is registered.
    auto* native=static_cast<ANativeWindow*>(original);
    if(native)ANativeWindow_acquire(native);
    if(generation)*generation=0;
    return native;
}
extern "C" __attribute__((visibility("default"))) int frostpunk_dxvk_surface_state(uint64_t surface, void* original, uint64_t generation, unsigned action) {
    struct Binding {void* original;uint64_t generation;};
    static std::unordered_map<uint64_t,Binding> surfaces;
    Binding binding{};
    {
        std::lock_guard lock(mutex);
        if(action==0){surfaces[surface]={original,generation};return 0;}
        if(action==2){surfaces.erase(surface);return 0;}
        auto found=surfaces.find(surface);
        if(found==surfaces.end())return 0;
        binding=found->second;
    }
    uint64_t current_generation=0;
    ANativeWindow* native=frostpunk_dxvk_acquire_window(binding.original,&current_generation);
    if(native)ANativeWindow_release(native);
    static unsigned notices=0;
    if((!native || current_generation!=binding.generation) && notices++<12)
        fprintf(stderr,"FP_SURFACE_STALE surface=%llx original=%p stored=%llu current=%llu available=%d\n",
            (unsigned long long)surface,binding.original,(unsigned long long)binding.generation,
            (unsigned long long)current_generation,native!=nullptr);
    return !native || current_generation!=binding.generation;
}
extern "C" void win64_d3d11_set_display_mode(uint32_t width,uint32_t height,uint32_t refresh) {
    if(!width||!height||width>16384||height>16384||!refresh||refresh>1000)trap("invalid guest virtual display mode");
    uint64_t extent=(uint64_t(width)<<32)|height;
    std::lock_guard lock(mutex);
    display_extent.store(extent,std::memory_order_relaxed);display_refresh.store(refresh,std::memory_order_release);
}
extern "C" int frostpunk_dxvk_get_display_mode(uint32_t *width,uint32_t *height,uint32_t *refresh) {
    if(!width||!height||!refresh)return 0;
    uint32_t rate=display_refresh.load(std::memory_order_acquire);if(!rate)return 0;
    uint64_t extent=display_extent.load(std::memory_order_relaxed);
    *width=uint32_t(extent>>32);*height=uint32_t(extent);*refresh=rate;return 1;
}
extern "C" void win64_d3d11_set_window_resolver(win64_d3d11_window_acquire_fn acquire,win64_d3d11_window_release_fn release) {
    if(bool(acquire)!=bool(release))trap("incomplete HWND resolver callbacks");
    std::lock_guard lock(mutex);
    if(acquire_guest_window&&(acquire_guest_window!=acquire||release_guest_window!=release))trap("changing live HWND resolver callbacks");
    acquire_guest_window=acquire;release_guest_window=release;
}
extern "C" void win64_d3d11_configure(PFN_vkGetInstanceProcAddr get, void* native_window) {
    std::lock_guard lock(mutex);
    if (!get) trap("missing native Vulkan provider");
    if (provider.load(std::memory_order_relaxed) && provider.load(std::memory_order_relaxed) != get) trap("changing Vulkan provider after configure");
    provider.store(get,std::memory_order_release); window.store(static_cast<ANativeWindow*>(native_window),std::memory_order_release);
    setenv("DXVK_WSI_DRIVER","Android",1);
    setenv("DXVK_LOG_PATH","none",1);
    { char value[PROP_VALUE_MAX]={0}; if(__system_property_get("debug.fp.dxvk_config",value)>0&&value[0])setenv("DXVK_CONFIG",value,1); }
}
extern "C" int win64_d3d11_import(CPU* c, const char* dll, const char* name) {
    if (!strcasecmp(dll,"d3d11.dll")) {
        if (!strcmp(name,"D3D11CreateDevice")) { create_device(c,false); return 1; }
        if (!strcmp(name,"D3D11CreateDeviceAndSwapChain")) { create_device(c,true); return 1; }
    } else if (!strcasecmp(dll,"dxgi.dll")) {
        if (!strcmp(name,"CreateDXGIFactory") || !strcmp(name,"CreateDXGIFactory1")) { create_factory(c,false); return 1; }
        if (!strcmp(name,"CreateDXGIFactory2")) { create_factory(c,true); return 1; }
    }
    return 0;
}
extern "C" void win64_d3d11_save_photo(CPU* c) {
    GfxDiagnosticErrors errors;
    uint32_t saved_error=win64_get_error();
    HRESULT hr=E_INVALIDARG;
    if(c->rcx && c->rdx && c->r9) {
        Native<ID3D11DeviceContext> context(c->rcx);
        Native<ID3D11Texture2D> source(c->rdx);
        D3D11_TEXTURE2D_DESC desc;source->GetDesc(&desc);
        bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        bool rgba=desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        hr=HRESULT(0x80070032u);
        if((bgra||rgba) && desc.Width && desc.Height && desc.Width<=16384 && desc.Height<=16384 && desc.SampleDesc.Count==1) {
            ID3D11Device* device=nullptr;source->GetDevice(&device);
            ID3D11Texture2D* staging=nullptr;
            desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
            hr=device->CreateTexture2D(&desc,nullptr,&staging);
            if(SUCCEEDED(hr)) {
                context->CopyResource(staging,source.p);
                D3D11_MAPPED_SUBRESOURCE mapped={};hr=context->Map(staging,0,D3D11_MAP_READ,0,&mapped);
                if(SUCCEEDED(hr)) {
                    std::vector<uint8_t> pixels(size_t(desc.Width)*desc.Height*4);
                    for(unsigned y=0;y<desc.Height;++y) {
                        auto* row=pixels.data()+size_t(y)*desc.Width*4;
                        memcpy(row,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(desc.Width)*4);
                        if(bgra)for(unsigned x=0;x<desc.Width;++x)std::swap(row[x*4],row[x*4+2]);
                    }
                    context->Unmap(staging,0);
                    int fd=-1;void* token=nullptr;
                    int error=win64_kernel_stream_open(c->r9,1,1,&fd,&token);
                    hr=HRESULT(0x80070000u|unsigned(error));
                    if(!error) {
                        char link[64],saved[4096]={};
                        snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
                        ssize_t path_size=readlink(link,saved,sizeof(saved)-1);
                        if(path_size>0)saved[path_size]=0;
                        FILE* file=fdopen(fd,"wb");
                        hr=E_FAIL;
                        if(file) {
                            auto write=[](void* out,void* data,int size){fwrite(data,1,size,static_cast<FILE*>(out));};
                            int encoded=stbi_write_png_to_func(write,file,desc.Width,desc.Height,4,pixels.data(),desc.Width*4);
                            bool ok=encoded && !ferror(file) && fflush(file)==0;
                            if(fclose(file)!=0)ok=false;
                            if(ok){hr=S_OK;if(frostpunk_android_photo_saved&&(path_size<=0||!frostpunk_android_photo_saved(saved)))hr=E_FAIL;}
                        } else close(fd);
                        win64_kernel_stream_close(token);
                    }
                }
                staging->Release();
            }
            device->Release();
        }
        fprintf(stderr,"FP_PHOTO_SAVED size=%ux%u format=%u status=%08x\n",desc.Width,desc.Height,unsigned(desc.Format),unsigned(hr));
    }
    win64_set_error(saved_error);c->rax=uint32_t(hr);
}
namespace {
/* Diagnostic, off unless debug.fp.drawstats=1 when the game starts: how much geometry every
 * render-target binding of a frame receives.  On a tile-based GPU the vertex side of a frame is
 * the sum over all passes that draw the scene again (shadow cascades, depth, G-buffer), and it
 * does not shrink with the output resolution; this shows which passes carry it.  One frame in
 * 600 is written to the log at Present: FP_DRAW_FRAME, then one FP_DRAW_PASS per binding. */
struct DrawPass {
    unsigned width=0,height=0,format=0,views=0,depth_width=0,depth_height=0;
    uint64_t draws=0,vertices=0,instances=0,patch_draws=0,indirect=0,clears=0;
};
struct DrawSlots {
    unsigned draw_indexed=~0u,draw=~0u,draw_indexed_instanced=~0u,draw_instanced=~0u,indexed_indirect=~0u,indirect=~0u,
        targets=~0u,targets_uav=~0u,topology=~0u,clear_depth=~0u,dispatch=~0u;
};
struct DrawMesh { uint64_t draws=0,instances=0; };
struct DrawFrame {
    std::vector<DrawPass> passes;
    /* Draws of the frame by index count of one instance: the meshes that carry the vertex load. */
    std::unordered_map<uint32_t,DrawMesh> meshes;
    uint64_t frame=0,dispatches=0;
    unsigned topology=0;
};
thread_local DrawFrame draw_frame;
/* Measurement only (debug.fp.knockout=N, picture is wrong on purpose): draws of more than N
 * indices per instance into a four-target binding (the G-buffer) are dropped, which shows how
 * much of the GPU frame is vertex work of the big meshes. */
unsigned draw_knockout() {
    static const unsigned limit=[]{char value[PROP_VALUE_MAX]={0};
        return __system_property_get("debug.fp.knockout",value)>0?unsigned(strtoul(value,nullptr,0)):0u;}();
    return limit;
}
int draw_stats_enabled() {
    static const int enabled=[]{char value[PROP_VALUE_MAX]={0};
        return (__system_property_get("debug.fp.drawstats",value)>0&&value[0]=='1')||draw_knockout();}();
    return enabled;
}
void draw_pass_begin(uint64_t count,uint64_t views,uint64_t depth) {
    DrawPass pass;
    if(count!=UINT32_MAX) {
        pass.views=unsigned(count);
        uint64_t first=count&&views?read64(views):0;
        if(first) {
            Native<ID3D11RenderTargetView> view(first);ID3D11Resource* resource=nullptr;view->GetResource(&resource);
            ID3D11Texture2D* texture=nullptr;
            if(resource&&SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))&&texture) {
                D3D11_TEXTURE2D_DESC d={};texture->GetDesc(&d);pass.width=d.Width;pass.height=d.Height;pass.format=unsigned(d.Format);texture->Release();
            }
            if(resource)resource->Release();
        }
        if(depth) {
            Native<ID3D11DepthStencilView> view(depth);ID3D11Resource* resource=nullptr;view->GetResource(&resource);
            ID3D11Texture2D* texture=nullptr;
            if(resource&&SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))&&texture) {
                D3D11_TEXTURE2D_DESC d={};texture->GetDesc(&d);pass.depth_width=d.Width;pass.depth_height=d.Height;texture->Release();
            }
            if(resource)resource->Release();
        }
    } else if(!draw_frame.passes.empty()) {
        /* D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL: the binding stays, only UAVs change. */
        DrawPass& last=draw_frame.passes.back();
        pass.width=last.width;pass.height=last.height;pass.format=last.format;pass.views=last.views;
        pass.depth_width=last.depth_width;pass.depth_height=last.depth_height;
    }
    draw_frame.passes.push_back(pass);
}
bool draw_stats(CPU* c,Kind kind,unsigned slot) {
    static const DrawSlots slots=[]{DrawSlots s;
        s.draw_indexed=context_slot("DrawIndexed");s.draw=context_slot("Draw");
        s.draw_indexed_instanced=context_slot("DrawIndexedInstanced");s.draw_instanced=context_slot("DrawInstanced");
        s.indexed_indirect=context_slot("DrawIndexedInstancedIndirect");s.indirect=context_slot("DrawInstancedIndirect");
        s.targets=context_slot("OMSetRenderTargets");s.targets_uav=context_slot("OMSetRenderTargetsAndUnorderedAccessViews");
        s.topology=context_slot("IASetPrimitiveTopology");s.clear_depth=context_slot("ClearDepthStencilView");
        s.dispatch=context_slot("Dispatch");return s;}();
    DrawFrame& f=draw_frame;
    if(kind==Kind::ID3D11DeviceContext) {
        if(slot==slots.targets||slot==slots.targets_uav){draw_pass_begin(uint32_t(arg(c,1)),arg(c,2),arg(c,3));return false;}
        if(slot==slots.topology){f.topology=unsigned(arg(c,1));return false;}
        if(slot==slots.dispatch){++f.dispatches;return false;}
        uint64_t vertices=0,instances=1;bool counted=true,indirect=false;
        if(slot==slots.draw_indexed||slot==slots.draw)vertices=uint32_t(arg(c,1));
        else if(slot==slots.draw_indexed_instanced||slot==slots.draw_instanced){instances=uint32_t(arg(c,2));vertices=uint64_t(uint32_t(arg(c,1)))*instances;}
        else if(slot==slots.indexed_indirect||slot==slots.indirect)indirect=true;
        else counted=false;
        if(counted||slot==slots.clear_depth) {
            if(f.passes.empty())f.passes.emplace_back();
            DrawPass& pass=f.passes.back();
            if(slot==slots.clear_depth){++pass.clears;return false;}
            if(draw_knockout()&&pass.views==4&&!indirect&&instances&&vertices/instances>draw_knockout())return true;
            ++pass.draws;pass.vertices+=vertices;pass.instances+=instances;pass.indirect+=indirect;
            if(!indirect&&instances){DrawMesh& mesh=f.meshes[uint32_t(vertices/instances)];++mesh.draws;mesh.instances+=instances;}
            /* D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST = 33: drawn through the tessellator. */
            if(f.topology>=33)++pass.patch_draws;
        }
        return false;
    }
    if((kind==Kind::IDXGISwapChain||kind==Kind::IDXGISwapChain1)&&slot==8&&!(uint32_t(arg(c,2))&DXGI_PRESENT_TEST)) {
        if(++f.frame%600==300) {
            uint64_t draws=0,vertices=0;unsigned used=0;
            for(const DrawPass& pass:f.passes){draws+=pass.draws;vertices+=pass.vertices;used+=pass.draws!=0;}
            fprintf(stderr,"FP_DRAW_FRAME frame=%llu bindings=%zu with_draws=%u draws=%llu vertices=%llu compute_dispatches=%llu\n",
                (unsigned long long)f.frame,f.passes.size(),used,(unsigned long long)draws,(unsigned long long)vertices,(unsigned long long)f.dispatches);
            unsigned index=0;
            for(const DrawPass& pass:f.passes) {
                if(pass.draws||pass.clears)
                    fprintf(stderr,"FP_DRAW_PASS frame=%llu index=%u target=%ux%u format=%u views=%u depth=%ux%u depth_clears=%llu draws=%llu vertices=%llu instances=%llu patch_draws=%llu indirect=%llu\n",
                        (unsigned long long)f.frame,index,pass.width,pass.height,pass.format,pass.views,pass.depth_width,pass.depth_height,
                        (unsigned long long)pass.clears,(unsigned long long)pass.draws,(unsigned long long)pass.vertices,
                        (unsigned long long)pass.instances,(unsigned long long)pass.patch_draws,(unsigned long long)pass.indirect);
                ++index;
            }
            std::vector<std::pair<uint64_t,uint32_t>> order;
            for(const auto& mesh:f.meshes)order.emplace_back(uint64_t(mesh.first)*mesh.second.instances,mesh.first);
            std::sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.first>b.first;});
            for(size_t i=0;i<order.size()&&i<40;++i) {
                const DrawMesh& mesh=f.meshes[order[i].second];
                fprintf(stderr,"FP_DRAW_MESH frame=%llu rank=%zu indices=%u draws=%llu instances=%llu vertices=%llu\n",(unsigned long long)f.frame,i,
                    order[i].second,(unsigned long long)mesh.draws,(unsigned long long)mesh.instances,(unsigned long long)order[i].first);
            }
        }
        f.passes.clear();f.meshes.clear();f.dispatches=0;
    }
    return false;
}
}
namespace {
/* Diagnostic (debug.fp.frames=1): where a long frame comes from, seen from the render thread.
 * interval = time between two Present calls; present = time inside the previous Present (DXVK,
 * frame latency, swap chain); pace = sleep of the frame limiter; the rest is the frame itself, of
 * which cpu is the render thread's own CPU time - a frame whose rest is much longer than cpu was
 * waiting for the main thread.  Frames over 19 ms are logged one by one, a histogram every 600. */
struct FrameTimer {
    uint64_t begin=0,present=0,pace=0,cpu=0,frame=0,histogram[6]={0},sum_present=0,sum_pace=0,sum_cpu=0,sum_interval=0;
    static uint64_t clock(clockid_t id){struct timespec t;clock_gettime(id,&t);return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec);}
    static bool enabled() {
        static const bool on=[]{char value[PROP_VALUE_MAX]={0};return __system_property_get("debug.fp.frames",value)>0&&value[0]=='1';}();
        return on;
    }
    /* Called when Present is entered, before the frame limiter. */
    void enter() {
        uint64_t now=clock(CLOCK_MONOTONIC),cpu_now=clock(CLOCK_THREAD_CPUTIME_ID);
        if(begin) {
            uint64_t interval=now-begin,used=cpu_now-cpu,rest=interval>present+pace?interval-present-pace:0;
            ++frame;sum_interval+=interval;sum_present+=present;sum_pace+=pace;sum_cpu+=used;
            unsigned bucket=interval<15000000?0:interval<17500000?1:interval<20000000?2:interval<25000000?3:interval<33400000?4:5;
            ++histogram[bucket];
            if(interval>=19000000&&frame>300)
                fprintf(stderr,"FP_FRAME_SPIKE frame=%llu interval_ms=%.1f present_ms=%.1f pace_ms=%.1f rest_ms=%.1f render_cpu_ms=%.1f\n",
                    (unsigned long long)frame,interval/1e6,present/1e6,pace/1e6,rest/1e6,used/1e6);
            if(frame%600==0) {
                fprintf(stderr,"FP_FRAME_TIMES frames=%llu mean_interval_ms=%.2f mean_present_ms=%.2f mean_pace_ms=%.2f mean_render_cpu_ms=%.2f "
                    "under15=%llu to17_5=%llu to20=%llu to25=%llu to33=%llu over33=%llu\n",(unsigned long long)frame,sum_interval/6e8,sum_present/6e8,
                    sum_pace/6e8,sum_cpu/6e8,(unsigned long long)histogram[0],(unsigned long long)histogram[1],(unsigned long long)histogram[2],
                    (unsigned long long)histogram[3],(unsigned long long)histogram[4],(unsigned long long)histogram[5]);
                for(uint64_t& h:histogram)h=0;
                sum_interval=sum_present=sum_pace=sum_cpu=0;
            }
        }
        begin=now;cpu=cpu_now;present=pace=0;
    }
};
thread_local FrameTimer frame_timer;
struct PresentScope {
    uint64_t start=0;
    explicit PresentScope(bool active){if(active)start=FrameTimer::clock(CLOCK_MONOTONIC);}
    ~PresentScope(){if(start)frame_timer.present=FrameTimer::clock(CLOCK_MONOTONIC)-start;}
};
}
extern "C" int win64_d3d11_dispatch(CPU* c, uint64_t target) {
    if (target<SENTINEL || target>=SENTINEL+unsigned(Kind::Count)*STRIDE*8 || (target-SENTINEL)%8) return 0;
    unsigned number=unsigned((target-SENTINEL)/8), slot=number%STRIDE;
    Kind kind=static_cast<Kind>(number/STRIDE);
    { Proxy& object=proxy(arg(c,0));
      if (kind!=object.kind || slot>=slot_counts[unsigned(kind)]) trap("sentinel/vtable kind mismatch",kind,slot); }
    if (geometry::enabled()) {
        if (kind>=Kind::ID3D11DeviceContext1&&kind<=Kind::ID3D11DeviceContext4) geometry::context_call(c,slot);
        else if (kind==Kind::ID3D11DeviceContext) geometry::context_call(c,slot);
        else if ((kind==Kind::IDXGISwapChain||kind==Kind::IDXGISwapChain1)&&slot==8&&!(uint32_t(arg(c,2))&DXGI_PRESENT_TEST)) geometry::present();
    }
    if (__builtin_expect(draw_stats_enabled(),0) && draw_stats(c,kind,slot)) return 1;
    if (slot<3) { unknown(c,slot); return 1; }
    static const unsigned copy_slot=[]{unsigned k=unsigned(Kind::ID3D11DeviceContext);
        for(unsigned i=0;i<slot_counts[k];++i)if(!strcmp(slot_names[k][i],"CopyResource"))return i;return ~0u;}();
    if (kind==Kind::ID3D11DeviceContext && slot==copy_slot && readback_copy(c)) return 1;
    bool presenting=(kind==Kind::IDXGISwapChain||kind==Kind::IDXGISwapChain1)&&slot==8&&!(uint32_t(arg(c,2))&DXGI_PRESENT_TEST);
    bool timed=presenting&&FrameTimer::enabled();
    if (presenting) {
        if (timed) frame_timer.enter();
        if (frostpunk_thread_placement_frame) frostpunk_thread_placement_frame();
        uint64_t before=timed?FrameTimer::clock(CLOCK_MONOTONIC):0;
        if (frostpunk_android_frame_pace) frostpunk_android_frame_pace();
        if (timed) frame_timer.pace=FrameTimer::clock(CLOCK_MONOTONIC)-before;
    }
    PresentScope present_scope(timed);
    switch (kind) {
#include "bridge_dispatch.inc"
    case Kind::Count: break;
    }
    trap("unimplemented COM method",kind,slot);
}
