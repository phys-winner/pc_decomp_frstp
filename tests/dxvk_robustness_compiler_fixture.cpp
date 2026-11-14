// Uses the actual pinned DXBC compiler. Only the final DxvkShader container and
// logging/device-options constructors are stubbed; no Vulkan descriptor behavior
// is replaced here. The separately executed Vulkan fixture consumes its SPIR-V.
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "dxbc/dxbc_compiler.h"
#include "dxbc/dxbc_module.h"

namespace dxvk {
  Logger Logger::s_instance("fixture");
  Logger::Logger(const std::string& name) : m_minLevel(LogLevel::None), m_fileName(name) { }
  Logger::~Logger() = default;
  void Logger::trace(const std::string&) { }
  void Logger::debug(const std::string&) { }
  void Logger::info(const std::string&) { }
  void Logger::warn(const std::string& text) { throw DxvkError("Unexpected compiler warning: " + text); }
  void Logger::err(const std::string& text) { throw DxvkError(text + ""); }
  void Logger::log(LogLevel, const std::string&) { }
  DxbcOptions::DxbcOptions() = default;
  DxvkBindingList::DxvkBindingList() = default;
  DxvkBindingList::~DxvkBindingList() = default;
  DxvkBindingLayout::DxvkBindingLayout(VkShaderStageFlags stage)
  : m_pushConst{}, m_pushConstStages(0), m_stages(stage) { }
  DxvkBindingLayout::~DxvkBindingLayout() = default;
  DxvkShaderKey::DxvkShaderKey() = default;
  DxvkShader::DxvkShader(const DxvkShaderCreateInfo& info, SpirvCodeBuffer&& code)
  : m_info(info), m_code(code), m_bindings(info.stage) { }
  DxvkShader::~DxvkShader() = default;
}

using namespace dxvk;

static DxbcRegister reg(DxbcOperandType type, uint32_t index = 0,
                        DxbcScalarType scalar = DxbcScalarType::Uint32) {
  DxbcRegister r{};
  r.type = type; r.dataType = scalar; r.componentCount = DxbcComponentCount::Component4;
  r.idxDim = 1; r.idx[0].offset = index;
  r.mask = DxbcRegMask::firstN(4); r.swizzle = DxbcRegSwizzle(0, 1, 2, 3);
  return r;
}

static DxbcRegister imm(uint32_t x, uint32_t y = 0, uint32_t z = 0, uint32_t w = 0) {
  auto r = reg(DxbcOperandType::Imm32);
  r.idxDim = 0;
  r.imm.u32_4[0] = x; r.imm.u32_4[1] = y; r.imm.u32_4[2] = z; r.imm.u32_4[3] = w;
  return r;
}

static void emit(DxbcCompiler& c, DxbcOpcode op, DxbcInstClass cls,
                 std::vector<DxbcRegister> dst = {}, std::vector<DxbcRegister> src = {},
                 std::vector<uint32_t> words = {}, uint32_t controls = 0) {
  std::vector<DxbcImmediate> values;
  for (auto word : words) { DxbcImmediate value; value.u32 = word; values.push_back(value); }
  DxbcShaderInstruction ins{};
  ins.op = op; ins.opClass = cls; ins.controls = DxbcShaderOpcodeControls(controls);
  ins.dstCount = uint32_t(dst.size()); ins.dst = dst.data();
  ins.srcCount = uint32_t(src.size()); ins.src = src.data();
  ins.immCount = uint32_t(values.size()); ins.imm = values.data();
  c.processInstruction(ins);
}

static unsigned generated;
static void save(DxbcCompiler& c, const std::string& directory, const std::string& name) {
  auto shader = c.finalize();
  auto code = shader->getRawCode();
  std::ofstream out(directory + "/" + name + ".spv", std::ios::binary);
  out.write(reinterpret_cast<const char*>(code.data()), code.size());
  if (!out) throw DxvkError("Cannot save fixture shader");
  generated++;
}

static DxbcModuleInfo options(bool fallback, bool ssbo = true) {
  DxbcModuleInfo result{};
  result.options.emulateRobustness2 = fallback;
  result.options.minSsboAlignment = ssbo ? 4 : ~0u;
  result.options.supportsTypedUavLoadR32 = true;
  return result;
}

