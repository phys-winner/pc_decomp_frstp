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
