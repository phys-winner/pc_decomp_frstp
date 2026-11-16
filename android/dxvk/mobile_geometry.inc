/* Mobile geometry: simplified index data for static meshes, part of android/dxvk/bridge.cpp.
 *
 * A developed city draws about 8 million indices per frame into the G-buffer, almost all of them in
 * a few hundred building meshes that carry one level of detail on PC.  A tile-based GPU shades and
 * bins every vertex whatever the output size, so at a phone's resolution the frame is vertex work.
 * The Nintendo Switch release solves this with hand-made meshes of about a quarter of the triangles;
 * this module derives such index data from the PC meshes while the game runs.
 *
 * Nothing of the vertex data changes.  For a triangle-list range that the game draws from a static
 * index buffer, a worker thread runs meshoptimizer (absolute error bound, attribute-aware, small
 * loose parts pruned) over the positions, normals and texture coordinates of the vertices the range
 * uses.  The result is written into the same range of the same index buffer - simplified triangles
 * first, the rest of the range filled with degenerate triangles - and later draws of that exact
 * range ask for the shorter count.  Any other draw that reads these indices (a bigger range around
 * it, an indirect draw) still gets a valid picture.
 *
 * Safety rules:
 *  - a range is an "atom": no two atoms overlap.  A draw that cuts through an atom restores the
 *    original indices of that atom at once and the atom is given up;
 *  - a simplified range is tied to the vertex buffer, stride, base vertex and input layout it was
 *    computed from; a draw of the same indices with other vertices restores the original;
 *  - dynamic buffers, other topologies, other position formats and small ranges are left alone.
 *
 * The error bound is ERROR_PIXELS / (height of the back buffer) in object units (the models are in
 * metres): about one output pixel at the closest zoom.  The graphics row "Model detail" picks the
 * numerator live (0 auto, 1 full, 2..5 coarser).  Auto is 100 with mobile post-processing unless the
 * buildings group is PC, and 0 otherwise.  debug.fp.geometry=N overrides Auto only.
 * A change restores original indices and queues the ranges again, so the triangle count moves
 * without a restart.  debug.fp.geometry is read once per process.
 */
