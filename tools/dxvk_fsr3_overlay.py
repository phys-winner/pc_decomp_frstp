"""Optional FSR3: original scene depth/camera, real optical flow, raster jitter, UI reactivity."""
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
FILES=('src/dxvk/dxvk_fsr3.h','src/dxvk/dxvk_fsr3.cpp','src/dxvk/fsr3_api.h',
       'src/dxvk/shaders/dxvk_fsr3_prepare.comp','src/d3d11/frostpunk_fsr3_context.h',
       'src/dxvk/dxvk_context.h','src/dxvk/dxvk_cmdlist.h','src/dxvk/dxvk_swapchain_blitter.h',
       'src/dxvk/dxvk_swapchain_blitter.cpp','src/d3d11/d3d11_context.h','src/d3d11/d3d11_context.cpp',
       'src/d3d11/d3d11_swapchain.cpp','src/dxvk/meson.build','src/dxvk/dxvk_adapter.cpp','src/dxvk/dxvk_context.cpp')

def apply(work):
    work=Path(work)
    def patch(name,before,after):
        path=work/name;text=path.read_text(encoding='utf-8')
        if text.count(before)!=1:raise ValueError('FSR3 overlay preimage mismatch: '+name+': '+before[:80])
        path.write_text(text.replace(before,after),encoding='utf-8')
    copies={FILES[0]:'fsr3_dxvk.h',FILES[1]:'fsr3_dxvk.cpp',FILES[2]:'fsr3_api.h',FILES[3]:'fsr3_prepare.comp',FILES[4]:'fsr3_context.h'}
    for target,source in copies.items():
        path=work/target;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes((ROOT/'android/dxvk'/source).read_bytes())
    patch('src/dxvk/dxvk_adapter.cpp','    enabledFeatures.vk12.vulkanMemoryModel = VK_TRUE;',
        '    enabledFeatures.vk12.vulkanMemoryModel = VK_TRUE;\n'
        '#if defined(__ANDROID__)\n'
        '    enabledFeatures.vk12.vulkanMemoryModelDeviceScope = m_deviceFeatures.vk12.vulkanMemoryModelDeviceScope;\n'
        '#endif')
    patch('src/dxvk/dxvk_context.cpp', '''          VkImageLayout             initialLayout) {
    if (initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {''',
        '''          VkImageLayout             initialLayout) {
    this->spillRenderPass(true);
    if (initialLayout == VK_IMAGE_LAYOUT_PREINITIALIZED) {''')
    patch('src/dxvk/meson.build',"  'dxvk_context.cpp',","  'dxvk_context.cpp',\n  'dxvk_fsr3.cpp',")
    patch('src/dxvk/meson.build',"  'shaders/dxvk_present_frag_fsr.frag',","  'shaders/dxvk_present_frag_fsr.frag',\n  'shaders/dxvk_fsr3_prepare.comp',")
    patch('src/dxvk/dxvk_context.h','#include "dxvk_barrier.h"','#include "dxvk_barrier.h"\n#include "dxvk_fsr3.h"')
    patch('src/dxvk/dxvk_context.h','  private:\n    \n    Rc<DxvkDevice>          m_device;',
        '''    Rc<DxvkImageView> fpFsr3Upscale(const Rc<DxvkFsr3>& state,const FpTemporalFrame& metadata,const Rc<DxvkImageView>& source);

  private:
    Rc<DxvkImageView> m_fpFsr3Color;
    uint32_t fpFsr3External(const Rc<DxvkFsr3>& state,FpFsr3Frame& frame,bool optical);
    Rc<DxvkDevice>          m_device;''')
    patch('src/dxvk/dxvk_cmdlist.h','  class DxvkCommandList : public RcObject {',
        '  class DxvkCommandList : public RcObject {\n    friend class DxvkContext;')
    patch('src/d3d11/d3d11_context.h','#include "d3d11_context_state.h"',
        '#include "d3d11_context_state.h"\n#include "../dxvk/dxvk_fsr3.h"')
    patch('src/d3d11/d3d11_context.h','  protected:',
        '''#if defined(__ANDROID__)
    FpTemporalFrame FpTakeTemporalFrame();
    void FpSetTemporalJitter(float x,float y,uint32_t width,uint32_t height);
#endif
  protected:''')
    patch('src/d3d11/d3d11_context.h','    bool FpSkipDraw();',
        '''    FpTemporalFrame m_fpTemporal;
    Rc<DxvkImageView> m_fpOpaqueSnapshot;
    bool m_fpTemporalToneMapped=false,m_fpTemporalSnapshotPending=false,m_fpJitterActive=false;
    float m_fpJitterX=0,m_fpJitterY=0,m_fpJitterAppliedX=0,m_fpJitterAppliedY=0;
    uint32_t m_fpJitterWidth=0,m_fpJitterHeight=0;
    void FpTrackTemporalDraw();
    void FpSnapshotTemporalColor();
    bool FpSkipDraw();''')
    cpp='src/d3d11/d3d11_context.cpp'
    patch(cpp,'extern "C" int frostpunk_dxvk_object_highlight() __attribute__((weak));',
        '''extern "C" int frostpunk_dxvk_object_highlight() __attribute__((weak));
extern "C" int frostpunk_dxvk_upscaler() __attribute__((weak));
extern "C" int frostpunk_dxvk_scene_active() __attribute__((weak));
#include <cmath>
static bool fpTemporalSceneEnabled(){
  return frostpunk_dxvk_upscaler&&frostpunk_dxvk_upscaler()==2
    &&frostpunk_dxvk_scene_active&&frostpunk_dxvk_scene_active();
}''')
    patch(cpp,'  bool D3D11CommonContext<ContextType>::FpSkipDraw() {',
        '  bool D3D11CommonContext<ContextType>::FpSkipDraw() {\n    FpTrackTemporalDraw();')
    patch(cpp,'    if (!m_fpLowRes)\n      return;',
        '    if (!m_fpLowRes) { FpSnapshotTemporalColor(); return; }')
    patch(cpp,'    m_fpLowResOut = nullptr;\n  }',
        '    m_fpLowResOut = nullptr;\n    FpSnapshotTemporalColor();\n  }')
    patch(cpp,'    g_fpStats.dispatches += 1;',
        '''    if (fpTemporalSceneEnabled()) {
      const auto& input=m_state.srv[DxbcProgramType::ComputeShader].views[10];
      if(input!=nullptr&&input->GetResourceType()==D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        m_fpTemporal.depth=input->GetImageView();
    }
    g_fpStats.dispatches += 1;''')
    patch(cpp,'        vp.TopLeftX, vp.Height + vp.TopLeftY,',
        '''        vp.TopLeftX+(m_fpJitterActive?m_fpJitterX*vp.Width/m_fpJitterWidth:0),
        vp.Height+vp.TopLeftY+(m_fpJitterActive?m_fpJitterY*vp.Height/m_fpJitterHeight:0),''')
    patch(cpp,'  // Explicitly instantiate here',
        '#include "frostpunk_fsr3_context.h"\n\n  // Explicitly instantiate here')
    header='src/dxvk/dxvk_swapchain_blitter.h'
    patch(header,'    ~DxvkSwapchainBlitter();',
        '''    ~DxvkSwapchainBlitter();
    void setTemporalFrame(FpTemporalFrame frame){m_fpFrame=std::move(frame);}
    void nextTemporalJitter(float& x,float& y,uint32_t& width,uint32_t& height){
      x=y=0;width=height=0;
      if(m_fpFsr3!=nullptr&&m_fpFrame.valid&&!m_fpFsr3->failed){m_fpFsr3->nextJitter(m_fpFrameIndex++,x,y);width=m_fpFsr3->input.width;height=m_fpFsr3->input.height;}
    }''')
    patch(header,'    Rc<DxvkDevice>      m_device;',
        '    Rc<DxvkDevice>      m_device;\n    FpTemporalFrame m_fpFrame;\n    Rc<DxvkFsr3> m_fpFsr3;\n    uint32_t m_fpFrameIndex=0;')
    blitter='src/dxvk/dxvk_swapchain_blitter.cpp'
    patch(blitter,'bool fsr = frostpunk_dxvk_upscaler && frostpunk_dxvk_upscaler() == 1',
        'bool fsr = frostpunk_dxvk_upscaler && frostpunk_dxvk_upscaler() != 0')
    patch(blitter,'    bool sameSize = dstRect.extent == srcRect.extent;',
        '''    auto sceneExtent=m_fpFrame.opaque!=nullptr?m_fpFrame.opaque->mipLevelExtent(0):VkExtent3D{};
    VkExtent2D sceneSize={sceneExtent.width,sceneExtent.height};
    if(frostpunk_dxvk_upscaler&&frostpunk_dxvk_upscaler()==2&&m_fpFrame.valid&&sceneSize.width&&sceneSize.height
       &&!srcRect.offset.x&&!srcRect.offset.y&&!dstRect.offset.x&&!dstRect.offset.y
       &&dstRect.extent.width>=sceneSize.width&&dstRect.extent.height>=sceneSize.height){
      try {
        if(m_fpFsr3==nullptr||!m_fpFsr3->matches(sceneSize,dstRect.extent,m_fpFrame.inverted)){
          // The finish queue signals idle before dropping its resource references.
          m_device->waitForIdle();if(m_fpFsr3!=nullptr)m_fpFsr3->retire();m_fpFsr3=nullptr;
          m_fpFsr3=new DxvkFsr3(m_device,sceneSize,dstRect.extent,m_fpFrame.inverted);
        }
        auto result=ctx->fpFsr3Upscale(m_fpFsr3,m_fpFrame,srcView);
        if(result!=nullptr){
          this->draw(ctx,m_fsCopy,dstView,dstRect,result,dstRect);return;
        }
      }catch(const std::exception& error){
        Logger::err(str::format("FP_FSR3 exception: ",error.what()));
        if(m_fpFsr3!=nullptr)m_fpFsr3->fail(1);
        else if(frostpunk_dxvk_upscaler_failed)frostpunk_dxvk_upscaler_failed(1);
      }
    }else if(m_fpFsr3!=nullptr){
      if(!frostpunk_dxvk_upscaler||frostpunk_dxvk_upscaler()!=2)m_fpFsr3=nullptr;
      else m_fpFsr3->reset=true;
    }
    bool sameSize = dstRect.extent == srcRect.extent;''')
    swapchain='src/d3d11/d3d11_swapchain.cpp'
    patch(swapchain,'    immediateContext->EndFrame();\n    immediateContext->Flush();',
        '''    immediateContext->EndFrame();
    immediateContext->Flush();
    {auto lock=immediateContext->LockContext();m_blitter->setTemporalFrame(immediateContext->FpTakeTemporalFrame());}''')
    patch(swapchain,'      SubmitPresent(immediateContext, sync, i);',
        '''      SubmitPresent(immediateContext, sync, i);
      if(!i){
        float x=0,y=0;uint32_t width=0,height=0;m_blitter->nextTemporalJitter(x,y,width,height);
        auto lock=immediateContext->LockContext();immediateContext->FpSetTemporalJitter(x,y,width,height);
      }''')
