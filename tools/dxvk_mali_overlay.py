"""Exact upstream overlays for the native Android Mali/PowerVR game renderer.

This enables a bounded D3D11 API subset, not full FL11 conformance. Unsupported
blend/raster/viewport/shader operations fail explicitly rather than change meaning.
"""
from pathlib import Path

FILES = ('src/d3d11/d3d11_device.cpp', 'src/d3d11/d3d11_context.cpp',
         'src/dxbc/dxbc_options.h', 'src/dxbc/dxbc_options.cpp', 'src/dxbc/dxbc_compiler.cpp')


def apply(work, source=None):
    source = Path(source or work)
    contents = {name: (source / name).read_text(encoding='utf-8') for name in FILES}
    def change(name, before, after, count=1):
        text = contents[name]
        if text.count(before) != count:
            raise ValueError('Mali overlay preimage mismatch: ' + name + ': ' + before[:70])
        contents[name] = text.replace(before, after)

    name = FILES[0]
    anchor = '    // Required for Feature Level 11_0\n'
    policy = '''#if defined(__ANDROID__)
    enabled.core.features.textureCompressionASTC_LDR = supported.core.features.textureCompressionASTC_LDR;
    if (Adapter->deviceProperties().vendorID == 0x13b5 || Adapter->deviceProperties().vendorID == 0x1010) {
      // The native game profile rejects unsupported operations at their API boundary.
      enabled.core.features.dualSrcBlend = supported.core.features.dualSrcBlend;
      enabled.core.features.fillModeNonSolid = supported.core.features.fillModeNonSolid;
      enabled.core.features.multiViewport = supported.core.features.multiViewport;
      enabled.core.features.shaderClipDistance = supported.core.features.shaderClipDistance;
      enabled.core.features.shaderCullDistance = supported.core.features.shaderCullDistance;
      enabled.core.features.textureCompressionBC = supported.core.features.textureCompressionBC;
      enabled.extTransformFeedback.geometryStreams = supported.extTransformFeedback.geometryStreams;
    }
#endif

'''
    change(name, anchor, policy + anchor)
    before = '    if (!m_dxvkDevice->features().extTransformFeedback.transformFeedback)\n'
    change(name, before, '''    // DXBC stream-output emits GeometryStreams even when all streams are zero.
    if (!m_dxvkDevice->features().extTransformFeedback.transformFeedback
     || !m_dxvkDevice->features().extTransformFeedback.geometryStreams)
''')
    before = '    if (FAILED(D3D11BlendState::NormalizeDesc(&desc)))\n'
    change(name, before, '''
    if (!m_dxvkDevice->features().core.features.dualSrcBlend) {
      for (unsigned i = 0; i < (desc.IndependentBlendEnable ? 8u : 1u); i++) {
        const auto& rt = desc.RenderTarget[i];
        if (rt.BlendEnable && (rt.SrcBlend >= D3D11_BLEND_SRC1_COLOR
         || rt.DestBlend >= D3D11_BLEND_SRC1_COLOR || rt.SrcBlendAlpha >= D3D11_BLEND_SRC1_COLOR
         || rt.DestBlendAlpha >= D3D11_BLEND_SRC1_COLOR)) {
          Logger::err("D3D11: dual-source blending is unavailable on this Vulkan device");
          return E_INVALIDARG;
        }
      }
    }
''' + before, 2)
    before = '    if (FAILED(D3D11RasterizerState::NormalizeDesc(&desc)))\n'
    change(name, before, '''    if (desc.FillMode == D3D11_FILL_WIREFRAME && !m_dxvkDevice->features().core.features.fillModeNonSolid) {
      Logger::err("D3D11: wireframe rasterization is unavailable on this Vulkan device");
      return E_INVALIDARG;
    }

''' + before, 3)
    name = FILES[1]
    before = '    if (unlikely(NumViewports > m_state.rs.viewports.size()))\n'
    change(name, before, '''    if (NumViewports > 1 && !m_device->features().core.features.multiViewport) {
      throw DxvkError("D3D11: multiple viewports are unavailable on this Vulkan device");
    }

''' + before)
    name = FILES[2]
    before = '    bool useDepthClipWorkaround = false;\n'
    change(name, before, before + '\n    bool supportsClipDistance = true;\n    bool supportsCullDistance = true;\n')
    name = FILES[3]
    before = '    const DxvkDeviceFeatures& devFeatures = device->features();\n'
    change(name, before, before + '''    supportsClipDistance = devFeatures.core.features.shaderClipDistance;
    supportsCullDistance = devFeatures.core.features.shaderCullDistance;
''')
    name = FILES[4]
    before = '''    m_module.enableCapability(spv::CapabilityClipDistance);
    m_module.enableCapability(spv::CapabilityCullDistance);
'''
    change(name, before, '', 4)
    before = '    // Declare an entry point ID. We\'ll need it during the\n'
    change(name, before, '''    const bool clips = analysis.clipCullIn.numClipPlanes || analysis.clipCullOut.numClipPlanes;
    const bool culls = analysis.clipCullIn.numCullPlanes || analysis.clipCullOut.numCullPlanes;
    if ((clips && !moduleInfo.options.supportsClipDistance) || (culls && !moduleInfo.options.supportsCullDistance))
      throw DxvkError("DXBC: clip/cull distance requires an unavailable Vulkan feature");
    if (clips) m_module.enableCapability(spv::CapabilityClipDistance);
    if (culls) m_module.enableCapability(spv::CapabilityCullDistance);

''' + before)
    for name, text in contents.items():
        path = Path(work) / name
        data = text.encode('utf-8')
        if not path.exists() or path.read_bytes() != data:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
