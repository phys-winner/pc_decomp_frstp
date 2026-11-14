#if defined(__ANDROID__)
  template<typename ContextType>
  FpTemporalFrame D3D11CommonContext<ContextType>::FpTakeTemporalFrame(){
    if(!fpTemporalSceneEnabled()){
      m_fpTemporal=FpTemporalFrame{};m_fpOpaqueSnapshot=nullptr;m_fpTemporalToneMapped=false;return {};
    }
    FpTemporalFrame frame=m_fpTemporal;
    frame.valid=frame.valid&&m_fpTemporalToneMapped&&m_fpOpaqueSnapshot!=nullptr;
    frame.opaque=m_fpOpaqueSnapshot;frame.jitterX=m_fpJitterX;frame.jitterY=m_fpJitterY;
    if(frostpunk_dxvk_upscaler&&frostpunk_dxvk_upscaler()==2){
      static uint32_t reports=0;
      if(reports++<3||reports%300==0)Logger::info(str::format("FP_FSR3 frame camera=",m_fpTemporal.valid," depth=",frame.depth!=nullptr," opaque=",m_fpTemporalToneMapped," valid=",frame.valid));
    }
    m_fpTemporal.valid=false;m_fpTemporalToneMapped=false;return frame;
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpSetTemporalJitter(float x,float y,uint32_t width,uint32_t height){
    if(x!=m_fpJitterX||y!=m_fpJitterY||width!=m_fpJitterWidth||height!=m_fpJitterHeight){
      m_fpJitterX=x;m_fpJitterY=y;m_fpJitterWidth=width;m_fpJitterHeight=height;
      m_fpJitterAppliedX=INFINITY;m_fpJitterAppliedY=INFINITY;
    }
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpTrackTemporalDraw(){
    bool enabled=fpTemporalSceneEnabled();
    if(!enabled&&!m_fpJitterActive)return;
    Rc<DxvkImage> depth=m_state.om.dsv!=nullptr?m_state.om.dsv->GetImageView()->image():nullptr;
    bool scene=enabled&&m_fpTemporal.depth!=nullptr&&depth==m_fpTemporal.depth->image();
    bool jitter=scene&&m_fpJitterWidth&&m_fpJitterHeight;
    if(jitter!=m_fpJitterActive||m_fpJitterAppliedX!=m_fpJitterX||m_fpJitterAppliedY!=m_fpJitterY){
      m_fpJitterActive=jitter;m_fpJitterAppliedX=m_fpJitterX;m_fpJitterAppliedY=m_fpJitterY;ApplyViewportState();
    }
    if(scene&&!m_fpTemporal.valid){
      const auto& binding=m_state.cbv[DxbcProgramType::VertexShader].buffers[0];
      if(binding.buffer!=nullptr){
        D3D11_BUFFER_DESC desc{};binding.buffer->GetDesc(&desc);auto mapped=binding.buffer->GetMappedSlice();
        // PerFrame starts with ViewProjMatrix/ViewMatrix; the engine pools larger buffers.
        if(desc.ByteWidth>=576&&mapped.mapPtr&&binding.constantOffset==0){
          float matrices[32];std::memcpy(matrices,mapped.mapPtr,sizeof(matrices));
          m_fpTemporal.valid=DxvkFsr3::projection(matrices,matrices+16,m_fpTemporal);
        }
      }
    }
    uint32_t role=m_state.ps!=nullptr?m_state.ps->GetCommonShader()->GetFpRole():uint32_t(D3D11FpRoleNone);
    m_fpTemporalSnapshotPending=enabled&&m_fpTemporal.valid&&m_state.om.rtvs[0]!=nullptr
      &&(role==D3D11FpRoleTonemap||role==D3D11FpRoleTonemapOpaque);
  }

  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpSnapshotTemporalColor(){
    if(!m_fpTemporalSnapshotPending)return;
    m_fpTemporalSnapshotPending=false;
    const auto& rtv=m_state.om.rtvs[0];if(rtv==nullptr)return;
    auto source=rtv->GetImageView();auto extent=source->mipLevelExtent(0);
    // Presentation samples UNORM; copying the bytes into the same view format avoids comparing gamma-encoded UI with linear scene color.
    VkFormat format=source->info().format;
    if(format==VK_FORMAT_R8G8B8A8_SRGB)format=VK_FORMAT_R8G8B8A8_UNORM;
    else if(format==VK_FORMAT_B8G8R8A8_SRGB)format=VK_FORMAT_B8G8R8A8_UNORM;
    if(m_fpOpaqueSnapshot==nullptr||m_fpOpaqueSnapshot->imageInfo().extent!=extent||m_fpOpaqueSnapshot->info().format!=format){
      DxvkImageCreateInfo info{};info.type=VK_IMAGE_TYPE_2D;info.format=format;info.sampleCount=VK_SAMPLE_COUNT_1_BIT;
      info.extent=extent;info.numLayers=info.mipLevels=1;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.layout=VK_IMAGE_LAYOUT_GENERAL;
      info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.stages=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;info.access=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
      auto image=m_device->createImage(info,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      DxvkImageViewCreateInfo view{};view.format=info.format;view.usage=VK_IMAGE_USAGE_SAMPLED_BIT;view.aspect=VK_IMAGE_ASPECT_COLOR_BIT;view.numLayers=view.numLevels=1;
      m_fpOpaqueSnapshot=m_device->createImageView(image,view);
      EmitCs([image] (DxvkContext* ctx){ctx->initImage(image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},VK_IMAGE_LAYOUT_UNDEFINED);});
    }
    EmitCs([dst=m_fpOpaqueSnapshot,src=source,extent] (DxvkContext* ctx){
      ctx->copyImage(dst->image(),{VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},src->image(),
        {VK_IMAGE_ASPECT_COLOR_BIT,src->info().minLevel,src->info().minLayer,1},{0,0,0},extent);
    });
    m_fpTemporalToneMapped=true;
  }
#endif