namespace geometry {

constexpr unsigned MIN_INDICES=600;              /* 200 triangles: smaller ranges are not worth a job */
constexpr size_t MAX_COPY_BYTES=size_t(48)<<20;  /* one buffer */
constexpr size_t COPY_BUDGET=size_t(640)<<20;    /* all retained copies of static buffers */
constexpr float DEFAULT_ERROR_PIXELS=100.0f;

struct Element { bool present=false; unsigned slot=0,offset=0; DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; };
struct Layout { Element position,normal,uv,bones; };
struct Copy { std::vector<uint8_t> bytes; };
struct Source {
    IUnknown* buffer=nullptr; IUnknown* layout=nullptr; unsigned offset=0,stride=0; int base=0;
    bool operator==(const Source& o) const { return buffer==o.buffer&&layout==o.layout&&offset==o.offset&&stride==o.stride&&base==o.base; }
};
struct Atom {
    enum State { Pending, Ready, Skip } state=Skip;
    uint32_t first=0,count=0,reduced=0;
    uint64_t ticket=0;
    Source source;
    std::vector<uint8_t> original;               /* index bytes of the range before the rewrite */
};
struct IndexBuffer {
    std::shared_ptr<Copy> copy;
    std::map<uint32_t,Atom> atoms;               /* by first index */
    bool frozen=false;                           /* read by a draw whose range is unknown: never rewritten again */
};
struct Job {
    uint64_t ticket=0; IUnknown* buffer=nullptr; uint32_t first=0,count=0;
    std::vector<uint32_t> indices; std::shared_ptr<Copy> vertices; Source source; Layout layout; float error=0;
};
struct Result { uint64_t ticket=0; IUnknown* buffer=nullptr; uint32_t first=0; std::vector<uint32_t> indices; };
struct Bound { IUnknown* buffer=nullptr; unsigned stride=0,offset=0; };

std::mutex lock;                                 /* registries and atoms; never held across a DXVK call */
std::unordered_map<IUnknown*,std::shared_ptr<Copy>> vertex_copies;
std::unordered_map<IUnknown*,IndexBuffer> index_buffers;
std::unordered_map<IUnknown*,Layout> layouts;
size_t copy_bytes=0;
uint64_t next_ticket=1;
/* Input-assembler state of the immediate context as the game set it (render thread). */
IUnknown* state_context=nullptr;
IUnknown* state_layout=nullptr;
IUnknown* state_index=nullptr;
unsigned state_index_bytes=0,state_index_offset=0,state_topology=0;
Bound state_vertex[8];
/* Bound render targets: new ranges are only taken from world passes - the G-buffer (several colour
 * targets) or a depth-only shadow pass.  Interface templates (progress rings, gauges, book pages)
 * draw their meshes into one colour target; simplifying them turns circles into polygons. */
bool state_world=false;
bool foreign_context=false;
/* Worker. */
std::mutex queue_lock;
std::condition_variable queue_signal;
std::deque<Job> jobs;
std::deque<Result> results;
std::atomic<unsigned> results_ready{0};
std::atomic<unsigned> tracked_index_buffers{0};  /* index_buffers.size(), read without the lock */
bool worker_started=false;
/* Counters for the log line. */
std::atomic<uint64_t> stat_jobs{0},stat_ready{0},stat_skipped{0},stat_restored{0},stat_before{0},stat_after{0},stat_micros{0};
uint64_t stat_frame=0,frame_before=0,frame_after=0;

/* 0 auto, 1 full meshes, 2 high (25 px), 3 medium (100, the old default), 4 low (250), 5 lowest (500). */
std::atomic<int> detail_choice{0};
int draw_on=-1;
float applied_pixels=0;
bool detail_primed=false;

float property_pixels() {
    static const float value=[]{
        char text[PROP_VALUE_MAX]={0};
        return __system_property_get("debug.fp.geometry",text)>0?strtof(text,nullptr):-1.0f;
    }();
    return value;
}
float error_pixels() {
    int level=detail_choice.load(std::memory_order_relaxed);
    if(level>=1&&level<=5){
        static const float table[]={0.0f,0.0f,25.0f,100.0f,250.0f,500.0f};
        return table[level];
    }
    float forced=property_pixels();
    if(forced>=0.0f)return forced;
    /* Launcher group "buildings and objects" (FP_GFX_BUILDINGS = 1): PC keeps the original meshes. */
    if(frostpunk_mobile_graphics_pc&&frostpunk_mobile_graphics_pc(1))return 0.0f;
    return frostpunk_dxvk_mobile_effects&&frostpunk_dxvk_mobile_effects()?DEFAULT_ERROR_PIXELS:0.0f;
}
bool enabled() { return error_pixels()>0; }
bool draws_active() {
    if(draw_on<0)draw_on=error_pixels()>0?1:0;
    return draw_on>0;
}
float error_bound() {
    unsigned height=unsigned(display_extent.load(std::memory_order_relaxed)&0xffffffffu);
    if(!height)height=720;
    float bound=error_pixels()/float(height);
    return bound<0.01f?0.01f:bound>1.0f?1.0f:bound;
}

/* ---- vertex attribute decoding (only what the simplifier needs) ---- */
float half_float(uint16_t h) {
    uint32_t sign=uint32_t(h>>15)<<31,exponent=(h>>10)&31,mantissa=h&1023u,bits;
    if(!exponent) {
        if(!mantissa)bits=sign;
        else { int shift=0; while(!(mantissa&1024u)){mantissa<<=1;++shift;} bits=sign|uint32_t(113-shift)<<23|(mantissa&1023u)<<13; }
    } else if(exponent==31)bits=sign|0x7f800000u|mantissa<<13;
    else bits=sign|(exponent+112)<<23|mantissa<<13;
    float value;memcpy(&value,&bits,4);return value;
}
unsigned format_bytes(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT: return 12;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: return 4;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: return 1;
    default: return 0;
    }
}
/* Three components of a direction; false when the format is not one of the known ones. */
bool decode_normal(const uint8_t* p,DXGI_FORMAT format,float out[3]) {
    switch(format) {
    case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: memcpy(out,p,12); break;
    case DXGI_FORMAT_R16G16B16A16_SNORM: { int16_t v[3];memcpy(v,p,6);for(int i=0;i<3;++i)out[i]=float(v[i])/32767.0f; break; }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: { uint16_t v[3];memcpy(v,p,6);for(int i=0;i<3;++i)out[i]=half_float(v[i]); break; }
    case DXGI_FORMAT_R8G8B8A8_SNORM: for(int i=0;i<3;++i)out[i]=float(int8_t(p[i]))/127.0f; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM: for(int i=0;i<3;++i)out[i]=float(p[i])/127.5f-1.0f; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: { uint32_t v;memcpy(&v,p,4);for(int i=0;i<3;++i)out[i]=float((v>>(10*i))&1023u)/511.5f-1.0f; break; }
    default: return false;
    }
    float length=sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]);
    if(!(length>1e-6f)||!std::isfinite(length))return false;
    for(int i=0;i<3;++i)out[i]/=length;
    return true;
}
bool decode_uv(const uint8_t* p,DXGI_FORMAT format,float out[2]) {
    switch(format) {
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: memcpy(out,p,8); break;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT: { uint16_t v[2];memcpy(v,p,4);out[0]=half_float(v[0]);out[1]=half_float(v[1]); break; }
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16B16A16_SNORM: { int16_t v[2];memcpy(v,p,4);out[0]=float(v[0])/32767.0f;out[1]=float(v[1])/32767.0f; break; }
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM: { uint16_t v[2];memcpy(v,p,4);out[0]=float(v[0])/65535.0f;out[1]=float(v[1])/65535.0f; break; }
    default: return false;
    }
    return std::isfinite(out[0])&&std::isfinite(out[1]);
}