static void initialize(DxbcCompiler& c, bool fragment = false) {
  if (!fragment)
    emit(c, DxbcOpcode::DclThreadGroup, DxbcInstClass::Declaration, {}, {}, {1, 1, 1});
  // A destination aliasing the address/source is intentional.
  emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu,
       {reg(DxbcOperandType::Temp)}, {imm(0, 4, 0, 0)});
}

static void raw_cases(const std::string& directory) {
  for (bool ssbo : {false, true}) for (bool structured : {false, true}) {
    DxbcAnalysisInfo analysis{};
    analysis.uavInfos[0].accessFlags = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    for (unsigned operation = 0; operation < 7; operation++) {
      auto info = options(true, ssbo);
      DxbcCompiler c("robustness", info, DxbcProgramType::ComputeShader, nullptr, nullptr, nullptr, analysis);
      initialize(c);
      auto srv = reg(DxbcOperandType::Resource);
      auto uav = reg(DxbcOperandType::UnorderedAccessView);
      emit(c, structured ? DxbcOpcode::DclResourceStructured : DxbcOpcode::DclResourceRaw,
           DxbcInstClass::Declaration, {srv}, {}, structured ? std::vector<uint32_t>{12} : std::vector<uint32_t>{});
      emit(c, structured ? DxbcOpcode::DclUavStructured : DxbcOpcode::DclUavRaw,
           DxbcInstClass::Declaration, {uav}, {}, structured ? std::vector<uint32_t>{12} : std::vector<uint32_t>{});
      auto temp = reg(DxbcOperandType::Temp);
      if (operation == 0 || operation == 1) {
        auto resource = operation == 0 ? srv : uav;
        emit(c, structured ? DxbcOpcode::LdStructured : DxbcOpcode::LdRaw,
             DxbcInstClass::BufferLoad, {temp}, structured ? std::vector<DxbcRegister>{temp, imm(4), resource}
                                                         : std::vector<DxbcRegister>{temp, resource});
      } else if (operation == 2) {
        emit(c, structured ? DxbcOpcode::StoreStructured : DxbcOpcode::StoreRaw,
             DxbcInstClass::BufferStore, {uav}, structured ? std::vector<DxbcRegister>{temp, imm(4), temp}
                                                         : std::vector<DxbcRegister>{temp, temp});
      } else if (operation == 3) {
        emit(c, DxbcOpcode::BufInfo, DxbcInstClass::BufferQuery, {temp}, {srv});
      } else if (operation == 4) {
        emit(c, DxbcOpcode::ImmAtomicIAdd, DxbcInstClass::Atomic, {temp, uav}, {temp, imm(1)});
      } else if (operation == 5) {
        emit(c, DxbcOpcode::AtomicIAdd, DxbcInstClass::Atomic, {uav}, {temp, imm(1)});
      } else {
        emit(c, DxbcOpcode::ImmAtomicConsume, DxbcInstClass::AtomicCounter, {temp, uav});
      }
      save(c, directory, std::string(ssbo ? "ssbo" : "texel") + (structured ? "-structured-" : "-raw-") + std::to_string(operation));
    }
  }
}

static void ubo_case(const std::string& directory) {
  DxbcAnalysisInfo analysis{};
  auto info = options(true);
  DxbcCompiler c("robustness", info, DxbcProgramType::ComputeShader, nullptr, nullptr, nullptr, analysis);
  initialize(c);
  auto cb = reg(DxbcOperandType::ConstantBuffer, 0, DxbcScalarType::Float32);
  cb.idxDim = 2; cb.idx[1].offset = 4;
  emit(c, DxbcOpcode::DclConstantBuffer, DxbcInstClass::Declaration, {cb});
  auto index = reg(DxbcOperandType::Temp);
  cb.idx[1].relReg = &index;
  emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {reg(DxbcOperandType::Temp)}, {cb});
  save(c, directory, "ubo-dynamic-alias");
}

