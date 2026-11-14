"""Pinned DXBC guards for the Android Mali robustness2 fallback.

Compose after dxvk_mali_overlay. This compiler half does not enable the Vulkan
fallback: the descriptor/backend half must bind the metadata buffer and omit
unbound descriptor writes. Original reference sources remain unchanged.
"""
from pathlib import Path
import hashlib

FILES = tuple('src/dxbc/' + name for name in (
    'dxbc_options.h', 'dxbc_options.cpp', 'dxbc_compiler.h', 'dxbc_compiler.cpp'))
METADATA_BINDING = 1216
METADATA_ENTRIES = 1216
METADATA_STRIDE = 8
INPUT_SHA256 = {
    'src/dxbc/dxbc_options.h': 'f9c2849aa5a2ecaaf58a3c42849813b826559b9b49e2aef9388d2de99dc6d74e',
    'src/dxbc/dxbc_options.cpp': '7f9afa9937c104e13eebbe97d510bd757fd585a1138d5e2108f37bcc3b909ac9',
    'src/dxbc/dxbc_compiler.h': 'cde65f9be940ef3fc55e0209c8363e7f7d01e0f4a2dd4b4f49e940339b65d0b4',
    'src/dxbc/dxbc_compiler.cpp': '1e536406d236cf6a32b07caa2624308d79f0c29f9d86d55fc94dedd225233fd9',
}