/* ---- worker ---- */
/* Simplified indices of the job's range (same vertex numbering); empty when the range must stay. */
std::vector<uint32_t> simplify(const Job& job) {
    const Layout& layout=job.layout;
    const std::vector<uint8_t>& bytes=job.vertices->bytes;
    uint32_t low=UINT32_MAX,high=0;
    for(uint32_t index:job.indices){low=index<low?index:low;high=index>high?index:high;}
    int64_t base=int64_t(job.source.offset)+int64_t(job.source.base)*int64_t(job.source.stride);
    int64_t first_byte=base+int64_t(low)*job.source.stride,last_byte=base+int64_t(high)*job.source.stride+job.source.stride;
    if(first_byte<0||last_byte>int64_t(bytes.size())||high-low>=(1u<<22))return {};
    size_t vertices=size_t(high-low)+1;
    std::vector<float> positions(vertices*3),attributes(vertices*6,0.0f);
    bool normals=layout.normal.present&&layout.normal.slot==layout.position.slot;
    bool uvs=layout.uv.present&&layout.uv.slot==layout.position.slot;
    bool bones=layout.bones.present&&layout.bones.slot==layout.position.slot&&format_bytes(layout.bones.format)>=4;
    for(size_t v=0;v<vertices;++v) {
        const uint8_t* vertex=bytes.data()+first_byte+int64_t(v)*job.source.stride;
        memcpy(&positions[v*3],vertex+layout.position.offset,12);
        if(!std::isfinite(positions[v*3])||!std::isfinite(positions[v*3+1])||!std::isfinite(positions[v*3+2]))return {};
        float* a=&attributes[v*6];
        if(normals&&!decode_normal(vertex+layout.normal.offset,layout.normal.format,a))normals=false;
        if(uvs&&!decode_uv(vertex+layout.uv.offset,layout.uv.format,a+3))uvs=false;
        /* The first bone a vertex follows: vertices of different bones never merge. */
        if(bones){uint32_t packed;memcpy(&packed,vertex+layout.bones.offset,4);a[5]=float(packed&0xffffu);}
    }
    if(!normals)for(size_t v=0;v<vertices;++v){attributes[v*6]=attributes[v*6+1]=attributes[v*6+2]=0;}
    if(!uvs)for(size_t v=0;v<vertices;++v){attributes[v*6+3]=attributes[v*6+4]=0;}
    std::vector<uint32_t> local(job.indices.size()),result(job.indices.size());
    for(size_t i=0;i<local.size();++i)local[i]=job.indices[i]-low;
    /* Shading and texture seams cost error; the weights are relative to the size of the mesh. */
    const float weights[6]={0.25f,0.25f,0.25f,0.5f,0.5f,8.0f};
    float reached=0;
    size_t kept=meshopt_simplifyWithAttributes(result.data(),local.data(),local.size(),positions.data(),vertices,12,
        attributes.data(),24,weights,6,nullptr,0,job.error,
        meshopt_SimplifyErrorAbsolute|meshopt_SimplifyPrune|meshopt_SimplifyPermissive|meshopt_SimplifySparse,&reached);
    result.resize(kept);
    for(uint32_t& index:result)index+=low;
    return result;
}
void worker_main() {
    pthread_setname_np(pthread_self(),"fp-geometry");
    setpriority(PRIO_PROCESS,gettid(),10);       /* never competes with the game's own threads */
    for(;;) {
        Job job;
        { std::unique_lock guard(queue_lock);queue_signal.wait(guard,[]{return !jobs.empty();});job=std::move(jobs.front());jobs.pop_front(); }
        struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);
        Result result;result.ticket=job.ticket;result.buffer=job.buffer;result.first=job.first;result.indices=simplify(job);
        clock_gettime(CLOCK_MONOTONIC,&end);
        stat_micros.fetch_add(uint64_t((end.tv_sec-begin.tv_sec)*1000000LL+(end.tv_nsec-begin.tv_nsec)/1000),std::memory_order_relaxed);
        { std::lock_guard guard(queue_lock);results.push_back(std::move(result)); }
        results_ready.fetch_add(1,std::memory_order_release);
    }
}
void submit(Job&& job) {
    std::lock_guard guard(queue_lock);
    if(!worker_started){worker_started=true;std::thread(worker_main).detach();}
    jobs.push_back(std::move(job));queue_signal.notify_one();
}