static void typed_cases(const std::string& directory) {
  for (auto dim : {DxbcResourceDim::Buffer, DxbcResourceDim::Texture1D, DxbcResourceDim::Texture1DArr,
                  DxbcResourceDim::Texture2D, DxbcResourceDim::Texture2DArr, DxbcResourceDim::Texture3D,
                  DxbcResourceDim::Texture2DMs, DxbcResourceDim::Texture2DMsArr}) {
    bool ms = dim == DxbcResourceDim::Texture2DMs || dim == DxbcResourceDim::Texture2DMsArr;
    for (unsigned operation = 0; operation < (ms ? 3u : 6u); operation++) {
      DxbcAnalysisInfo analysis{};
      analysis.uavInfos[0].accessFlags = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      analysis.uavInfos[0].accessAtomicOp = operation == 5;
      auto info = options(true);
      DxbcCompiler c("robustness", info, DxbcProgramType::ComputeShader, nullptr, nullptr, nullptr, analysis);
      initialize(c);
      auto srv = reg(DxbcOperandType::Resource);
      auto uav = reg(DxbcOperandType::UnorderedAccessView);
      auto temp = reg(DxbcOperandType::Temp);
      emit(c, DxbcOpcode::DclResource, DxbcInstClass::Declaration, {srv}, {}, {0x4444}, uint32_t(dim) << 11);
      if (!ms)
        emit(c, DxbcOpcode::DclUavTyped, DxbcInstClass::Declaration, {uav}, {}, {0x4444}, uint32_t(dim) << 11);
      if (operation == 0) {
        emit(c, ms ? DxbcOpcode::LdMs : DxbcOpcode::Ld, DxbcInstClass::TextureFetch,
             {temp}, ms ? std::vector<DxbcRegister>{temp, srv, imm(0)} : std::vector<DxbcRegister>{temp, srv});
      } else if (operation == 1) {
        emit(c, DxbcOpcode::ResInfo, DxbcInstClass::TextureQuery, {temp}, {temp, srv});
      } else if (operation == 2) {
        emit(c, DxbcOpcode::SampleInfo, DxbcInstClass::TextureQueryMs, {temp}, {srv});
      } else if (operation == 3) {
        emit(c, DxbcOpcode::LdUavTyped, DxbcInstClass::TypedUavLoad, {temp}, {temp, uav});
      } else if (operation == 4) {
        emit(c, DxbcOpcode::StoreUavTyped, DxbcInstClass::TypedUavStore, {uav}, {temp, temp});
      } else {
        emit(c, DxbcOpcode::ImmAtomicIAdd, DxbcInstClass::Atomic, {temp, uav}, {temp, imm(1)});
      }
      save(c, directory, "image-" + std::to_string(uint32_t(dim)) + "-" + std::to_string(operation));
    }
  }
}

static void sampling_cases(const std::string& directory) {
  for (unsigned operation = 0; operation < 8; operation++) {
    DxbcAnalysisInfo analysis{};
    auto info = options(true);
    DxbcCompiler c("robustness", info, DxbcProgramType::PixelShader, nullptr, nullptr, nullptr, analysis);
    initialize(c, true);
    auto srv = reg(DxbcOperandType::Resource, 0, DxbcScalarType::Float32);
    auto sampler = reg(DxbcOperandType::Sampler, 0, DxbcScalarType::Float32);
    auto temp = reg(DxbcOperandType::Temp, 0, DxbcScalarType::Float32);
    auto floatZero = imm(0); floatZero.dataType = DxbcScalarType::Float32;
    emit(c, DxbcOpcode::DclResource, DxbcInstClass::Declaration, {srv}, {}, {0x5555}, uint32_t(DxbcResourceDim::Texture2D) << 11);
    emit(c, DxbcOpcode::DclSampler, DxbcInstClass::Declaration, {sampler});
    switch (operation) {
      case 0: emit(c, DxbcOpcode::Sample, DxbcInstClass::TextureSample, {temp}, {temp, srv, sampler}); break;
      case 1: emit(c, DxbcOpcode::SampleC, DxbcInstClass::TextureSample, {temp}, {temp, srv, sampler, floatZero}); break;
      case 2: emit(c, DxbcOpcode::SampleL, DxbcInstClass::TextureSample, {temp}, {temp, srv, sampler, floatZero}); break;
      case 3: emit(c, DxbcOpcode::Gather4, DxbcInstClass::TextureGather, {temp}, {temp, srv, sampler}); break;
      case 4: emit(c, DxbcOpcode::Gather4C, DxbcInstClass::TextureGather, {temp}, {temp, srv, sampler, floatZero}); break;
      case 5: emit(c, DxbcOpcode::Gather4Po, DxbcInstClass::TextureGather, {temp}, {temp, imm(0), srv, sampler}); break;
      case 6: emit(c, DxbcOpcode::Lod, DxbcInstClass::TextureQueryLod, {temp}, {temp, srv, sampler}); break;
      case 7: emit(c, DxbcOpcode::SamplePos, DxbcInstClass::TextureQueryMsPos, {temp}, {srv, imm(0)}); break;
    }
    save(c, directory, "sampling-" + std::to_string(operation));
  }
}