HELPERS = r'''
  // FROSTPUNK_ROBUSTNESS_ABI_1
  // ABI: logical binding 1216, readonly std430 uvec2 records[1216].
  // x bit0 = compatible descriptor populated; y = actual descriptor byte range.
  // The metadata descriptor is always populated; it is never indexed by shader
  // input. Resource descriptors may be unwritten only with PARTIALLY_BOUND.
  uint32_t DxbcCompiler::emitRobustnessMetadata(uint32_t binding, uint32_t word) {
    if (binding >= 1216 || word > 1)
      throw DxvkError("Dxbc: invalid robustness metadata index");
    uint32_t uintType = m_module.defIntType(32, 0);
    if (!m_robustnessMetadata) {
      uint32_t pairType = m_module.defVectorType(uintType, 2);
      uint32_t arrayType = m_module.defArrayTypeUnique(pairType, m_module.constu32(1216));
      uint32_t blockType = m_module.defStructTypeUnique(1, &arrayType);
      m_module.decorateArrayStride(arrayType, 8);
      m_module.decorate(blockType, spv::DecorationBlock);
      m_module.memberDecorateOffset(blockType, 0, 0);
      m_robustnessMetadata = m_module.newVar(
        m_module.defPointerType(blockType, spv::StorageClassStorageBuffer),
        spv::StorageClassStorageBuffer);
      m_module.decorate(m_robustnessMetadata, spv::DecorationNonWritable);
      m_module.decorateDescriptorSet(m_robustnessMetadata, 0);
      m_module.decorateBinding(m_robustnessMetadata, 1216);
      m_module.setDebugName(m_robustnessMetadata, "dxvk_robustness_metadata");
      DxvkBindingInfo info = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
      info.resourceBinding = 1216;
      info.viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM;
      info.access = VK_ACCESS_SHADER_READ_BIT;
      m_bindings.push_back(info);
    }
    uint32_t indices[] = { m_module.constu32(0), m_module.constu32(binding), m_module.constu32(word) };
    return m_module.opLoad(uintType, m_module.opAccessChain(
      m_module.defPointerType(uintType, spv::StorageClassStorageBuffer),
      m_robustnessMetadata, 3, indices));
  }

  uint32_t DxbcCompiler::getRobustnessBinding(const DxbcRegister& reg) {
    uint32_t index = reg.idx[0].offset;
    switch (reg.type) {
      case DxbcOperandType::ConstantBuffer: return computeConstantBufferBinding(m_programInfo.type(), index);
      case DxbcOperandType::Resource: return computeSrvBinding(m_programInfo.type(), index);
      case DxbcOperandType::UnorderedAccessView: return computeUavBinding(m_programInfo.type(), index);
      default: throw DxvkError("Dxbc: unsupported robustness resource register");
    }
  }

  uint32_t DxbcCompiler::emitRobustnessBound(uint32_t binding) {
    return m_module.opINotEqual(m_module.defBoolType(),
      m_module.opBitwiseAnd(m_module.defIntType(32, 0),
        emitRobustnessMetadata(binding, 0), m_module.constu32(1)), m_module.constu32(0));
  }

  DxbcConditional DxbcCompiler::emitRobustnessBegin(uint32_t condition) {
    DxbcConditional labels;
    labels.labelIf = m_module.allocateId();
    labels.labelElse = m_module.allocateId();
    labels.labelEnd = m_module.allocateId();
    m_module.opSelectionMerge(labels.labelEnd, spv::SelectionControlMaskNone);
    m_module.opBranchConditional(condition, labels.labelIf, labels.labelElse);
    m_module.opLabel(labels.labelIf);
    return labels;
  }

  DxbcRegisterValue DxbcCompiler::emitRobustnessMerge(
          DxbcConditional labels, DxbcRegisterValue value) {
    // This helper is used only around one non-branching load. The predecessor
    // of the merge is therefore labelIf; nested operations use their own guard.
    m_module.opBranch(labels.labelEnd);
    m_module.opLabel(labels.labelElse);
    DxbcRegisterValue zero = emitBuildZeroVector(value.type);
    m_module.opBranch(labels.labelEnd);
    m_module.opLabel(labels.labelEnd);
    SpirvPhiLabel incoming[] = { {value.id, labels.labelIf}, {zero.id, labels.labelElse} };
    value.id = m_module.opPhi(getVectorTypeId(value.type), 2, incoming);
    return value;
  }

  const DxbcRegister* DxbcCompiler::getRobustnessResource(const DxbcShaderInstruction& ins) {
    switch (ins.opClass) {
      case DxbcInstClass::Atomic: return &ins.dst[ins.dstCount - 1];
      case DxbcInstClass::AtomicCounter: return &ins.dst[1];
      case DxbcInstClass::BufferQuery: return &ins.src[0];
      case DxbcInstClass::BufferLoad:
        return &ins.src[ins.op == DxbcOpcode::LdStructured || ins.op == DxbcOpcode::LdStructuredS ? 2 : 1];
      case DxbcInstClass::BufferStore:
      case DxbcInstClass::TypedUavStore: return &ins.dst[0];
      case DxbcInstClass::TextureQuery:
      case DxbcInstClass::TextureQueryLod:
      case DxbcInstClass::TextureFetch:
      case DxbcInstClass::TextureSample:
      case DxbcInstClass::TypedUavLoad: return &ins.src[1];
      case DxbcInstClass::TextureGather: {
        bool extended = ins.op == DxbcOpcode::Gather4Po || ins.op == DxbcOpcode::Gather4PoC
          || ins.op == DxbcOpcode::Gather4PoS || ins.op == DxbcOpcode::Gather4PoCS;
        return &ins.src[1 + extended];
      }
      case DxbcInstClass::TextureQueryMs:
      case DxbcInstClass::TextureQueryMsPos: return &ins.src[0];
      default: return nullptr;
    }
  }

  void DxbcCompiler::emitRobustnessZeroDestinations(const DxbcShaderInstruction& ins) {
    for (uint32_t i = 0; i < ins.dstCount; i++) {
      auto type = ins.dst[i].type;
      if (type == DxbcOperandType::UnorderedAccessView
       || type == DxbcOperandType::ThreadGroupSharedMemory || type == DxbcOperandType::Null)
        continue;
      emitRegisterStore(ins.dst[i], emitBuildZeroVector({DxbcScalarType::Uint32, ins.dst[i].mask.popCount()}));
    }
  }

  // A masked raw vector may straddle the end of a view. Check each dword,
  // retaining the valid lanes; never guard the whole vector as one access.
  uint32_t DxbcCompiler::emitRobustnessBufferCondition(
    const DxbcRegister& resource, DxbcRegisterValue index,
    DxbcRegisterValue offset, uint32_t component, uint32_t flatIndex) {
    uint32_t uintType = m_module.defIntType(32, 0);
    uint32_t boolType = m_module.defBoolType();
    uint32_t bytes = emitRobustnessMetadata(getRobustnessBinding(resource), 1);
    uint32_t count = m_module.opShiftRightLogical(uintType, bytes, m_module.constu32(2));
    uint32_t flat = m_module.opBitcast(uintType, flatIndex);
    uint32_t valid = m_module.opULessThan(boolType, flat, count);
    const DxbcBufferInfo info = getBufferInfo(resource);
    if (info.type == DxbcResourceType::Structured) {
      if (!info.stride || (info.stride & 3))
        throw DxvkError("Dxbc: invalid structured robustness stride");
      uint32_t structures = m_module.opUDiv(uintType, bytes, m_module.constu32(info.stride));
      uint32_t idx = emitRegisterBitcast(index, DxbcScalarType::Uint32).id;
      uint32_t off = emitRegisterBitcast(offset, DxbcScalarType::Uint32).id;
      off = m_module.opShiftRightLogical(uintType, off, m_module.constu32(2));
      off = m_module.opIAdd(uintType, off, m_module.constu32(component));
      valid = m_module.opLogicalAnd(boolType, valid,
        m_module.opLogicalAnd(boolType,
          m_module.opULessThan(boolType, idx, structures),
          m_module.opULessThan(boolType, off, m_module.constu32(info.stride / 4))));
    }
    return valid;
  }

  uint32_t DxbcCompiler::emitRobustnessImageCondition(
    const DxbcShaderInstruction& ins, const DxbcRegister& resource) {
    const DxbcBufferInfo info = getBufferInfo(resource);
    if (info.image.dim == spv::DimCube)
      throw DxvkError("Dxbc: integer cube access is unavailable in robustness fallback");
    uint32_t boolType = m_module.defBoolType();
    uint32_t uintType = m_module.defIntType(32, 0);
    uint32_t valid = m_module.constBool(true);
    DxbcRegisterValue address = emitRegisterLoad(ins.src[0], DxbcRegMask::firstN(4));
    DxbcRegisterValue coord = emitRegisterBitcast(emitCalcTexCoord(address, info.image), DxbcScalarType::Uint32);
    DxbcRegisterValue lod = {{DxbcScalarType::Uint32, 1}, m_module.constu32(0)};
    bool fetch = ins.opClass == DxbcInstClass::TextureFetch;
    bool ldms = ins.op == DxbcOpcode::LdMs || ins.op == DxbcOpcode::LdMsS;
    if (fetch && info.image.dim != spv::DimBuffer && !info.image.ms && !ldms) {
      lod = emitRegisterBitcast(emitRegisterExtract(address, DxbcRegMask::select(3)), DxbcScalarType::Uint32);
      uint32_t levels = emitQueryTextureLods(resource).id;
      valid = m_module.opULessThan(boolType, lod.id, levels);
      // OpImageQuerySizeLod is itself undefined for an invalid LOD. Query a
      // valid level while preserving the original validity for the real load.
      lod.id = m_module.opSelect(uintType, valid, lod.id, m_module.constu32(0));
    }
    DxbcRegisterValue size = emitQueryTextureSize(resource, lod);
    if (coord.type.ccount != size.type.ccount)
      throw DxvkError("Dxbc: incompatible integer image bounds dimensions");
    for (uint32_t i = 0; i < coord.type.ccount; i++) {
      uint32_t c = emitRegisterExtract(coord, DxbcRegMask::select(i)).id;
      uint32_t extent = emitRegisterExtract(size, DxbcRegMask::select(i)).id;
      if (fetch && i < getTexLayerDim(info.image)) {
        int32_t delta = i == 0 ? ins.sampleControls.u : i == 1 ? ins.sampleControls.v : ins.sampleControls.w;
        if (delta)
          c = m_module.opIAdd(uintType, c, m_module.constu32(uint32_t(delta)));
      }
      valid = m_module.opLogicalAnd(boolType, valid, m_module.opULessThan(boolType, c, extent));
    }
    if (fetch && ldms && info.image.ms) {
      DxbcRegisterValue sample = emitRegisterBitcast(
        emitRegisterLoad(ins.src[2], DxbcRegMask::select(0)), DxbcScalarType::Uint32);
      valid = m_module.opLogicalAnd(boolType, valid,
        m_module.opULessThan(boolType, sample.id, emitQueryTextureSamples(resource).id));
    }
    return valid;
  }

  void DxbcCompiler::processInstruction(const DxbcShaderInstruction& ins) {
    const DxbcRegister* resource = m_moduleInfo.options.emulateRobustness2 ? getRobustnessResource(ins) : nullptr;
    if (!resource || resource->type == DxbcOperandType::ThreadGroupSharedMemory
                  || resource->type == DxbcOperandType::Rasterizer) {
      processInstructionUnchecked(ins);
      return;
    }
    // Sparse instructions require a separate residency-code proof. D3D11 tiled
    // resources are not part of this bounded native Mali profile.
    bool sparse = ins.dstCount == 2 && ins.opClass != DxbcInstClass::Atomic
                                  && ins.opClass != DxbcInstClass::AtomicCounter;
    if (sparse)
      throw DxvkError("Dxbc: sparse resource instruction unavailable in robustness fallback");
    uint32_t bound = emitRobustnessBound(getRobustnessBinding(*resource));
    if (ins.opClass == DxbcInstClass::AtomicCounter) {
      uint32_t counter = computeUavCounterBinding(m_programInfo.type(), resource->idx[0].offset);
      uint32_t validCounter = m_module.opLogicalAnd(m_module.defBoolType(), emitRobustnessBound(counter),
        m_module.opUGreaterThanEqual(m_module.defBoolType(), emitRobustnessMetadata(counter, 1), m_module.constu32(4)));
      bound = m_module.opLogicalAnd(m_module.defBoolType(), bound, validCounter);
    }
    DxbcConditional outer = emitRobustnessBegin(bound);
    uint32_t access = 0;
    if (ins.opClass == DxbcInstClass::TextureFetch || ins.opClass == DxbcInstClass::TypedUavLoad
     || ins.opClass == DxbcInstClass::TypedUavStore)
      access = emitRobustnessImageCondition(ins, *resource);
    if (ins.opClass == DxbcInstClass::Atomic) {
      DxbcBufferInfo info = getBufferInfo(*resource);
      if (info.type == DxbcResourceType::Typed) {
        access = emitRobustnessImageCondition(ins, *resource);
      } else {
        DxbcRegisterValue index = emitRegisterLoad(ins.src[0], DxbcRegMask::select(0));
        DxbcRegisterValue offset = index;
        DxbcRegisterValue flat;
        if (info.type == DxbcResourceType::Structured) {
          DxbcRegisterValue pair = emitRegisterLoad(ins.src[0], DxbcRegMask::firstN(2));
          offset = emitRegisterExtract(pair, DxbcRegMask::select(1));
          flat = emitCalcBufferIndexStructured(index, offset, info.stride);
        } else {
          flat = emitCalcBufferIndexRaw(offset);
        }
        access = emitRobustnessBufferCondition(*resource, index, offset, 0, flat.id);
      }
    }
    DxbcConditional inner;
    if (access)
      inner = emitRobustnessBegin(access);
    processInstructionUnchecked(ins);
    if (access) {
      m_module.opBranch(inner.labelEnd);
      m_module.opLabel(inner.labelElse);
      emitRobustnessZeroDestinations(ins);
      m_module.opBranch(inner.labelEnd);
      m_module.opLabel(inner.labelEnd);
    }
    m_module.opBranch(outer.labelEnd);
    m_module.opLabel(outer.labelElse);
    // Do this only in the skipped branch: dst can alias an address/source reg.
    emitRobustnessZeroDestinations(ins);
    m_module.opBranch(outer.labelEnd);
    m_module.opLabel(outer.labelEnd);
  }
'''