/* ---- registries ---- */
void forget(IUnknown* identity) {
    std::lock_guard guard(lock);
    auto vertex=vertex_copies.find(identity);
    if(vertex!=vertex_copies.end()){copy_bytes-=vertex->second->bytes.size();vertex_copies.erase(vertex);}
    auto index=index_buffers.find(identity);
    if(index!=index_buffers.end()) {
        if(index->second.copy)copy_bytes-=index->second.copy->bytes.size();
        index_buffers.erase(index);tracked_index_buffers.store(unsigned(index_buffers.size()),std::memory_order_relaxed);
    }
    layouts.erase(identity);
    /* The input-assembler state is left alone: an object that is still bound is kept alive by
     * DXVK, so a destroyed identity cannot be one of the bound ones. */
}
/* CreateBuffer: a static index buffer is created updatable, so its ranges can be rewritten later. */
struct BufferCreation {
    D3D11_BUFFER_DESC* desc=nullptr; D3D11_USAGE usage=D3D11_USAGE_DEFAULT; bool capture=false;
    BufferCreation(const D3D11_BUFFER_DESC* description,const D3D11_SUBRESOURCE_DATA* data) {
        if(!enabled()||!description||!data||!data->pSysMem)return;
        bool index=description->BindFlags==D3D11_BIND_INDEX_BUFFER,vertex=description->BindFlags==D3D11_BIND_VERTEX_BUFFER;
        if((!index&&!vertex)||description->Usage==D3D11_USAGE_DYNAMIC||description->Usage==D3D11_USAGE_STAGING||description->CPUAccessFlags
           ||description->MiscFlags||description->ByteWidth<MIN_INDICES*2||description->ByteWidth>MAX_COPY_BYTES)return;
        capture=true;
        if(index&&description->Usage==D3D11_USAGE_IMMUTABLE) {
            desc=const_cast<D3D11_BUFFER_DESC*>(description);usage=desc->Usage;desc->Usage=D3D11_USAGE_DEFAULT;
        }
    }
    void created(CPU* c,const D3D11_BUFFER_DESC* description,const D3D11_SUBRESOURCE_DATA* data) {
        if(desc){desc->Usage=usage;desc=nullptr;}
        if(!capture||FAILED(HRESULT(c->rax))||!arg(c,3))return;
        uint64_t guest=read64(arg(c,3));
        if(!guest)return;
        IUnknown* identity=proxy(guest).identity;
        const uint8_t* bytes=static_cast<const uint8_t*>(win64_pointer(reinterpret_cast<uintptr_t>(data->pSysMem),description->ByteWidth,0));
        std::lock_guard guard(lock);
        if(copy_bytes+description->ByteWidth>COPY_BUDGET)return;
        auto copy=std::make_shared<Copy>();copy->bytes.assign(bytes,bytes+description->ByteWidth);copy_bytes+=description->ByteWidth;
        if(description->BindFlags==D3D11_BIND_INDEX_BUFFER) {
            IndexBuffer& buffer=index_buffers[identity];buffer=IndexBuffer();buffer.copy=std::move(copy);
            tracked_index_buffers.store(unsigned(index_buffers.size()),std::memory_order_relaxed);
        } else vertex_copies[identity]=std::move(copy);
    }
    ~BufferCreation() { if(desc)desc->Usage=usage; }
};
void layout_created(CPU* c,const D3D11_INPUT_ELEMENT_DESC* descs,size_t count) {
    if(!enabled()||FAILED(HRESULT(c->rax))||!arg(c,5))return;
    uint64_t guest=read64(arg(c,5));
    if(!guest)return;
    Layout layout;unsigned next[16]={0};bool known[16];for(bool& k:known)k=true;
    for(size_t i=0;i<count;++i) {
        const D3D11_INPUT_ELEMENT_DESC& d=descs[i];
        if(d.InputSlot>=16)continue;
        unsigned size=format_bytes(d.Format);
        unsigned offset=d.AlignedByteOffset==D3D11_APPEND_ALIGNED_ELEMENT?(next[d.InputSlot]+3u)&~3u:d.AlignedByteOffset;
        bool located=d.AlignedByteOffset!=D3D11_APPEND_ALIGNED_ELEMENT||known[d.InputSlot];
        if(size)next[d.InputSlot]=offset+size;else known[d.InputSlot]=false;
        if(!located||d.InputSlotClass!=D3D11_INPUT_PER_VERTEX_DATA||d.SemanticIndex||!d.SemanticName)continue;
        const char* name=win64_string_a(reinterpret_cast<uintptr_t>(d.SemanticName));
        Element element;element.present=true;element.slot=d.InputSlot;element.offset=offset;element.format=d.Format;
        if(!strcasecmp(name,"POSITION"))layout.position=element;
        else if(!strcasecmp(name,"NORMAL"))layout.normal=element;
        else if(!strcasecmp(name,"TEXCOORD"))layout.uv=element;
        else if(!strcasecmp(name,"BLENDINDICES"))layout.bones=element;
    }
    std::lock_guard guard(lock);
    layouts[proxy(guest).identity]=layout;
}

