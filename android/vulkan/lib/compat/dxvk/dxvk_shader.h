/* Stand-in for DXVK's backend shader header: only what the DXBC -> SPIR-V compiler of DXVK 2.4.1
 * (src/dxbc, src/spirv, zlib licence) needs.  The native rendering device takes the SPIR-V and
 * the binding list from DxvkShader and builds its own Vulkan objects. */
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/util_flags.h"
#include "../util/util_error.h"
#include "../spirv/spirv_code_buffer.h"
#include "../spirv/spirv_module.h"

namespace dxvk {
  enum DxvkLimits : uint32_t {
    MaxNumRenderTargets = 8,
    MaxNumVertexAttributes = 32,
    MaxNumVertexBindings = 32,
    MaxNumXfbBuffers = 4,
    MaxNumXfbStreams = 4,
    MaxNumViewports = 16,
    MaxNumResourceSlots = 1216,
    MaxNumQueuedCommandBuffers = 32,
    MaxNumQueryCountPerPool = 128,
    MaxVertexBindingStride = 2048,
    MaxPushConstantSize = 128,
  };

  enum DxvkShaderFlag : uint64_t {
    HasSampleRateShading,
    HasTransformFeedback,
    ExportsPosition,
    ExportsStencilRef,
    ExportsViewportIndexLayerFromVertexStage,
    ExportsSampleMask,
    UsesFragmentCoverage,
    UsesSparseResidency,
  };
  using DxvkShaderFlags = Flags<DxvkShaderFlag>;

  struct DxvkBindingInfo {
    VkDescriptorType      descriptorType;
    uint32_t              resourceBinding;
    VkImageViewType       viewType;
    VkShaderStageFlagBits stage;
    VkAccessFlags         access;
    VkBool32              uboSet;
  };

  struct DxvkShaderCreateInfo {
    VkShaderStageFlagBits stage;
    uint32_t bindingCount = 0;
    const DxvkBindingInfo* bindings = nullptr;
    uint32_t inputMask = 0;
    uint32_t outputMask = 0;
    uint32_t flatShadingInputs = 0;
    VkShaderStageFlags pushConstStages = 0;
    uint32_t pushConstSize = 0;
    uint32_t uniformSize = 0;
    const char* uniformData = nullptr;
    int32_t xfbRasterizedStream = 0;
    uint32_t patchVertexCount = 0;
    uint32_t xfbStrides[MaxNumXfbBuffers] = { };
    VkPrimitiveTopology outputTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
  };

  class DxvkShader : public RcObject {
  public:
    DxvkShader(const DxvkShaderCreateInfo& info, SpirvCodeBuffer&& spirv)
    : m_info(info), m_code(std::move(spirv)) {
      m_bindings.assign(info.bindings, info.bindings + info.bindingCount);
      m_info.bindings = m_bindings.data();
      if (info.uniformSize) {
        m_uniform.assign(info.uniformData, info.uniformData + info.uniformSize);
        m_info.uniformData = m_uniform.data();
      }
    }
    const DxvkShaderCreateInfo& info() const { return m_info; }
    const std::vector<DxvkBindingInfo>& bindings() const { return m_bindings; }
    const SpirvCodeBuffer& code() const { return m_code; }
    SpirvCodeBuffer& code() { return m_code; }
  private:
    DxvkShaderCreateInfo m_info;
    std::vector<DxvkBindingInfo> m_bindings;
    std::vector<char> m_uniform;
    SpirvCodeBuffer m_code;
  };
}
