#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rdvk {
using ShaderOutputWidths = std::array<uint8_t, 32>;
ShaderOutputWidths shader_output_widths(const uint32_t* code, size_t words);
std::vector<uint32_t> match_shader_inputs(const std::vector<uint32_t>& code, const ShaderOutputWidths& outputs,
                                          uint32_t& locations);
}  // namespace rdvk