/* ---- input-assembler state ---- */
IUnknown* identity_or_null(uint64_t guest) { return guest?proxy(guest).identity:nullptr; }
struct Slots { unsigned layout,vertex,index,topology,draw,draw_instanced,draw_indirect,clear_state,update,copy,copy_region,targets,targets_uav; };
const Slots& slots() {
    static const Slots s={context_slot("IASetInputLayout"),context_slot("IASetVertexBuffers"),context_slot("IASetIndexBuffer"),
        context_slot("IASetPrimitiveTopology"),context_slot("DrawIndexed"),context_slot("DrawIndexedInstanced"),
        context_slot("DrawIndexedInstancedIndirect"),context_slot("ClearState"),
        context_slot("UpdateSubresource"),context_slot("CopyResource"),context_slot("CopySubresourceRegion"),
        context_slot("OMSetRenderTargets"),context_slot("OMSetRenderTargetsAndUnorderedAccessViews")};
    return s;
}
void write_range(ID3D11DeviceContext* context,IUnknown* buffer,uint32_t first_byte,const std::vector<uint8_t>& bytes) {
    ID3D11Buffer* native=nullptr;
    if(FAILED(buffer->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&native)))||!native)return;
    D3D11_BOX box={first_byte,0,0,first_byte+UINT(bytes.size()),1,1};
    context->UpdateSubresource(native,0,&box,bytes.data(),0,0);
    native->Release();
}
/* Results of the worker: at most a few ranges per frame are written. */
void apply_results(ID3D11DeviceContext* context) {
    for(unsigned budget=6;budget&&results_ready.load(std::memory_order_acquire);--budget) {
        Result result;
        { std::lock_guard guard(queue_lock);if(results.empty())break;result=std::move(results.front());results.pop_front(); }
        results_ready.fetch_sub(1,std::memory_order_release);
        std::vector<uint8_t> bytes;uint32_t first_byte=0;
        {
            std::lock_guard guard(lock);
            auto found=index_buffers.find(result.buffer);
            if(found==index_buffers.end())continue;
            auto atom=found->second.atoms.find(result.first);
            if(atom==found->second.atoms.end()||atom->second.ticket!=result.ticket||atom->second.state!=Atom::Pending)continue;
            Atom& a=atom->second;
            /* Not worth a rewrite unless a tenth of the range goes away. */
            if(result.indices.empty()||result.indices.size()%3||result.indices.size()*10>size_t(a.count)*9){a.state=Atom::Skip;stat_skipped.fetch_add(1,std::memory_order_relaxed);continue;}
            unsigned width=a.original.size()/a.count;
            bytes=a.original;
            uint32_t filler=0;memcpy(&filler,a.original.data(),width);
            for(uint32_t i=0;i<a.count;++i) {
                uint32_t index=i<result.indices.size()?result.indices[i]:filler;
                memcpy(bytes.data()+size_t(i)*width,&index,width);
            }
            first_byte=a.first*width;
            memcpy(found->second.copy->bytes.data()+first_byte,bytes.data(),bytes.size());
            a.reduced=uint32_t(result.indices.size());a.state=Atom::Ready;
            stat_ready.fetch_add(1,std::memory_order_relaxed);stat_before.fetch_add(a.count,std::memory_order_relaxed);stat_after.fetch_add(a.reduced,std::memory_order_relaxed);
        }
        write_range(context,result.buffer,first_byte,bytes);
    }
}
/* The index count a draw of [first, first+count) of the bound index buffer asks DXVK for. */
uint32_t draw(ID3D11DeviceContext* context,uint32_t count,uint32_t start,int base) {
    if(results_ready.load(std::memory_order_acquire))apply_results(context);
    if(!state_index||!tracked_index_buffers.load(std::memory_order_relaxed))return count;
    std::vector<std::pair<uint32_t,std::vector<uint8_t>>> restore;
    uint32_t answer=count;
    IUnknown* target=nullptr;
    Job job;bool submit_job=false;
    {
        std::lock_guard guard(lock);
        if(!state_index||!state_index_bytes||state_index_offset%state_index_bytes||!count)return count;
        uint32_t first=start+state_index_offset/state_index_bytes;
        auto found=index_buffers.find(state_index);
        if(found==index_buffers.end())return count;
        IndexBuffer& buffer=found->second;
        if(buffer.atoms.empty()&&(count<MIN_INDICES||buffer.frozen))return count;
        target=state_index;
        const Layout* layout=nullptr;
        auto known=layouts.find(state_layout);
        if(known!=layouts.end()&&known->second.position.present&&known->second.position.slot<8)layout=&known->second;
        Source source;
        if(layout){const Bound& bound=state_vertex[layout->position.slot];source.buffer=bound.buffer;source.offset=bound.offset;source.stride=bound.stride;source.base=base;source.layout=state_layout;}
        auto exact=buffer.atoms.find(first);
        if(exact!=buffer.atoms.end()&&exact->second.count==count) {
            Atom& atom=exact->second;
            if(atom.state==Atom::Skip)return count;
            if(atom.source==source&&state_topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
                if(atom.state==Atom::Ready){frame_before+=count;frame_after+=atom.reduced;return atom.reduced;}
                return count;
            }
            /* The same indices with other vertices or another topology: the rewrite is not valid for them. */
            if(atom.state==Atom::Ready){restore.emplace_back(atom.first*uint32_t(atom.original.size()/atom.count),atom.original);
                memcpy(buffer.copy->bytes.data()+restore.back().first,atom.original.data(),atom.original.size());stat_restored.fetch_add(1,std::memory_order_relaxed);}
            atom.state=Atom::Skip;atom.ticket=0;atom.original.clear();
        } else {
            /* Atoms that this range touches without being one of them. */
            bool contains_only=true,touches=false;
            auto it=buffer.atoms.lower_bound(first);
            if(it!=buffer.atoms.begin()){auto before=std::prev(it);if(before->second.first+before->second.count>first)it=before;}
            std::vector<uint32_t> cut;
            for(;it!=buffer.atoms.end()&&it->second.first<first+count;++it) {
                const Atom& atom=it->second;touches=true;
                if(atom.first<first||atom.first+atom.count>first+count){contains_only=false;cut.push_back(atom.first);}
            }
            for(uint32_t key:cut) {
                Atom& atom=buffer.atoms[key];
                if(atom.state==Atom::Ready){restore.emplace_back(atom.first*uint32_t(atom.original.size()/atom.count),atom.original);
                    memcpy(buffer.copy->bytes.data()+restore.back().first,atom.original.data(),atom.original.size());stat_restored.fetch_add(1,std::memory_order_relaxed);}
                buffer.atoms.erase(key);
            }
            if(touches&&contains_only)return count;      /* a range around whole atoms: its indices are valid as they are */
            bool contains=false;
            if(touches) {
                /* After giving up the atoms it cut, the range may still contain others. */
                auto again=buffer.atoms.lower_bound(first);
                contains=again!=buffer.atoms.end()&&again->second.first<first+count;
            }
            /* Small ranges are only checked against the atoms; they never become one. */
            if(!contains&&count>=MIN_INDICES&&!buffer.frozen) {
            Atom atom;atom.first=first;atom.count=count;atom.source=source;
            uint64_t end_byte=(uint64_t(first)+count)*state_index_bytes;
            bool eligible=state_world&&layout&&count>=MIN_INDICES&&count%3==0&&state_topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST&&source.buffer&&source.stride
                &&buffer.copy&&end_byte<=buffer.copy->bytes.size()
                &&(layout->position.format==DXGI_FORMAT_R32G32B32_FLOAT||layout->position.format==DXGI_FORMAT_R32G32B32A32_FLOAT)
                &&layout->position.offset+12<=source.stride;
            auto vertices=eligible?vertex_copies.find(source.buffer):vertex_copies.end();
            if(eligible&&vertices!=vertex_copies.end()) {
                const uint8_t* data=buffer.copy->bytes.data()+size_t(first)*state_index_bytes;
                atom.original.assign(data,data+size_t(count)*state_index_bytes);
                atom.state=Atom::Pending;atom.ticket=next_ticket++;
                job.ticket=atom.ticket;job.buffer=state_index;job.first=first;job.count=count;job.vertices=vertices->second;
                job.source=source;job.layout=*layout;job.error=error_bound();
                job.indices.resize(count);
                for(uint32_t i=0;i<count;++i){uint32_t index=0;memcpy(&index,data+size_t(i)*state_index_bytes,state_index_bytes);job.indices[i]=index;}
                submit_job=true;stat_jobs.fetch_add(1,std::memory_order_relaxed);
            }
            buffer.atoms[first]=std::move(atom);
            }
        }
    }
    for(auto& range:restore)write_range(context,target,range.first,range.second);
    if(submit_job)submit(std::move(job));
    return answer;
}
/* A draw that takes its range from a GPU buffer: every rewritten range of the bound index buffer
 * goes back to the original indices and the buffer is left alone from now on. */
