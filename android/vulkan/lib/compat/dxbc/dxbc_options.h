/* Stand-in for DXVK's dxbc_options.h: the native rendering device fills the options itself. */
#pragma once
#include <vulkan/vulkan.h>
#include "../util/util_flags.h"

namespace dxvk {
  enum class DxbcFloatControlFlag : uint32_t {
    DenormFlushToZero32,
    DenormPreserve64,
    PreserveNan32,
    PreserveNan64,
  };
  using DxbcFloatControlFlags = Flags<DxbcFloatControlFlag>;

  struct DxbcOptions {
    bool useDepthClipWorkaround = false;
    bool supportsClipDistance = true;
    bool supportsCullDistance = true;
    bool supportsTypedUavLoadR32 = false;
    bool supportsRawAccessChains = false;
    bool emulateRobustness2 = false;
    bool zeroInitWorkgroupMemory = false;
    bool invariantPosition = false;
    bool forceVolatileTgsmAccess = false;
    bool disableMsaa = false;
    bool forceSampleRateShading = false;
    bool enableSampleShadingInterlock = false;
    DxbcFloatControlFlags floatControl;
    VkDeviceSize minSsboAlignment = 0;
  };
}
