#include "shader_interface.h"
#include <algorithm>
#include <cstring>

namespace rdvk {
namespace {
enum : uint32_t {
    OpExtInstImport = 11, OpExtInst = 12, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpVariable = 59,
    OpLoad = 61, OpDecorate = 71, OpVectorShuffle = 79,
    StorageInput = 1, StorageOutput = 3, DecorationLocation = 30, DecorationComponent = 31,
};

struct Varying {
    uint32_t id = 0;
    uint32_t width = 0;
};

struct Interface {
    std::array<Varying, 32> locations{};
    std::array<uint32_t, 5> vectors{}, pointers{};
    uint32_t float_type = 0;
    uint32_t glsl = 0;
};

Interface read_interface(const uint32_t* code, size_t words, uint32_t storage) {
    if (words < 5 || code[0] != 0x07230203) return {};
    Interface info;
    struct Location { uint32_t id, value; };
    std::vector<Location> locations;
    std::vector<uint32_t> component_ids;
    for (size_t at = 5; at < words;) {
        const uint32_t* ins = code + at;
        uint32_t count = ins[0] >> 16, opcode = ins[0] & 0xffff;
        if (!count || count > words - at) return {};
        if (opcode == OpExtInstImport && count == 6 && !memcmp(ins + 2, "GLSL.std.450", 13)) info.glsl = ins[1];
        if (opcode == OpDecorate && count == 4) {
            if (ins[2] == DecorationLocation && ins[3] < info.locations.size()) locations.push_back({ins[1], ins[3]});
            if (ins[2] == DecorationComponent && ins[3]) component_ids.push_back(ins[1]);
        }
        if (opcode == OpTypeFloat && count == 3 && ins[2] == 32) info.float_type = ins[1];
        if (opcode == OpTypeVector && count == 4 && ins[2] == info.float_type && ins[3] >= 2 && ins[3] <= 4)
            info.vectors[ins[3]] = ins[1];
        if (opcode == OpTypePointer && count == 4 && ins[2] == storage) {
            auto type = std::find(info.vectors.begin() + 2, info.vectors.end(), ins[3]);
            if (type != info.vectors.end()) info.pointers[type - info.vectors.begin()] = ins[1];
        }
        if (opcode == OpVariable && count >= 4 && ins[3] == storage) {
            auto location = std::find_if(locations.begin(), locations.end(), [&](const Location& l) { return l.id == ins[2]; });
            if (location != locations.end()) {
                Varying& slot = info.locations[location->value];
                auto type = std::find(info.pointers.begin() + 2, info.pointers.end(), ins[1]);
                if (slot.id || type == info.pointers.end() ||
                    std::find(component_ids.begin(), component_ids.end(), ins[2]) != component_ids.end())
                    slot.width = 0;
                else
                    slot.width = uint32_t(type - info.pointers.begin());
                slot.id = ins[2];
            }
        }
        at += count;
    }
    return info;
}

struct Widen {
    uint32_t variable, from, to;
};
}  // namespace

ShaderOutputWidths shader_output_widths(const uint32_t* code, size_t words) {
    Interface info = read_interface(code, words, StorageOutput);
    ShaderOutputWidths widths{};
    for (size_t i = 0; i < widths.size(); ++i) widths[i] = uint8_t(info.locations[i].width);
    return widths;
}

std::vector<uint32_t> match_shader_inputs(const std::vector<uint32_t>& code, const ShaderOutputWidths& outputs,
                                          uint32_t& locations) {
    locations = 0;
    Interface info = read_interface(code.data(), code.size(), StorageInput);
    std::vector<Widen> changes;
    std::array<bool, 5> needed{};
    for (size_t i = 0; i < outputs.size(); ++i) {
        Varying v = info.locations[i];
        if (!v.width || outputs[i] <= v.width || outputs[i] > 4) continue;
        changes.push_back({v.id, v.width, outputs[i]});
        needed[outputs[i]] = true;
        locations |= uint32_t(1) << i;
    }
    if (changes.empty()) return {};

    uint32_t next_id = code[3];
    for (uint32_t width = 2; width <= 4; ++width) {
        if (!needed[width]) continue;
        if (!info.vectors[width]) info.vectors[width] = next_id++;
        if (!info.pointers[width]) info.pointers[width] = next_id++;
    }
    std::vector<uint32_t> out(code.begin(), code.begin() + 5);
    out.reserve(code.size() + changes.size() * 12);
    for (size_t at = 5; at < code.size();) {
        const uint32_t* ins = code.data() + at;
        uint32_t count = ins[0] >> 16, opcode = ins[0] & 0xffff;
        at += count;
        /* Move the wider types ahead of all Input variables, retaining an existing type ID. */
        if (opcode == OpTypeVector && count == 4 && ins[2] == info.float_type && ins[3] <= 4 && needed[ins[3]]) continue;
        if (opcode == OpTypePointer && count == 4 && ins[2] == StorageInput) {
            auto type = std::find(info.pointers.begin() + 2, info.pointers.end(), ins[1]);
            if (type != info.pointers.end() && needed[type - info.pointers.begin()]) continue;
        }
        size_t start = out.size();
        out.insert(out.end(), ins, ins + count);
        if (opcode == OpTypeFloat && count == 3 && ins[1] == info.float_type) {
            for (uint32_t width = 2; width <= 4; ++width) {
                if (!needed[width]) continue;
                out.insert(out.end(), {4u << 16 | OpTypeVector, info.vectors[width], info.float_type, width,
                                       4u << 16 | OpTypePointer, info.pointers[width], StorageInput, info.vectors[width]});
            }
        }
        uint32_t variable = 0;
        if (opcode == OpVariable && count >= 4 && ins[3] == StorageInput) variable = ins[2];
        if (opcode == OpLoad && count >= 4) variable = ins[3];
        /* GLSL.std.450 InterpolateAtCentroid, InterpolateAtSample and InterpolateAtOffset. */
        if (opcode == OpExtInst && count >= 6 && ins[3] == info.glsl && ins[4] >= 76 && ins[4] <= 78) variable = ins[5];
        if (!variable) continue;
        auto change = std::find_if(changes.begin(), changes.end(), [&](const Widen& w) { return w.variable == variable; });
        if (change == changes.end()) continue;
        if (opcode == OpVariable) {
            out[start + 1] = info.pointers[change->to];
        } else {
            if (ins[1] != info.vectors[change->from]) { locations = 0; return {}; }
            uint32_t wide = next_id++;
            out[start + 1] = info.vectors[change->to];
            out[start + 2] = wide;
            out.insert(out.end(), {(5 + change->from) << 16 | OpVectorShuffle, ins[1], ins[2], wide, wide});
            for (uint32_t lane = 0; lane < change->from; ++lane) out.push_back(lane);
        }
    }
    out[3] = next_id;
    return out;
}
}  // namespace rdvk