void draw_unknown_range(ID3D11DeviceContext* context) {
    std::vector<std::pair<uint32_t,std::vector<uint8_t>>> restore;
    IUnknown* target=nullptr;
    {
        std::lock_guard guard(lock);
        auto found=index_buffers.find(state_index);
        if(found==index_buffers.end()||found->second.frozen)return;
        IndexBuffer& buffer=found->second;target=state_index;buffer.frozen=true;
        for(auto& entry:buffer.atoms) {
            Atom& atom=entry.second;
            if(atom.state!=Atom::Ready)continue;
            restore.emplace_back(atom.first*uint32_t(atom.original.size()/atom.count),atom.original);
            memcpy(buffer.copy->bytes.data()+restore.back().first,atom.original.data(),atom.original.size());
            stat_restored.fetch_add(1,std::memory_order_relaxed);
        }
        buffer.atoms.clear();
    }
    for(auto& range:restore)write_range(context,target,range.first,range.second);
}
/* Put every remembered range back on `want` pixels.  False when a GPU write is required and the
 * immediate context is not known yet: the caller retries on the next present. */
bool retarget(ID3D11DeviceContext* context, float want) {
    struct Restore { IUnknown* buffer; uint32_t first; std::vector<uint8_t> bytes; };
    std::vector<Restore> restores;
    std::vector<Job> queued;
    float bound=0;
    if(want>0){
        unsigned height=unsigned(display_extent.load(std::memory_order_relaxed)&0xffffffffu);
        if(!height)height=720;
        bound=want/float(height);
        if(bound<0.01f)bound=0.01f; else if(bound>1.0f)bound=1.0f;
    }
    if(!context){
        std::lock_guard guard(lock);
        for(auto& entry:index_buffers)
            for(auto& atom_entry:entry.second.atoms){
                const Atom& atom=atom_entry.second;
                if(atom.state==Atom::Ready&&atom.reduced&&atom.reduced<atom.count&&!atom.original.empty())return false;
            }
    }
    {
        std::lock_guard guard(queue_lock);
        jobs.clear();
    }
    {
        std::lock_guard guard(lock);
        for(auto& entry:index_buffers) {
            IndexBuffer& buffer=entry.second;
            for(auto& atom_entry:buffer.atoms) {
                Atom& atom=atom_entry.second;
                if(atom.original.empty()||!atom.count||atom.original.size()%atom.count)continue;
                unsigned width=unsigned(atom.original.size()/atom.count);
                bool simplified=atom.state==Atom::Ready&&atom.reduced&&atom.reduced<atom.count;
                if(simplified){
                    if(!context)return false;
                    uint32_t first_byte=atom.first*width;
                    if(buffer.copy&&size_t(first_byte)+atom.original.size()<=buffer.copy->bytes.size())
                        memcpy(buffer.copy->bytes.data()+first_byte,atom.original.data(),atom.original.size());
                    restores.push_back({entry.first,first_byte,atom.original});
                }
                atom.ticket=next_ticket++;
                if(!(want>0)){atom.state=Atom::Ready;atom.reduced=atom.count;continue;}
                const Layout* layout=nullptr;
                auto known=layouts.find(atom.source.layout);
                if(known!=layouts.end()&&known->second.position.present)layout=&known->second;
                auto vertices=vertex_copies.find(atom.source.buffer);
                if(!layout||vertices==vertex_copies.end()||width<2||width>4||atom.count%3){
                    atom.state=Atom::Ready;atom.reduced=atom.count;continue;
                }
                Job job;job.ticket=atom.ticket;job.buffer=entry.first;job.first=atom.first;job.count=atom.count;
                job.vertices=vertices->second;job.source=atom.source;job.layout=*layout;job.error=bound;
                job.indices.resize(atom.count);
                for(uint32_t i=0;i<atom.count;++i){uint32_t index=0;memcpy(&index,atom.original.data()+size_t(i)*width,width);job.indices[i]=index;}
                atom.state=Atom::Pending;
                queued.push_back(std::move(job));
            }
        }
    }
    for(auto& range:restores)write_range(context,range.buffer,range.first,range.bytes);
    for(auto& job:queued){stat_jobs.fetch_add(1,std::memory_order_relaxed);submit(std::move(job));}
    fprintf(stderr,"FP_GEOMETRY_RETARGET pixels=%.1f restores=%zu jobs=%zu\n",double(want),restores.size(),queued.size());
    return true;
}
void present() {
    float want=error_pixels();
    ID3D11DeviceContext* context=nullptr;
    if(state_context)state_context->QueryInterface(__uuidof(ID3D11DeviceContext),reinterpret_cast<void**>(&context));
    if(!detail_primed){applied_pixels=want;detail_primed=true;draw_on=want>0?1:0;}
    else if(want!=applied_pixels){
        if(!retarget(context,want)){if(context)context->Release();return;}
        applied_pixels=want;draw_on=want>0?1:0;
    }
    if(context)context->Release();
    if(!(want>0))return;
    if(++stat_frame%600==300)
        fprintf(stderr,"FP_GEOMETRY frame=%llu error=%.3f jobs=%llu ready=%llu skipped=%llu restored=%llu indices_before=%llu indices_after=%llu "
            "frame_indices_before=%llu frame_indices_after=%llu worker_ms=%llu copies_mb=%zu foreign_context=%d\n",
            (unsigned long long)stat_frame,double(error_bound()),(unsigned long long)stat_jobs.load(),(unsigned long long)stat_ready.load(),
            (unsigned long long)stat_skipped.load(),(unsigned long long)stat_restored.load(),(unsigned long long)stat_before.load(),
            (unsigned long long)stat_after.load(),(unsigned long long)frame_before,(unsigned long long)frame_after,
            (unsigned long long)(stat_micros.load()/1000),copy_bytes>>20,int(foreign_context));
    frame_before=frame_after=0;
}
/* The game itself writes into a buffer this module keeps a copy of: nothing derived from the old
 * contents may stay.  Rewritten ranges that used the buffer get their original indices back
 * before the game's write goes through, and the buffer is not looked at again. */
