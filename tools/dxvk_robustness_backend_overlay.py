"""Exact DXVK 2.4.1 backend half of the Android Mali robustness ABI 1.

Apply after dxvk_mali_overlay and dxvk_robustness_shader_overlay. Pristine
reference sources are never written. Logical binding 1216 is a descriptor-only
metadata slot, never an index into the 1216 ordinary CPU resource slots.
"""
from pathlib import Path

FILES = ('src/dxvk/dxvk_adapter.cpp', 'src/dxvk/dxvk_pipelayout.cpp',
         'src/dxvk/dxvk_context.h', 'src/dxvk/dxvk_context.cpp')
ABI = {'version': 1, 'logical_binding': 1216, 'records': 1216,
       'stride': 8, 'bound_bit': 1, 'range_units': 'bytes', 'range_max': 0xffffffff}

POLICY = '''  static bool frostpunkEmulatesRobustness2(const DxvkDevice* device) {
#if defined(__ANDROID__)
    return device->properties().core.properties.vendorID == 0x13b5
        && !device->features().extRobustness2.robustBufferAccess2;
#else
    return false;
#endif
  }

'''

METADATA = '''  DxvkBufferSlice DxvkContext::createRobustnessMetadata(const DxvkBindingLayout& layout) {
    static_assert(MaxNumResourceSlots == 1216, "FROSTPUNK_ROBUSTNESS_ABI_1");
    // Immutable per-update snapshot; command-list resource tracking retains its allocation.
    auto slice = m_staging.alloc(m_device->properties().core.properties.limits.minStorageBufferOffsetAlignment,
      MaxNumResourceSlots * 2 * sizeof(uint32_t));
    auto data = static_cast<uint32_t*>(slice.mapPtr(0));
    std::memset(data, 0, MaxNumResourceSlots * 2 * sizeof(uint32_t));
    std::array<bool, MaxNumResourceSlots> seen = { };
    for (uint32_t set = 0; set < DxvkDescriptorSets::SetCount; set++) {
      for (uint32_t index = 0; index < layout.getBindingCount(set); index++) {
        const auto& binding = layout.getBinding(set, index);
        if (binding.resourceBinding == MaxNumResourceSlots)
          continue;
        if (binding.resourceBinding > MaxNumResourceSlots)
          throw DxvkError("Robustness ABI: invalid logical resource slot");
        const auto& res = m_rc[binding.resourceBinding];
        bool bound = false;
        VkDeviceSize range = 0;
        switch (binding.descriptorType) {
          case VK_DESCRIPTOR_TYPE_SAMPLER:
            // An absent D3D sampler uses the existing API-default dummy sampler.
            bound = true;
            break;
          case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
          case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            bound = res.imageView != nullptr && res.imageView->handle(binding.viewType) != VK_NULL_HANDLE;
            break;
          case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            bound = res.sampler != nullptr && res.imageView != nullptr
                 && res.imageView->handle(binding.viewType) != VK_NULL_HANDLE;
            break;
          case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
          case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (res.bufferView != nullptr) {
              res.bufferView->updateView();
              bound = res.bufferView->handle() != VK_NULL_HANDLE;
              range = res.bufferView->info().rangeLength;
            }
            break;
          case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
          case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            bound = res.bufferSlice.length() != 0;
            if (bound)
              range = res.bufferSlice.getDescriptor().buffer.range;
            break;
          default:
            throw DxvkError("Robustness ABI: unsupported descriptor type");
        }
        if (range > UINT32_MAX)
          throw DxvkError("Robustness ABI: buffer view exceeds 32-bit byte range");
        uint32_t slot = binding.resourceBinding;
        uint32_t flags = bound ? 1u : 0u;
        uint32_t bytes = bound ? uint32_t(range) : 0u;
        if (seen[slot] && (data[slot * 2] != flags || data[slot * 2 + 1] != bytes))
          throw DxvkError("Robustness ABI: conflicting logical resource declarations");
        data[slot * 2] = flags;
        data[slot * 2 + 1] = bytes;
        seen[slot] = true;
      }
    }
    m_cmd->trackResource<DxvkAccess::Read>(slice.buffer());
    return slice;
  }


'''