def apply(work, source=None):
    work, source = Path(work), Path(source or work)
    for name, expected in INPUT_SHA256.items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise ValueError('Robustness shader overlay pinned input differs: ' + name)
    data = {name: (source / name).read_text(encoding='utf-8') for name in FILES}

    def change(name, before, after, count=1):
        if data[name].count(before) != count:
            raise ValueError('Robustness shader overlay preimage mismatch: ' + name + ': ' + before[:65])
        data[name] = data[name].replace(before, after)

    opt_h, opt_cpp, hdr, cpp = FILES
    change(opt_h, '    bool supportsRawAccessChains = false;',
           '    bool supportsRawAccessChains = false;\n\n    // Requires matching partially-bound descriptor + metadata backend.\n    bool emulateRobustness2 = false;')
    anchor = '    supportsRawAccessChains = device->features().nvRawAccessChains.shaderRawAccessChains;'
    change(opt_cpp, anchor, anchor + '''
#if defined(__ANDROID__)
    emulateRobustness2 = adapter->deviceProperties().vendorID == 0x13b5
      && !devFeatures.extRobustness2.robustBufferAccess2;
    if (emulateRobustness2)
      supportsRawAccessChains = false;
#endif''')
    anchor = '    DxbcModuleInfo      m_moduleInfo;'
    declarations = '''    // The reserved metadata binding is not a guest resource slot.
    uint32_t m_robustnessMetadata = 0;
    uint32_t m_robustnessImmConstCount = 0;
    uint32_t emitRobustnessMetadata(uint32_t binding, uint32_t word);
    uint32_t getRobustnessBinding(const DxbcRegister& reg);
    uint32_t emitRobustnessBound(uint32_t binding);
    DxbcConditional emitRobustnessBegin(uint32_t condition);
    DxbcRegisterValue emitRobustnessMerge(DxbcConditional labels, DxbcRegisterValue value);
    const DxbcRegister* getRobustnessResource(const DxbcShaderInstruction& ins);
    void emitRobustnessZeroDestinations(const DxbcShaderInstruction& ins);
    uint32_t emitRobustnessBufferCondition(const DxbcRegister& resource,
      DxbcRegisterValue index, DxbcRegisterValue offset, uint32_t component, uint32_t flatIndex);
    uint32_t emitRobustnessImageCondition(const DxbcShaderInstruction& ins, const DxbcRegister& resource);
    void processInstructionUnchecked(const DxbcShaderInstruction& ins);

'''
    change(hdr, anchor, declarations + anchor)
    anchor = '  void DxbcCompiler::processInstruction(const DxbcShaderInstruction& ins) {'
    change(cpp, anchor, HELPERS + '\n  void DxbcCompiler::processInstructionUnchecked(const DxbcShaderInstruction& ins) {')
    # Keep raw-access-chain grouping disabled even for manually constructed options.
    change(cpp, 'bool useRawAccessChains = m_hasRawAccessChains && isSsbo && !imageOperands.sparse;',
           'bool useRawAccessChains = !m_moduleInfo.options.emulateRobustness2 && m_hasRawAccessChains && isSsbo && !imageOperands.sparse;')
    change(cpp, 'bool useRawAccessChains = isSsbo && m_hasRawAccessChains;',
           'bool useRawAccessChains = !m_moduleInfo.options.emulateRobustness2 && isSsbo && m_hasRawAccessChains;')
    # One scalar component access, guarded before pointer construction / load.
    before = '''        if (isTgsm) {
          ccomps[sindex] = m_module.opLoad'''
    change(cpp, before, '''        DxbcConditional robustness;
        if (m_moduleInfo.options.emulateRobustness2 && !isTgsm)
          robustness = emitRobustnessBegin(emitRobustnessBufferCondition(
            srcReg, index, offset, sindex, elementIndexAdjusted));

''' + before)
    before = '''        readMask &= readMask - 1;'''
    change(cpp, before, '''        if (m_moduleInfo.options.emulateRobustness2 && !isTgsm) {
          DxbcRegisterValue scalar = {{DxbcScalarType::Uint32, 1}, ccomps[sindex]};
          ccomps[sindex] = emitRobustnessMerge(robustness, scalar).id;
        }

''' + before)
    before = '''        if (isTgsm) {
          m_module.opStore('''
    change(cpp, before, '''        DxbcConditional robustness;
        if (m_moduleInfo.options.emulateRobustness2 && !isTgsm)
          robustness = emitRobustnessBegin(emitRobustnessBufferCondition(
            dstReg, index, offset, sindex, elementIndexAdjusted));

''' + before)
    before = '''        writeMask &= writeMask - 1u;'''
    change(cpp, before, '''        if (m_moduleInfo.options.emulateRobustness2 && !isTgsm) {
          m_module.opBranch(robustness.labelEnd);
          m_module.opLabel(robustness.labelElse);
          m_module.opBranch(robustness.labelEnd);
          m_module.opLabel(robustness.labelEnd);
        }

''' + before)
    # UBO register loads can occur in every ALU instruction, outside the wrapper.
    # Construct no UBO pointer before the condition. Component loads allow a
    # non-16-byte descriptor range without reading its following allocation.
    before = '''    DxbcRegisterPointer ptr;
    ptr.type.ctype  = info.type.ctype;
    ptr.type.ccount = info.type.ccount;
    ptr.id = m_module.opAccessChain(ptrTypeId,
      m_constantBuffers.at(regId).varId,
      indices.size(), indices.data());'''
    change(cpp, before, '''    DxbcRegisterPointer ptr;
    ptr.type.ctype  = info.type.ctype;
    ptr.type.ccount = info.type.ccount;
    if (!m_moduleInfo.options.emulateRobustness2)
      ptr.id = m_module.opAccessChain(ptrTypeId,
        m_constantBuffers.at(regId).varId, indices.size(), indices.data());''')
    before = '''      uint32_t componentId = m_module.constu32(sindex);
      uint32_t componentPtr = m_module.opAccessChain('''
    change(cpp, before, '''      DxbcConditional robustness;
      if (m_moduleInfo.options.emulateRobustness2) {
        uint32_t uintType = m_module.defIntType(32, 0);
        uint32_t boolType = m_module.defBoolType();
        uint32_t binding = getRobustnessBinding(reg);
        uint32_t words = m_module.opShiftRightLogical(uintType,
          emitRobustnessMetadata(binding, 1), m_module.constu32(2));
        uint32_t vec = emitRegisterBitcast(constId, DxbcScalarType::Uint32).id;
        // Division and subtraction avoid overflow of vec*4 + component.
        uint32_t complete = m_module.opULessThan(boolType, vec,
          m_module.opShiftRightLogical(uintType, words, m_module.constu32(2)));
        uint32_t partial = m_module.opLogicalAnd(boolType,
          m_module.opIEqual(boolType, vec, m_module.opShiftRightLogical(uintType, words, m_module.constu32(2))),
          m_module.opULessThan(boolType, m_module.constu32(sindex),
            m_module.opBitwiseAnd(uintType, words, m_module.constu32(3))));
        uint32_t valid = m_module.opLogicalAnd(boolType, emitRobustnessBound(binding),
          m_module.opLogicalOr(boolType, complete, partial));
        // The SPIR-V declaration has an independent finite array bound.
        valid = m_module.opLogicalAnd(boolType, valid,
          m_module.opULessThan(boolType, vec, m_module.constu32(m_constantBuffers.at(regId).size)));
        robustness = emitRobustnessBegin(valid);
        ptr.id = m_module.opAccessChain(ptrTypeId,
          m_constantBuffers.at(regId).varId, indices.size(), indices.data());
      }
      uint32_t componentId = m_module.constu32(sindex);
      uint32_t componentPtr = m_module.opAccessChain(''')
    before = '''      ccomps[sindex] = m_module.opLoad(
        getScalarTypeId(DxbcScalarType::Float32),
        componentPtr);'''
    change(cpp, before, before + '''
      if (m_moduleInfo.options.emulateRobustness2) {
        DxbcRegisterValue scalar = {{DxbcScalarType::Float32, 1}, ccomps[sindex]};
        ccomps[sindex] = emitRobustnessMerge(robustness, scalar).id;
      }''')
    # Immediate constant buffers larger than sixteen dwords use the same UBO
    # mechanism, but enter through a different operand class. Smaller baked
    # arrays need a finite private-array guard instead of metadata.
    before = '''  void DxbcCompiler::emitDclImmediateConstantBuffer(const DxbcShaderInstruction& ins) {'''
    change(cpp, before, before + '''
    m_robustnessImmConstCount = ins.customDataSize / 4;''')
    before = '''    } else if (reg.type == DxbcOperandType::ConstantBuffer) {
      return emitConstantBufferLoad(reg, writeMask);'''
    change(cpp, before, '''    } else if (m_moduleInfo.options.emulateRobustness2
            && reg.type == DxbcOperandType::ImmediateConstantBuffer
            && m_constantBuffers.at(Icb_BindingSlotId).varId) {
      DxbcRegister uniform = reg;
      uniform.type = DxbcOperandType::ConstantBuffer;
      uniform.idxDim = 2;
      uniform.idx[1] = reg.idx[0];
      uniform.idx[0] = {nullptr, int32_t(Icb_BindingSlotId)};
      return emitConstantBufferLoad(uniform, writeMask);
''' + before)
    before = '''  DxbcRegisterValue DxbcCompiler::emitRegisterLoadRaw(
    const DxbcRegister&           reg) {
    if (reg.type == DxbcOperandType::IndexableTemp) {'''
    change(cpp, before, '''  DxbcRegisterValue DxbcCompiler::emitRegisterLoadRaw(
    const DxbcRegister&           reg) {
    if (m_moduleInfo.options.emulateRobustness2 && reg.type == DxbcOperandType::ImmediateConstantBuffer) {
      if (!m_immConstBuf)
        throw DxvkError("Dxbc: robustness immediate constant buffer is not declared");
      DxbcRegisterValue index = emitRegisterBitcast(emitIndexLoad(reg.idx[0]), DxbcScalarType::Uint32);
      DxbcConditional cond = emitRobustnessBegin(m_module.opULessThan(m_module.defBoolType(),
        index.id, m_module.constu32(m_robustnessImmConstCount)));
      DxbcRegisterInfo ptrInfo = {{DxbcScalarType::Uint32, 4, 0}, spv::StorageClassPrivate};
      DxbcRegisterPointer ptr = {{DxbcScalarType::Uint32, 4},
        m_module.opAccessChain(getPointerTypeId(ptrInfo), m_immConstBuf, 1, &index.id)};
      return emitRobustnessMerge(cond, emitValueLoad(ptr));
    }
    if (reg.type == DxbcOperandType::IndexableTemp) {''')
    # Existing resinfo checked LOD after OpImageQuerySizeLod. Prevent that
    # undefined query as well, while retaining the existing zero/rcp semantics.
    before = '''    DxbcRegisterValue imageSize   = emitQueryTextureSize(ins.src[1], mipLod);'''
    change(cpp, before, '''    DxbcRegisterValue safeMip = mipLod;
    if (m_moduleInfo.options.emulateRobustness2 && resourceInfo.image.sampled == 1 && !resourceInfo.image.ms) {
      // Decoded resinfo LODs can be signed; all Select operands must match.
      safeMip = emitRegisterBitcast(mipLod, DxbcScalarType::Uint32);
      uint32_t levels = emitQueryTextureLods(ins.src[1]).id;
      safeMip.id = m_module.opSelect(getVectorTypeId(safeMip.type),
        m_module.opULessThan(m_module.defBoolType(), safeMip.id, levels), safeMip.id, m_module.constu32(0));
    }
    DxbcRegisterValue imageSize   = emitQueryTextureSize(ins.src[1], safeMip);''')
    before = '''    if (info.image.ms == 0 && info.image.sampled == 1) {
      result.id = m_module.opImageQuerySizeLod('''
    change(cpp, before, '''    if (info.image.ms == 0 && info.image.sampled == 1
     && (!m_moduleInfo.options.emulateRobustness2 || info.image.dim != spv::DimBuffer)) {
      result.id = m_module.opImageQuerySizeLod(''')
    before = '''    if (info.image.ms == 0 && info.image.sampled == 1) {
      result.id = m_module.opImageQueryLevels('''
    change(cpp, before, '''    if (info.image.ms == 0 && info.image.sampled == 1
     && (!m_moduleInfo.options.emulateRobustness2 || info.image.dim != spv::DimBuffer)) {
      result.id = m_module.opImageQueryLevels(''')
    for name, contents in data.items():
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents, encoding='utf-8', newline='\n')
    return dict(files=list(FILES), input_sha256=INPUT_SHA256, metadata_binding=METADATA_BINDING,
                metadata_entries=METADATA_ENTRIES, metadata_stride=METADATA_STRIDE,
                sparse_instructions='explicitly rejected', upstream_sources_modified=False,
                resource_operation_classes=14,
                limitations=['Not complete D3D11/FL11 conformance',
                             'Fixed-function vertex/index fetch retains native core robustness; no vertex-pulling fallback',
                             'Sparse/tiled resource operations explicitly rejected',
                             'Invalid integer cube image accesses explicitly rejected',
                             'Metadata/backend half required before enabling fallback',
                             'Per-draw immutable metadata and PARTIALLY_BOUND descriptors required'],
                output_sha256={name: hashlib.sha256((work / name).read_bytes()).hexdigest() for name in FILES})