static void proof_case(const std::string& directory) {
  DxbcAnalysisInfo analysis{};
  for (auto& uav : analysis.uavInfos)
    uav.accessFlags = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  auto info = options(true);
  DxbcCompiler c("compiler-proof", info, DxbcProgramType::ComputeShader, nullptr, nullptr, nullptr, analysis);
  initialize(c);
  auto temp = reg(DxbcOperandType::Temp);
  auto srv = reg(DxbcOperandType::Resource);
  auto output = reg(DxbcOperandType::UnorderedAccessView);
  auto missing = reg(DxbcOperandType::UnorderedAccessView, 1);
  auto writable = reg(DxbcOperandType::UnorderedAccessView, 2);
  auto missingCounter = reg(DxbcOperandType::UnorderedAccessView, 3);
  auto structured = reg(DxbcOperandType::Resource, 5);
  for (auto resource : {srv})
    emit(c, DxbcOpcode::DclResourceRaw, DxbcInstClass::Declaration, {resource});
  emit(c, DxbcOpcode::DclResourceStructured, DxbcInstClass::Declaration, {structured}, {}, {12});
  for (auto resource : {output, missing, writable, missingCounter})
    emit(c, DxbcOpcode::DclUavRaw, DxbcInstClass::Declaration, {resource});
  for (unsigned slot = 1; slot <= 4; slot++)
    emit(c, DxbcOpcode::DclResource, DxbcInstClass::Declaration,
         {reg(DxbcOperandType::Resource, slot)}, {}, {0x4444},
         uint32_t(slot <= 2 ? DxbcResourceDim::Texture2D : DxbcResourceDim::Buffer) << 11);
  auto cb = reg(DxbcOperandType::ConstantBuffer);
  cb.idxDim = 2; cb.idx[1].offset = 4;
  emit(c, DxbcOpcode::DclConstantBuffer, DxbcInstClass::Declaration, {cb});
  unsigned row = 0;
  auto saveRow = [&] {
    emit(c, DxbcOpcode::StoreRaw, DxbcInstClass::BufferStore, {output}, {imm(row++ * 16), temp});
  };
  auto rawLoad = [&](DxbcRegister resource, uint32_t address) {
    emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {imm(address)});
    emit(c, DxbcOpcode::LdRaw, DxbcInstClass::BufferLoad, {temp}, {temp, resource}); saveRow();
  };
  rawLoad(srv, 8); rawLoad(srv, ~0u); rawLoad(missing, 0);
  emit(c, DxbcOpcode::BufInfo, DxbcInstClass::BufferQuery, {temp}, {srv}); saveRow();
  emit(c, DxbcOpcode::BufInfo, DxbcInstClass::BufferQuery, {temp}, {missing}); saveRow();
  cb.idx[1].offset = 1;
  emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {cb}); saveRow();
  cb.idx[1].offset = -1;
  emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {cb}); saveRow();
  auto fetch = [&](unsigned slot, uint32_t x) {
    emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {imm(x)});
    emit(c, DxbcOpcode::Ld, DxbcInstClass::TextureFetch,
         {temp}, {temp, reg(DxbcOperandType::Resource, slot)}); saveRow();
  };
  fetch(1, 0); fetch(1, 2); fetch(2, 0); fetch(3, 2); fetch(3, 3); fetch(4, 0);
  emit(c, DxbcOpcode::ImmAtomicConsume, DxbcInstClass::AtomicCounter, {temp, missingCounter}); saveRow();
  for (auto pair : {std::make_pair(writable, 8u), std::make_pair(writable, 12u), std::make_pair(missing, 0u)}) {
    emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {imm(pair.second)});
    emit(c, DxbcOpcode::ImmAtomicIAdd, DxbcInstClass::Atomic, {temp, pair.first}, {temp, imm(1)}); saveRow();
  }
  emit(c, DxbcOpcode::LdStructured, DxbcInstClass::BufferLoad, {temp}, {imm(0), imm(8), structured}); saveRow();
  emit(c, DxbcOpcode::LdStructured, DxbcInstClass::BufferLoad, {temp}, {imm(0x40000000), imm(0), structured}); saveRow();
  emit(c, DxbcOpcode::StoreRaw, DxbcInstClass::BufferStore, {writable}, {imm(8), imm(0xabababab, 0xbcbcbcbc, 0xcdcdcdcd, 0xdededede)});
  rawLoad(writable, 0);
  emit(c, DxbcOpcode::ResInfo, DxbcInstClass::TextureQuery,
       {temp}, {imm(0), reg(DxbcOperandType::Resource, 2)}, {}, uint32_t(DxbcResinfoType::Uint) << 11); saveRow();
  emit(c, DxbcOpcode::ResInfo, DxbcInstClass::TextureQuery,
       {temp}, {imm(0), reg(DxbcOperandType::Resource, 1)}, {}, uint32_t(DxbcResinfoType::Uint) << 11); saveRow();
  emit(c, DxbcOpcode::ResInfo, DxbcInstClass::TextureQuery,
       {temp}, {imm(~0u), reg(DxbcOperandType::Resource, 1)}, {}, uint32_t(DxbcResinfoType::Uint) << 11); saveRow();
  emit(c, DxbcOpcode::SampleInfo, DxbcInstClass::TextureQueryMs,
       {temp}, {reg(DxbcOperandType::Resource, 2)}, {}, 1u << 11); saveRow();
  cb.idx[1].offset = 0;
  emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {cb}); saveRow();
  if (row != 25) throw DxvkError("Unexpected compiler proof output count");
  save(c, directory, "compiler-proof");
}

