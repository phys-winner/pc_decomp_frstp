/* Build-time DXBC -> SPIR-V translation for the native rendering device (android/vulkan).
 *
 *     rdvk_shaderc INPUT OUTPUT
 *
 * INPUT is written by tools/build_shaderlib.py: records of {u8 role, u8 pad[3], u32 size, DXBC[size]}
 * (sizes padded to 4).  OUTPUT is the shader library that tools/build_rdvk.py links into
 * libfrostpunk_rdvk.so (layout in shaderlib.h).  The translator is DXVK 2.4.1's DXBC compiler
 * (src/dxbc, src/spirv, zlib licence) with exactly the options the device used to pass at run
 * time; the only device-dependent option, minSsboAlignment, is compiled for every power of two
 * from 4 to 256 and identical results are merged into one variant.  The device therefore holds
 * no shader translator.  The same source builds for aarch64-linux-android to prove on a device
 * that the build-time output equals what the device produced before (tools/build_shaderlib.py
 * verify SERIAL). */
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "dxbc/dxbc_module.h"
#include "dxbc/dxbc_util.h"
#include "shaderlib.h"

namespace rdvk {
static unsigned g_warnings = 0;
void log(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    ++g_warnings;
}
}  // namespace rdvk

namespace {
using namespace rdvk::shaderlib;

/* Identical to the device's former compiler_options() (android/vulkan/lib/shaders.cpp before the
 * build-time library), apart from minSsboAlignment. */
dxvk::DxbcOptions compiler_options(uint32_t min_ssbo_alignment) {
    dxvk::DxbcOptions o;
    o.useDepthClipWorkaround = false;
    o.supportsClipDistance = false;   /* Mali: no shaderClipDistance */
    o.supportsCullDistance = false;
    o.supportsTypedUavLoadR32 = true;
    o.supportsRawAccessChains = false;
    o.emulateRobustness2 = false;
    o.zeroInitWorkgroupMemory = true;
    o.invariantPosition = true;
    o.forceVolatileTgsmAccess = false;
    o.disableMsaa = false;
    o.forceSampleRateShading = false;
    o.enableSampleShadingInterlock = false;
    o.minSsboAlignment = min_ssbo_alignment;
    return o;
}

const char* kind_name(uint32_t program_type) {
    /* D3D10_SB_PIXEL_SHADER .. D3D11_SB_COMPUTE_SHADER -> the device's Kind names */
    static const char* const names[] = {"PixelShader", "VertexShader", "GeometryShader", "HullShader", "DomainShader", "ComputeShader"};
    return program_type < 6 ? names[program_type] : nullptr;
}

uint32_t u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

uint32_t program_type(const uint8_t* b, size_t size) {
    uint32_t chunks = size >= 32 ? u32(b + 28) : 0;
    for (uint32_t c = 0; c < chunks && 32 + 4 * c + 4 <= size; ++c) {
        uint32_t at = u32(b + 32 + 4 * c);
        if (at + 12 > size) continue;
        uint32_t fourcc = u32(b + at);
        if (fourcc == 0x58454853u || fourcc == 0x52444853u) return u32(b + at + 8) >> 16;   /* SHEX / SHDR */
    }
    return ~0u;
}

struct Variant {
    uint32_t lo, hi;
    std::vector<uint32_t> spirv;
    std::vector<Binding> bindings;
};

struct Entry {
    Shader record{};
    std::vector<uint8_t> uniform;
    std::vector<Variant> variants;
};

struct Out {
    std::vector<uint8_t> bytes;
    uint32_t put(const void* data, size_t size) {
        while (bytes.size() % 4) bytes.push_back(0);
        uint32_t at = uint32_t(bytes.size());
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
        return at;
    }
};
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: rdvk_shaderc INPUT OUTPUT\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> input;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) input.insert(input.end(), chunk, chunk + n);
    fclose(f);

    std::vector<Entry> entries;
    unsigned variants_total = 0, failed = 0, stages[6] = {};
    for (size_t at = 0; at + 8 <= input.size();) {
        uint8_t role = input[at];
        uint32_t size = u32(&input[at + 4]);
        const uint8_t* b = &input[at + 8];
        at += 8 + ((size + 3) & ~3u);
        if (at > input.size() + 3 || size < 32 || memcmp(b, "DXBC", 4)) { fprintf(stderr, "bad input record\n"); return 1; }
        uint32_t type = program_type(b, size);
        const char* kind = kind_name(type);
        if (!kind) { fprintf(stderr, "DXBC without a known program type\n"); return 1; }
        Entry e;
        e.record.hash = fnv1a64(b, size);
        e.record.dxbc_size = size;
        memcpy(e.record.checksum, b + 4, 16);
        e.record.role = role;
        char name[48];
        snprintf(name, sizeof(name), "%s_%016llx", kind, (unsigned long long)e.record.hash);
        bool ok = true;
        for (uint32_t align = kMinAlignment; align <= kMaxAlignment && ok; align *= 2) {
            try {
                dxvk::DxbcReader reader(reinterpret_cast<const char*>(b), size);
                dxvk::DxbcModule module(reader);
                dxvk::DxbcModuleInfo info{};
                info.options = compiler_options(align);
                dxvk::DxbcTessInfo tess{64.0f};
                info.tess = &tess;
                info.xfb = nullptr;
                dxvk::Rc<dxvk::DxvkShader> compiled = module.compile(info, name);
                const dxvk::DxvkShaderCreateInfo& ci = compiled->info();
                dxvk::SpirvCodeBuffer code = compiled->code();
                Variant v{align, align, std::vector<uint32_t>(code.data(), code.data() + code.dwords()), {}};
                for (const dxvk::DxvkBindingInfo& bi : compiled->bindings())
                    v.bindings.push_back(Binding{uint32_t(bi.descriptorType), bi.resourceBinding, uint32_t(bi.viewType)});
                std::vector<uint8_t> uniform(ci.uniformData, ci.uniformData + ci.uniformSize);
                if (e.variants.empty()) {
                    e.record.stage = uint32_t(ci.stage);
                    e.record.input_mask = ci.inputMask;
                    e.record.output_mask = ci.outputMask;
                    e.record.patch_vertices = ci.patchVertexCount;
                    e.uniform = uniform;
                } else if (e.record.stage != uint32_t(ci.stage) || e.record.input_mask != ci.inputMask ||
                           e.record.output_mask != ci.outputMask || e.record.patch_vertices != ci.patchVertexCount || e.uniform != uniform) {
                    fprintf(stderr, "%s: interface depends on minSsboAlignment\n", name);
                    return 1;
                }
                Variant& last = e.variants.empty() ? v : e.variants.back();
                if (!e.variants.empty() && last.spirv == v.spirv && last.bindings.size() == v.bindings.size() &&
                    !memcmp(last.bindings.data(), v.bindings.data(), v.bindings.size() * sizeof(Binding)))
                    last.hi = align;
                else
                    e.variants.push_back(std::move(v));
            } catch (const dxvk::DxvkError& error) {
                fprintf(stderr, "RDVK_SHADER_FAILED %s %s\n", name, error.message().c_str());
                ok = false;
            }
        }
        if (!ok) { ++failed; continue; }
        stages[type] += 1;
        variants_total += unsigned(e.variants.size());
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.record.hash != b.record.hash ? a.record.hash < b.record.hash : a.record.dxbc_size < b.record.dxbc_size; });
    for (size_t i = 1; i < entries.size(); ++i)
        if (entries[i].record.hash == entries[i - 1].record.hash && entries[i].record.dxbc_size == entries[i - 1].record.dxbc_size) {
            fprintf(stderr, "duplicate shader key %016llx\n", (unsigned long long)entries[i].record.hash);
            return 1;
        }

    Out out;
    Header header{};
    memcpy(header.magic, kMagic, sizeof(header.magic));
    header.version = kVersion;
    header.shader_count = uint32_t(entries.size());
    out.put(&header, sizeof(header));
    header.shaders = out.put(nullptr, 0);
    out.bytes.resize(header.shaders + entries.size() * sizeof(Shader));
    for (size_t i = 0; i < entries.size(); ++i) {
        Entry& e = entries[i];
        if (!e.uniform.empty()) { e.record.uniform = out.put(e.uniform.data(), e.uniform.size()); e.record.uniform_size = uint32_t(e.uniform.size()); }
        std::vector<VariantRecord> records;
        for (const Variant& v : e.variants) {
            VariantRecord r{};
            r.min_alignment = v.lo;
            r.max_alignment = v.hi == kMaxAlignment ? 0xFFFFFFFFu : v.hi;
            r.spirv = out.put(v.spirv.data(), v.spirv.size() * 4);
            r.spirv_words = uint32_t(v.spirv.size());
            r.bindings = out.put(v.bindings.data(), v.bindings.size() * sizeof(Binding));
            r.binding_count = uint32_t(v.bindings.size());
            records.push_back(r);
        }
        e.record.variants = out.put(records.data(), records.size() * sizeof(VariantRecord));
        e.record.variant_count = uint8_t(records.size());
        memcpy(&out.bytes[header.shaders + i * sizeof(Shader)], &e.record, sizeof(Shader));
    }
    while (out.bytes.size() % 16) out.bytes.push_back(0);
    header.total_size = uint32_t(out.bytes.size());
    memcpy(out.bytes.data(), &header, sizeof(header));
    FILE* o = fopen(argv[2], "wb");
    if (!o || fwrite(out.bytes.data(), 1, out.bytes.size(), o) != out.bytes.size() || fclose(o)) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    printf("{\"shaders\": %zu, \"variants\": %u, \"failed\": %u, \"compiler_messages\": %u, \"bytes\": %zu, "
           "\"stages\": {\"PS\": %u, \"VS\": %u, \"GS\": %u, \"HS\": %u, \"DS\": %u, \"CS\": %u}}\n",
           entries.size(), variants_total, failed, rdvk::g_warnings, out.bytes.size(),
           stages[0], stages[1], stages[2], stages[3], stages[4], stages[5]);
    return failed ? 1 : 0;
}
