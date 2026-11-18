/* Device side of the build-time shader library (shaderlib.cpp, layout in ../shaderlib/shaderlib.h). */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "../shaderlib/shaderlib.h"

namespace rdvk {
struct ShaderlibEntry {
    shaderlib::Shader shader;
    const uint8_t* spirv;          /* in the mapped APK range: raw words or a zlib stream */
    uint32_t spirv_words;
    uint32_t packed_bytes;         /* 0: raw */
    const uint8_t* bindings;       /* shaderlib::Binding[binding_count], read with memcpy */
    uint32_t binding_count;
    const uint8_t* uniform;        /* immediate constant buffer bytes or null */
};

/* Maps the packaged library; false (with a log line) when the APK has none or another build's. */
bool shaderlib_open();
/* The SPIR-V words of an entry: in place, or inflated into `scratch` (system libz). */
const uint32_t* shaderlib_spirv(const ShaderlibEntry& entry, std::vector<uint32_t>& scratch);
/* The translation of one DXBC container for this device's minStorageBufferOffsetAlignment. */
bool shaderlib_find(const uint8_t* dxbc, size_t size, uint64_t hash, uint32_t alignment, ShaderlibEntry* out);
}  // namespace rdvk