static void stage_cases(const std::string& directory) {
  for (auto stage : {DxbcProgramType::VertexShader, DxbcProgramType::GeometryShader,
                     DxbcProgramType::HullShader, DxbcProgramType::DomainShader}) {
    DxbcAnalysisInfo analysis{};
    auto info = options(true);
    DxbcCompiler c("stage-proof", info, stage, nullptr, nullptr, nullptr, analysis);
    if (stage == DxbcProgramType::GeometryShader) {
      emit(c, DxbcOpcode::DclGsInputPrimitive, DxbcInstClass::Declaration, {}, {}, {}, uint32_t(DxbcPrimitive::Point) << 11);
      emit(c, DxbcOpcode::DclGsOutputPrimitiveTopology, DxbcInstClass::Declaration, {}, {}, {}, uint32_t(DxbcPrimitiveTopology::PointList) << 11);
      emit(c, DxbcOpcode::DclMaxOutputVertexCount, DxbcInstClass::Declaration, {}, {}, {1});
    }
    if (stage == DxbcProgramType::HullShader || stage == DxbcProgramType::DomainShader) {
      emit(c, DxbcOpcode::DclInputControlPointCount, DxbcInstClass::Declaration, {}, {}, {}, 1u << 11);
      if (stage == DxbcProgramType::HullShader)
        emit(c, DxbcOpcode::DclOutputControlPointCount, DxbcInstClass::Declaration, {}, {}, {}, 1u << 11);
      emit(c, DxbcOpcode::DclTessDomain, DxbcInstClass::Declaration, {}, {}, {}, uint32_t(DxbcTessDomain::Triangles) << 11);
      emit(c, DxbcOpcode::DclTessPartitioning, DxbcInstClass::Declaration, {}, {}, {}, uint32_t(DxbcTessPartitioning::Integer) << 11);
      emit(c, DxbcOpcode::DclTessOutputPrimitive, DxbcInstClass::Declaration, {}, {}, {}, uint32_t(DxbcTessOutputPrimitive::TriangleCw) << 11);
      if (stage == DxbcProgramType::HullShader)
        emit(c, DxbcOpcode::HsControlPointPhase, DxbcInstClass::HullShaderPhase);
    }
    auto srv = reg(DxbcOperandType::Resource);
    auto temp = reg(DxbcOperandType::Temp);
    emit(c, DxbcOpcode::DclResourceRaw, DxbcInstClass::Declaration, {srv});
    emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {temp}, {imm(8)});
    emit(c, DxbcOpcode::LdRaw, DxbcInstClass::BufferLoad, {temp}, {temp, srv});
    save(c, directory, "stage-" + std::to_string(uint32_t(stage)));
  }
}