def apply(work, source=None):
    work = Path(work)
    source = Path(source or work)
    shader = (work / 'src/dxbc/dxbc_compiler.cpp').read_text(encoding='utf-8')
    options = (work / 'src/dxbc/dxbc_options.h').read_text(encoding='utf-8')
    if 'FROSTPUNK_ROBUSTNESS_ABI_1' not in shader or 'emulateRobustness2' not in options:
        raise ValueError('Refuse backend feature relaxation without the ABI 1 shader guard overlay')
    contents = {name: (source / name).read_text(encoding='utf-8') for name in FILES}

    def change(name, before, after, count=1):
        if contents[name].count(before) != count:
            raise ValueError('Robustness backend preimage mismatch: ' + name + ': ' + before[:90])
        contents[name] = contents[name].replace(before, after)

    name = FILES[0]
    anchor = '''          DxvkDeviceFeatures  enabledFeatures) {
    DxvkDeviceExtensions devExtensions;
    auto devExtensionList = getExtensionList(devExtensions);
'''
    change(name, anchor, anchor + '''
    bool emulateRobustness2 = false;
#if defined(__ANDROID__)
    emulateRobustness2 = m_deviceInfo.core.properties.vendorID == 0x13b5
                     && !m_deviceFeatures.extRobustness2.robustBufferAccess2;
    if (emulateRobustness2) {
      if (!m_deviceFeatures.vk12.descriptorBindingPartiallyBound)
        throw DxvkError("Mali robustness fallback requires descriptorBindingPartiallyBound");
      devExtensions.extRobustness2.setMode(DxvkExtMode::Optional);
    }
#endif
''')
    before = '''    enabledFeatures.extRobustness2.robustBufferAccess2 = VK_TRUE;
    enabledFeatures.extRobustness2.robustImageAccess2 = m_deviceFeatures.extRobustness2.robustImageAccess2;
    enabledFeatures.extRobustness2.nullDescriptor = VK_TRUE;
'''
    change(name, before, '''    enabledFeatures.extRobustness2.robustBufferAccess2 = emulateRobustness2 ? VK_FALSE : VK_TRUE;
    enabledFeatures.extRobustness2.robustImageAccess2 = emulateRobustness2 ? VK_FALSE : m_deviceFeatures.extRobustness2.robustImageAccess2;
    enabledFeatures.extRobustness2.nullDescriptor = emulateRobustness2 ? VK_FALSE : VK_TRUE;
    if (emulateRobustness2)
      enabledFeatures.vk12.descriptorBindingPartiallyBound = VK_TRUE;
''')
    name = FILES[1]
    change(name, 'namespace dxvk {\n', 'namespace dxvk {\n\n' + POLICY)
    before = '''    layoutInfo.bindingCount = bindingInfos.size();
    layoutInfo.pBindings = bindingInfos.data();
'''
    change(name, before, before + '''
    std::vector<VkDescriptorBindingFlags> bindingFlags(bindingInfos.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    if (frostpunkEmulatesRobustness2(m_device)) {
      flagsInfo.bindingCount = bindingFlags.size();
      flagsInfo.pBindingFlags = bindingFlags.data();
      layoutInfo.pNext = &flagsInfo;
    }
''')
    name = FILES[2]
    before = '    template<VkPipelineBindPoint BindPoint>\n    void updateResourceBindings(const DxvkBindingLayoutObjects* layout);\n'
    change(name, before, '    DxvkBufferSlice createRobustnessMetadata(const DxvkBindingLayout& layout);\n\n' + before)
    name = FILES[3]
    change(name, 'namespace dxvk {\n', 'namespace dxvk {\n\n' + POLICY)
    anchor = '  template<VkPipelineBindPoint BindPoint>\n  void DxvkContext::updateResourceBindings(const DxvkBindingLayoutObjects* layout) {\n'
    change(name, anchor, METADATA + anchor)
    before = '    constexpr bool useDescriptorTemplates = env::is32BitHostPlatform();\n'
    change(name, before, '''    bool emulateRobustness2 = frostpunkEmulatesRobustness2(m_device.ptr());
    bool useDescriptorTemplates = env::is32BitHostPlatform() && !emulateRobustness2;
''')
    before = '    dirtySetMask &= layoutSetMask;\n'
    change(name, before, before + '''
    DxvkBufferSlice robustnessMetadata;
    if (emulateRobustness2 && dirtySetMask) {
      // FS constant buffers and views occupy different sets, but share one metadata table.
      dirtySetMask = layoutSetMask;
      robustnessMetadata = this->createRobustnessMetadata(bindings);
    }
''')
    before = '        auto& descriptorInfo = m_descriptors[descriptorCount++];\n\n        switch (binding.descriptorType) {\n'
    change(name, before, '''        auto& descriptorInfo = m_descriptors[descriptorCount];
        if (emulateRobustness2 && binding.resourceBinding == MaxNumResourceSlots) {
          if (binding.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            throw DxvkError("Robustness ABI: metadata descriptor must be a storage buffer");
          descriptorInfo = robustnessMetadata.getDescriptor();
          descriptorCount++;
          continue;
        }
        if (binding.resourceBinding >= MaxNumResourceSlots)
          throw DxvkError("Robustness ABI: resource slot exceeds CPU array bounds");

        switch (binding.descriptorType) {
''')
    before = '''          default:
            break;
        }
      }

      if (useDescriptorTemplates) {
'''
    change(name, before, '''          default:
            break;
        }
        if (emulateRobustness2) {
          const auto* metadata = static_cast<const uint32_t*>(robustnessMetadata.mapPtr(0));
          if (!(metadata[binding.resourceBinding * 2] & 1u))
            continue; // Leave the PARTIALLY_BOUND descriptor unwritten; never write null handles.
        }
        descriptorCount++;
      }

      if (useDescriptorTemplates) {
''')
    before = '''        const DxvkBindingInfo& binding = layout.getBinding(i, j);
        const DxvkShaderResourceSlot& slot = m_rc[binding.resourceBinding];
'''
    change(name, before, '''        const DxvkBindingInfo& binding = layout.getBinding(i, j);
        if (binding.resourceBinding == MaxNumResourceSlots && frostpunkEmulatesRobustness2(m_device.ptr()))
          continue; // Immutable host-written metadata is retained separately, never in m_rc.
        if (binding.resourceBinding >= MaxNumResourceSlots)
          throw DxvkError("Robustness ABI: barrier resource slot exceeds CPU array bounds");
        const DxvkShaderResourceSlot& slot = m_rc[binding.resourceBinding];
''', 2)
    for name, value in contents.items():
        path = work / name
        data = value.encode('utf-8')
        if not path.exists() or path.read_bytes() != data:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