void written(ID3D11DeviceContext* context,IUnknown* identity) {
    struct Range { IUnknown* buffer; uint32_t first_byte; std::vector<uint8_t> bytes; };
    std::vector<Range> restore;
    {
        std::lock_guard guard(lock);
        auto vertex=vertex_copies.find(identity);
        bool index=index_buffers.count(identity)!=0;
        if(vertex==vertex_copies.end()&&!index)return;
        for(auto& entry:index_buffers) {
            IndexBuffer& buffer=entry.second;
            bool whole=entry.first==identity;
            for(auto it=buffer.atoms.begin();it!=buffer.atoms.end();) {
                Atom& atom=it->second;
                if(!whole&&atom.source.buffer!=identity){++it;continue;}
                if(atom.state==Atom::Ready) {
                    uint32_t first_byte=atom.first*uint32_t(atom.original.size()/atom.count);
                    memcpy(buffer.copy->bytes.data()+first_byte,atom.original.data(),atom.original.size());
                    restore.push_back({entry.first,first_byte,atom.original});
                    stat_restored.fetch_add(1,std::memory_order_relaxed);
                }
                if(whole)it=buffer.atoms.erase(it);
                else{atom.state=Atom::Skip;atom.ticket=0;atom.original.clear();++it;}
            }
            if(whole)buffer.frozen=true;
        }
        if(vertex!=vertex_copies.end()){copy_bytes-=vertex->second->bytes.size();vertex_copies.erase(vertex);}
    }
    for(Range& range:restore)write_range(context,range.buffer,range.first_byte,range.bytes);
}
/* Every ID3D11DeviceContext call passes here before it is forwarded. */
enum Action : uint8_t { Ignore, SetLayout, SetVertex, SetIndex, SetTopology, DrawRange, DrawInstancedRange, DrawIndirect, ClearState, Write, SetTargets, SetTargetsUav };
const uint8_t* actions() {
    static const std::array<uint8_t,256> table=[]{
        std::array<uint8_t,256> t{};const Slots& s=slots();
        t[s.layout]=SetLayout;t[s.vertex]=SetVertex;t[s.index]=SetIndex;t[s.topology]=SetTopology;t[s.draw]=DrawRange;
        t[s.draw_instanced]=DrawInstancedRange;t[s.draw_indirect]=DrawIndirect;t[s.clear_state]=ClearState;
        t[s.update]=Write;t[s.copy]=Write;t[s.copy_region]=Write;t[s.targets]=SetTargets;t[s.targets_uav]=SetTargetsUav;return t;}();
    return table.data();
}
/* The render thread makes about twenty thousand context calls per frame: most return at the table
 * lookup, and the state changes touch only this thread's own copy of the input-assembler state. */