static void immediate_cases(const std::string& directory) {
  for (unsigned count : {16u, 32u}) {
    DxbcAnalysisInfo analysis{};
    auto info = options(true);
    DxbcCompiler c("icb-proof", info, DxbcProgramType::ComputeShader, nullptr, nullptr, nullptr, analysis);
    initialize(c);
    std::vector<uint32_t> data(count, 0x41414141);
    DxbcShaderInstruction custom{};
    custom.op = DxbcOpcode::CustomData; custom.opClass = DxbcInstClass::CustomData;
    custom.customDataType = DxbcCustomDataClass::ImmConstBuf;
    custom.customDataSize = count; custom.customData = data.data();
    c.processInstruction(custom);
    auto index = reg(DxbcOperandType::Temp);
    auto icb = reg(DxbcOperandType::ImmediateConstantBuffer);
    icb.idx[0].relReg = &index;
    for (unsigned offset : {0u, count / 4 - 1, count / 4, ~0u}) {
      emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {index}, {imm(offset)});
      emit(c, DxbcOpcode::Mov, DxbcInstClass::VectorAlu, {index}, {icb});
    }
    save(c, directory, "icb-" + std::to_string(count));
  }
}

static void resinfo_lod_cases(const std::string& directory) {
  for (auto scalar : {DxbcScalarType::Sint32, DxbcScalarType::Uint32})
  for (auto result : {DxbcResinfoType::Float, DxbcResinfoType::Uint, DxbcResinfoType::RcpFloat})
  for (uint32_t lod : {0u, 1u, ~0u}) {
    DxbcAnalysisInfo analysis{};
    auto info = options(true);
    DxbcCompiler c("resinfo-lod", info, DxbcProgramType::ComputeShader, nullptr, nullptr, nullptr, analysis);
    initialize(c);
    auto srv = reg(DxbcOperandType::Resource);
    emit(c, DxbcOpcode::DclResource, DxbcInstClass::Declaration, {srv}, {}, {0x5555},
         uint32_t(DxbcResourceDim::Texture2D) << 11);
    auto immediate = imm(lod); immediate.dataType = scalar;
    emit(c, DxbcOpcode::ResInfo, DxbcInstClass::TextureQuery,
         {reg(DxbcOperandType::Temp)}, {immediate, srv}, {}, uint32_t(result) << 11);
    save(c, directory, "resinfo-lod-" + std::to_string(uint32_t(scalar)) + "-"
         + std::to_string(uint32_t(result)) + "-" + std::to_string(lod));
  }
}

static void compile_capture(const std::string& manifest, const std::string& directory) {
  std::ifstream list(manifest);
  if (!list) throw DxvkError("Cannot open captured shader manifest");
  std::string path;
  while (std::getline(list, path)) {
    if (!path.empty() && path.back() == '\r') path.pop_back();
    if (path.empty()) continue;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw DxvkError("Cannot open captured DXBC: " + path);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    DxbcReader reader(data.data(), data.size());
    DxbcModule module(reader);
    auto info = options(true);
    info.options.invariantPosition = true;
    info.options.floatControl.set(DxbcFloatControlFlag::DenormFlushToZero32);
    info.options.floatControl.set(DxbcFloatControlFlag::PreserveNan32);
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    name.resize(name.size() - 5);
    auto code = module.compile(info, name)->getRawCode();
    std::ofstream out(directory + "/" + name + ".spv", std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), code.size());
    if (!out) throw DxvkError("Cannot save captured shader");
    generated++;
  }
}

int main(int argc, char** argv) {
  if (argc != 2 && !(argc == 4 && std::string(argv[1]) == "--compile-dxbc")) return 2;
  try {
    if (argc == 4) {
      compile_capture(argv[2], argv[3]);
      std::cout << "{\"success\":true,\"generated\":" << generated << "}\n";
      return 0;
    }
    raw_cases(argv[1]); ubo_case(argv[1]); typed_cases(argv[1]); sampling_cases(argv[1]); proof_case(argv[1]);
    stage_cases(argv[1]); immediate_cases(argv[1]);
    resinfo_lod_cases(argv[1]);
    std::cout << "{\"success\":true,\"generated\":" << generated << "}\n";
    return 0;
  } catch (const DxvkError& error) {
    std::cerr << error.message() << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