void context_call(CPU* c,unsigned slot) {
    uint8_t action=slot<256?actions()[slot]:uint8_t(Ignore);
    if(action==Ignore)return;
    if(action==Write) {
        uint64_t destination=arg(c,1);
        /* Only buffers are ever tracked; texture copies and updates leave at once. */
        if(destination&&proxy(destination).kind==Kind::ID3D11Buffer){Native<ID3D11DeviceContext> native(arg(c,0));written(native.p,proxy(destination).identity);}
        return;
    }
    uint64_t guest=arg(c,0);
    IUnknown* context=proxy(guest).identity;
    if(!state_context)state_context=context;
    if(context!=state_context){foreign_context=true;return;}   /* a deferred context: its state is not tracked, its draws stay as they are */
    switch(action) {
    case SetLayout: state_layout=identity_or_null(arg(c,1)); return;
    case SetTopology: state_topology=unsigned(arg(c,1)); return;
    case SetTargets: case SetTargetsUav: {
        uint32_t count=uint32_t(arg(c,1));
        /* D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL: only the UAVs change. */
        if(action==SetTargetsUav&&count==0xffffffffu)return;
        unsigned colour=0;uint64_t views=arg(c,2);
        for(uint32_t i=0;views&&i<count&&i<8;++i)if(read64(views+8ull*i))++colour;
        state_world=colour>=2||(colour==0&&arg(c,3)!=0);
        return;
    }
    case SetIndex:
        state_index=identity_or_null(arg(c,1));state_index_offset=uint32_t(arg(c,3));
        state_index_bytes=uint32_t(arg(c,2))==DXGI_FORMAT_R16_UINT?2:uint32_t(arg(c,2))==DXGI_FORMAT_R32_UINT?4:0;
        return;
    case ClearState:
        state_layout=state_index=nullptr;state_index_bytes=state_index_offset=state_topology=0;state_world=false;
        for(Bound& bound:state_vertex)bound=Bound();
        return;
    case SetVertex: {
        unsigned start=uint32_t(arg(c,1)),count=uint32_t(arg(c,2));
        for(unsigned i=0;i<count&&start+i<8;++i) {
            Bound& bound=state_vertex[start+i];
            bound.buffer=arg(c,3)?identity_or_null(read64(arg(c,3)+8ull*i)):nullptr;
            bound.stride=arg(c,4)?ref<const UINT>(arg(c,4)+4ull*i):0;
            bound.offset=arg(c,5)?ref<const UINT>(arg(c,5)+4ull*i):0;
        }
        return;
    }
    default: break;
    }
    Native<ID3D11DeviceContext> native(guest);
    if(action==DrawIndirect){draw_unknown_range(native.p);return;}
    uint32_t count=uint32_t(arg(c,1));
    uint32_t start=uint32_t(arg(c,action==DrawRange?2:3));
    int base=int(uint32_t(arg(c,action==DrawRange?3:4)));
    uint32_t reduced=draw(native.p,count,start,base);
    if(reduced!=count)c->rdx=reduced;                         /* IndexCount / IndexCountPerInstance */
}
}
extern "C" void frostpunk_geometry_detail(int level) {
    if(level<0)level=0;
    if(level>5)level=5;
    geometry::detail_choice.store(level,std::memory_order_relaxed);
    fprintf(stderr,"FP_GEOMETRY_DETAIL level=%d\n",level);
}
