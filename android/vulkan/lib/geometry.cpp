/* Mobile geometry on the native Vulkan device.
 *
 * The same module as android/dxvk/mobile_geometry.inc (the D3D11/DXVK path): static index and
 * vertex buffers keep a CPU copy of their initial data; a triangle-list range drawn from a static
 * index buffer in a world pass (G-buffer or depth-only) becomes an "atom" that a worker thread
 * simplifies with meshoptimizer; the simplified indices are written into the same range (rest
 * degenerate) and later draws of exactly that range record the shorter count.  The safety rules,
 * thresholds, error bound and the FP_GEOMETRY log line are those of the DXVK module, so both
 * backends draw the same meshes.  Objects are the device's own (Buffer*, InputLayout*); the
 * input-assembler state is read from the context at the draw.
 */
#include "geometry.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <pthread.h>
#include <strings.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "context.h"
#include "meshoptimizer.h"
#include "rdvk.h"

namespace rdvk {
namespace geometry {
std::atomic<bool> drawing{false};

namespace {
constexpr unsigned MIN_INDICES = 600;               /* 200 triangles: smaller ranges are not worth a job */
constexpr size_t MAX_COPY_BYTES = size_t(48) << 20;  /* one buffer */
constexpr size_t COPY_BUDGET = size_t(640) << 20;    /* all retained copies of static buffers */
constexpr float DEFAULT_ERROR_PIXELS = 100.0f;
constexpr uint32_t APPEND_ALIGNED = 0xFFFFFFFFu, TOPOLOGY_TRIANGLELIST = 4;

/* DXGI formats of vertex attributes. */
enum : uint32_t {
    F_RGBA32_FLOAT = 2, F_RGBA32_UINT = 3, F_RGBA32_SINT = 4, F_RGB32_FLOAT = 6, F_RGB32_UINT = 7, F_RGB32_SINT = 8,
    F_RGBA16_FLOAT = 10, F_RGBA16_UNORM = 11, F_RGBA16_UINT = 12, F_RGBA16_SNORM = 13, F_RGBA16_SINT = 14,
    F_RG32_FLOAT = 16, F_RG32_UINT = 17, F_RG32_SINT = 18, F_RGB10A2_UNORM = 24, F_RGB10A2_UINT = 25, F_RG11B10_FLOAT = 26,
    F_RGBA8_UNORM = 28, F_RGBA8_UINT = 30, F_RGBA8_SNORM = 31, F_RGBA8_SINT = 32,
    F_RG16_FLOAT = 34, F_RG16_UNORM = 35, F_RG16_UINT = 36, F_RG16_SNORM = 37, F_RG16_SINT = 38,
    F_R32_FLOAT = 41, F_R32_UINT = 42, F_R32_SINT = 43, F_RG8_UNORM = 49, F_RG8_UINT = 50, F_RG8_SNORM = 51, F_RG8_SINT = 52,
    F_R16_FLOAT = 54, F_R16_UNORM = 56, F_R16_UINT = 57, F_R16_SNORM = 58, F_R16_SINT = 59,
    F_R8_UNORM = 61, F_R8_UINT = 62, F_R8_SNORM = 63, F_R8_SINT = 64, F_BGRA8_UNORM = 87,
};

struct Element { bool present = false; unsigned slot = 0, offset = 0; uint32_t format = 0; };
struct Layout { Element position, normal, uv, bones; };
struct Copy { std::vector<uint8_t> bytes; };
struct Source {
    const Buffer* buffer = nullptr; const InputLayout* layout = nullptr; unsigned offset = 0, stride = 0; int base = 0;
    bool operator==(const Source& o) const {
        return buffer == o.buffer && layout == o.layout && offset == o.offset && stride == o.stride && base == o.base;
    }
};
struct Atom {
    enum State { Pending, Ready, Skip } state = Skip;
    uint32_t first = 0, count = 0, reduced = 0;
    uint64_t ticket = 0;
    Source source;
    std::vector<uint8_t> original;               /* index bytes of the range before the rewrite */
};
struct IndexBuffer {
    std::shared_ptr<Copy> copy;
    std::map<uint32_t, Atom> atoms;              /* by first index */
    bool frozen = false;                         /* read by a draw whose range is unknown: never rewritten again */
};
struct Job {
    uint64_t ticket = 0; Buffer* buffer = nullptr; uint32_t first = 0, count = 0;
    std::vector<uint32_t> indices; std::shared_ptr<Copy> vertices; Source source; Layout layout; float error = 0;
};
struct Result { uint64_t ticket = 0; Buffer* buffer = nullptr; uint32_t first = 0; std::vector<uint32_t> indices; };

/* Registries (loader threads insert, any thread erases).  Elements of an unordered_map keep their
 * address on rehash, so the objects point at their own entry (Buffer::geometry,
 * InputLayout::geometry) and a draw reads it without the lock: atoms are touched by the render
 * thread only, and an object is not destroyed while the context draws with it. */
std::mutex lock;                                 /* never held across a context call */
std::unordered_map<const Object*, std::shared_ptr<Copy>> vertex_copies;
std::unordered_map<const Object*, IndexBuffer> index_buffers;
std::unordered_map<const Object*, Layout> layouts;
size_t copy_bytes = 0;
uint64_t next_ticket = 1;
/* Worker. */
std::mutex queue_lock;
std::condition_variable queue_signal;
std::deque<Job> jobs;
std::deque<Result> results;
std::atomic<unsigned> results_ready{0};
std::atomic<unsigned> tracked_index_buffers{0}, tracked_vertex_buffers{0};
bool worker_started = false;
/* Writes of the module itself are not the game's writes. */
thread_local bool own_write = false;
/* Counters for the log line. */
std::atomic<uint64_t> stat_jobs{0}, stat_ready{0}, stat_skipped{0}, stat_restored{0}, stat_before{0}, stat_after{0}, stat_micros{0};
uint64_t stat_frame = 0, frame_before = 0, frame_after = 0;
float applied_pixels = 0;
bool primed = false;

float property_pixels() {
    static const float value = [] {
        std::string text = property("debug.fp.geometry");
        return text.empty() ? -1.0f : strtof(text.c_str(), nullptr);
    }();
    return value;
}
/* 0 auto, 1 full meshes, 2 high (25 px), 3 medium (100), 4 low (250), 5 lowest (500). */
float error_pixels() {
    int level = host->geometry_detail ? host->geometry_detail() : 0;
    if (level >= 1 && level <= 5) {
        static const float table[] = {0.0f, 0.0f, 25.0f, 100.0f, 250.0f, 500.0f};
        return table[level];
    }
    float forced = property_pixels();
    if (forced >= 0.0f) return forced;
    /* Launcher group "buildings and objects" (FP_GFX_BUILDINGS = 1): PC keeps the original meshes. */
    if (host->mobile_graphics_pc && host->mobile_graphics_pc(1)) return 0.0f;
    return host->mobile_effects && host->mobile_effects() ? DEFAULT_ERROR_PIXELS : 0.0f;
}
bool capturing() { return error_pixels() > 0; }
float bound_of(float pixels) {
    uint32_t width = 0, height = 0, refresh = 0;
    if (!host->display_mode || !host->display_mode(&width, &height, &refresh) || !height) height = 720;
    float bound = pixels / float(height);
    return bound < 0.01f ? 0.01f : bound > 1.0f ? 1.0f : bound;
}

/* ---- vertex attribute decoding (only what the simplifier needs) ---- */
float half_float(uint16_t h) {
    uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023u, bits;
    if (!exponent) {
        if (!mantissa) bits = sign;
        else { int shift = 0; while (!(mantissa & 1024u)) { mantissa <<= 1; ++shift; } bits = sign | uint32_t(113 - shift) << 23 | (mantissa & 1023u) << 13; }
    } else if (exponent == 31) bits = sign | 0x7f800000u | mantissa << 13;
    else bits = sign | (exponent + 112) << 23 | mantissa << 13;
    float value; memcpy(&value, &bits, 4); return value;
}
unsigned format_bytes(uint32_t format) {
    switch (format) {
    case F_RGBA32_FLOAT: case F_RGBA32_UINT: case F_RGBA32_SINT: return 16;
    case F_RGB32_FLOAT: case F_RGB32_UINT: case F_RGB32_SINT: return 12;
    case F_RGBA16_FLOAT: case F_RGBA16_UNORM: case F_RGBA16_UINT: case F_RGBA16_SNORM: case F_RGBA16_SINT:
    case F_RG32_FLOAT: case F_RG32_UINT: case F_RG32_SINT: return 8;
    case F_RGB10A2_UNORM: case F_RGB10A2_UINT: case F_RG11B10_FLOAT:
    case F_RGBA8_UNORM: case F_RGBA8_UINT: case F_RGBA8_SNORM: case F_RGBA8_SINT: case F_BGRA8_UNORM:
    case F_RG16_FLOAT: case F_RG16_UNORM: case F_RG16_UINT: case F_RG16_SNORM: case F_RG16_SINT:
    case F_R32_FLOAT: case F_R32_UINT: case F_R32_SINT: return 4;
    case F_RG8_UNORM: case F_RG8_UINT: case F_RG8_SNORM: case F_RG8_SINT:
    case F_R16_FLOAT: case F_R16_UNORM: case F_R16_UINT: case F_R16_SNORM: case F_R16_SINT: return 2;
    case F_R8_UNORM: case F_R8_UINT: case F_R8_SNORM: case F_R8_SINT: return 1;
    default: return 0;
    }
}
bool decode_normal(const uint8_t* p, uint32_t format, float out[3]) {
    switch (format) {
    case F_RGB32_FLOAT: case F_RGBA32_FLOAT: memcpy(out, p, 12); break;
    case F_RGBA16_SNORM: { int16_t v[3]; memcpy(v, p, 6); for (int i = 0; i < 3; ++i) out[i] = float(v[i]) / 32767.0f; break; }
    case F_RGBA16_FLOAT: { uint16_t v[3]; memcpy(v, p, 6); for (int i = 0; i < 3; ++i) out[i] = half_float(v[i]); break; }
    case F_RGBA8_SNORM: for (int i = 0; i < 3; ++i) out[i] = float(int8_t(p[i])) / 127.0f; break;
    case F_RGBA8_UNORM: for (int i = 0; i < 3; ++i) out[i] = float(p[i]) / 127.5f - 1.0f; break;
    case F_RGB10A2_UNORM: { uint32_t v; memcpy(&v, p, 4); for (int i = 0; i < 3; ++i) out[i] = float((v >> (10 * i)) & 1023u) / 511.5f - 1.0f; break; }
    default: return false;
    }
    float length = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (!(length > 1e-6f) || !std::isfinite(length)) return false;
    for (int i = 0; i < 3; ++i) out[i] /= length;
    return true;
}
bool decode_uv(const uint8_t* p, uint32_t format, float out[2]) {
    switch (format) {
    case F_RG32_FLOAT: case F_RGB32_FLOAT: case F_RGBA32_FLOAT: memcpy(out, p, 8); break;
    case F_RG16_FLOAT: case F_RGBA16_FLOAT: { uint16_t v[2]; memcpy(v, p, 4); out[0] = half_float(v[0]); out[1] = half_float(v[1]); break; }
    case F_RG16_SNORM: case F_RGBA16_SNORM: { int16_t v[2]; memcpy(v, p, 4); out[0] = float(v[0]) / 32767.0f; out[1] = float(v[1]) / 32767.0f; break; }
    case F_RG16_UNORM: case F_RGBA16_UNORM: { uint16_t v[2]; memcpy(v, p, 4); out[0] = float(v[0]) / 65535.0f; out[1] = float(v[1]) / 65535.0f; break; }
    default: return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

/* ---- worker ---- */
std::vector<uint32_t> simplify(const Job& job) {
    const Layout& layout = job.layout;
    const std::vector<uint8_t>& bytes = job.vertices->bytes;
    uint32_t low = UINT32_MAX, high = 0;
    for (uint32_t index : job.indices) { low = index < low ? index : low; high = index > high ? index : high; }
    int64_t base = int64_t(job.source.offset) + int64_t(job.source.base) * int64_t(job.source.stride);
    int64_t first_byte = base + int64_t(low) * job.source.stride, last_byte = base + int64_t(high) * job.source.stride + job.source.stride;
    if (first_byte < 0 || last_byte > int64_t(bytes.size()) || high - low >= (1u << 22)) return {};
    size_t vertices = size_t(high - low) + 1;
    std::vector<float> positions(vertices * 3), attributes(vertices * 6, 0.0f);
    bool normals = layout.normal.present && layout.normal.slot == layout.position.slot;
    bool uvs = layout.uv.present && layout.uv.slot == layout.position.slot;
    bool bones = layout.bones.present && layout.bones.slot == layout.position.slot && format_bytes(layout.bones.format) >= 4;
    for (size_t v = 0; v < vertices; ++v) {
        const uint8_t* vertex = bytes.data() + first_byte + int64_t(v) * job.source.stride;
        memcpy(&positions[v * 3], vertex + layout.position.offset, 12);
        if (!std::isfinite(positions[v * 3]) || !std::isfinite(positions[v * 3 + 1]) || !std::isfinite(positions[v * 3 + 2])) return {};
        float* a = &attributes[v * 6];
        if (normals && !decode_normal(vertex + layout.normal.offset, layout.normal.format, a)) normals = false;
        if (uvs && !decode_uv(vertex + layout.uv.offset, layout.uv.format, a + 3)) uvs = false;
        /* The first bone a vertex follows: vertices of different bones never merge. */
        if (bones) { uint32_t packed; memcpy(&packed, vertex + layout.bones.offset, 4); a[5] = float(packed & 0xffffu); }
    }
    if (!normals) for (size_t v = 0; v < vertices; ++v) { attributes[v * 6] = attributes[v * 6 + 1] = attributes[v * 6 + 2] = 0; }
    if (!uvs) for (size_t v = 0; v < vertices; ++v) { attributes[v * 6 + 3] = attributes[v * 6 + 4] = 0; }
    std::vector<uint32_t> local(job.indices.size()), result(job.indices.size());
    for (size_t i = 0; i < local.size(); ++i) local[i] = job.indices[i] - low;
    /* Shading and texture seams cost error; the weights are relative to the size of the mesh. */
    const float weights[6] = {0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 8.0f};
    float reached = 0;
    size_t kept = meshopt_simplifyWithAttributes(result.data(), local.data(), local.size(), positions.data(), vertices, 12,
        attributes.data(), 24, weights, 6, nullptr, 0, job.error,
        meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune | meshopt_SimplifyPermissive | meshopt_SimplifySparse, &reached);
    result.resize(kept);
    for (uint32_t& index : result) index += low;
    return result;
}
void worker_main() {
    pthread_setname_np(pthread_self(), "fp-geometry");
    setpriority(PRIO_PROCESS, gettid(), 10);       /* never competes with the game's own threads */
    for (;;) {
        Job job;
        { std::unique_lock<std::mutex> guard(queue_lock); queue_signal.wait(guard, [] { return !jobs.empty(); }); job = std::move(jobs.front()); jobs.pop_front(); }
        struct timespec begin, end; clock_gettime(CLOCK_MONOTONIC, &begin);
        Result result; result.ticket = job.ticket; result.buffer = job.buffer; result.first = job.first; result.indices = simplify(job);
        clock_gettime(CLOCK_MONOTONIC, &end);
        stat_micros.fetch_add(uint64_t((end.tv_sec - begin.tv_sec) * 1000000LL + (end.tv_nsec - begin.tv_nsec) / 1000), std::memory_order_relaxed);
        { std::lock_guard<std::mutex> guard(queue_lock); results.push_back(std::move(result)); }
        results_ready.fetch_add(1, std::memory_order_release);
    }
}
void submit(Job&& job) {
    std::lock_guard<std::mutex> guard(queue_lock);
    if (!worker_started) { worker_started = true; std::thread(worker_main).detach(); }
    jobs.push_back(std::move(job)); queue_signal.notify_one();
}

void write_range(Buffer* buffer, uint32_t first_byte, const std::vector<uint8_t>& bytes) {
    d3d::BOX box{first_byte, 0, 0, first_byte + uint32_t(bytes.size()), 1, 1};
    own_write = true;
    g_ctx.update_subresource(buffer, 0, &box, bytes.data(), 0, 0);
    own_write = false;
}
/* Results of the worker: at most a few ranges per frame are written. */
void apply_results() {
    for (unsigned budget = 6; budget && results_ready.load(std::memory_order_acquire); --budget) {
        Result result;
        { std::lock_guard<std::mutex> guard(queue_lock); if (results.empty()) break; result = std::move(results.front()); results.pop_front(); }
        results_ready.fetch_sub(1, std::memory_order_release);
        std::vector<uint8_t> bytes; uint32_t first_byte = 0;
        {
            std::lock_guard<std::mutex> guard(lock);
            auto found = index_buffers.find(result.buffer);
            if (found == index_buffers.end()) continue;
            auto atom = found->second.atoms.find(result.first);
            if (atom == found->second.atoms.end() || atom->second.ticket != result.ticket || atom->second.state != Atom::Pending) continue;
            Atom& a = atom->second;
            /* Not worth a rewrite unless a tenth of the range goes away. */
            if (result.indices.empty() || result.indices.size() % 3 || result.indices.size() * 10 > size_t(a.count) * 9) {
                a.state = Atom::Skip; stat_skipped.fetch_add(1, std::memory_order_relaxed); continue;
            }
            unsigned width = unsigned(a.original.size() / a.count);
            bytes = a.original;
            uint32_t filler = 0; memcpy(&filler, a.original.data(), width);
            for (uint32_t i = 0; i < a.count; ++i) {
                uint32_t index = i < result.indices.size() ? result.indices[i] : filler;
                memcpy(bytes.data() + size_t(i) * width, &index, width);
            }
            first_byte = a.first * width;
            memcpy(found->second.copy->bytes.data() + first_byte, bytes.data(), bytes.size());
            a.reduced = uint32_t(result.indices.size()); a.state = Atom::Ready;
            stat_ready.fetch_add(1, std::memory_order_relaxed); stat_before.fetch_add(a.count, std::memory_order_relaxed);
            stat_after.fetch_add(a.reduced, std::memory_order_relaxed);
        }
        write_range(result.buffer, first_byte, bytes);
    }
}
struct Restore { Buffer* buffer; uint32_t first_byte; std::vector<uint8_t> bytes; };
void restore_atom(IndexBuffer& buffer, const Object* key, const Atom& atom, std::vector<Restore>& out) {
    uint32_t first_byte = atom.first * uint32_t(atom.original.size() / atom.count);
    memcpy(buffer.copy->bytes.data() + first_byte, atom.original.data(), atom.original.size());
    out.push_back({const_cast<Buffer*>(static_cast<const Buffer*>(key)), first_byte, atom.original});
    stat_restored.fetch_add(1, std::memory_order_relaxed);
}
/* Put every remembered range back on `want` pixels. */
void retarget(float want) {
    std::vector<Restore> restores;
    std::vector<Job> queued;
    float bound = want > 0 ? bound_of(want) : 0;
    { std::lock_guard<std::mutex> guard(queue_lock); jobs.clear(); }
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto& entry : index_buffers) {
            IndexBuffer& buffer = entry.second;
            for (auto& atom_entry : buffer.atoms) {
                Atom& atom = atom_entry.second;
                if (atom.original.empty() || !atom.count || atom.original.size() % atom.count) continue;
                unsigned width = unsigned(atom.original.size() / atom.count);
                if (atom.state == Atom::Ready && atom.reduced && atom.reduced < atom.count && buffer.copy &&
                    size_t(atom.first) * width + atom.original.size() <= buffer.copy->bytes.size()) {
                    restore_atom(buffer, entry.first, atom, restores);
                    stat_restored.fetch_sub(1, std::memory_order_relaxed);   /* a retarget, not a conflict */
                }
                atom.ticket = next_ticket++;
                if (!(want > 0)) { atom.state = Atom::Ready; atom.reduced = atom.count; continue; }
                const Layout* layout = nullptr;
                auto known = layouts.find(atom.source.layout);
                if (known != layouts.end() && known->second.position.present) layout = &known->second;
                auto vertices = vertex_copies.find(atom.source.buffer);
                if (!layout || vertices == vertex_copies.end() || width < 2 || width > 4 || atom.count % 3) {
                    atom.state = Atom::Ready; atom.reduced = atom.count; continue;
                }
                Job job; job.ticket = atom.ticket; job.buffer = const_cast<Buffer*>(static_cast<const Buffer*>(entry.first));
                job.first = atom.first; job.count = atom.count;
                job.vertices = vertices->second; job.source = atom.source; job.layout = *layout; job.error = bound;
                job.indices.resize(atom.count);
                for (uint32_t i = 0; i < atom.count; ++i) { uint32_t index = 0; memcpy(&index, atom.original.data() + size_t(i) * width, width); job.indices[i] = index; }
                atom.state = Atom::Pending;
                queued.push_back(std::move(job));
            }
        }
    }
    for (Restore& r : restores) write_range(r.buffer, r.first_byte, r.bytes);
    for (Job& job : queued) { stat_jobs.fetch_add(1, std::memory_order_relaxed); submit(std::move(job)); }
    log("FP_GEOMETRY_RETARGET pixels=%.1f restores=%zu jobs=%zu", double(want), restores.size(), queued.size());
}
}  // namespace

void init() {
    float want = error_pixels();
    applied_pixels = want;
    primed = true;
    drawing.store(want > 0, std::memory_order_relaxed);
    log("RDVK_GEOMETRY error_pixels=%.1f", double(want));
}

void buffer_created(Buffer* buffer, const d3d::BUFFER_DESC& desc, const void* data) {
    if (!data || !capturing()) return;
    bool index = desc.BindFlags == d3d::BIND_INDEX_BUFFER, vertex = desc.BindFlags == d3d::BIND_VERTEX_BUFFER;
    if ((!index && !vertex) || desc.Usage == d3d::USAGE_DYNAMIC || desc.Usage == d3d::USAGE_STAGING || desc.CPUAccessFlags ||
        desc.MiscFlags || desc.ByteWidth < MIN_INDICES * 2 || desc.ByteWidth > MAX_COPY_BYTES)
        return;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (copy_bytes + desc.ByteWidth > COPY_BUDGET) return;
        copy_bytes += desc.ByteWidth;   /* reserved; the copy is made without the lock (loader threads) */
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    auto copy = std::make_shared<Copy>();
    copy->bytes.assign(bytes, bytes + desc.ByteWidth);
    std::lock_guard<std::mutex> guard(lock);
    if (index) {
        IndexBuffer& entry = index_buffers[buffer];
        if (entry.copy) copy_bytes -= entry.copy->bytes.size();
        entry = IndexBuffer();
        entry.copy = std::move(copy);
        buffer->geometry = &entry;
        tracked_index_buffers.store(unsigned(index_buffers.size()), std::memory_order_relaxed);
    } else {
        auto& slot = vertex_copies[buffer];
        if (slot) copy_bytes -= slot->bytes.size();
        slot = std::move(copy);
        tracked_vertex_buffers.store(unsigned(vertex_copies.size()), std::memory_order_relaxed);
    }
}

void layout_created(InputLayout* object, const d3d::INPUT_ELEMENT_DESC* descs, uint32_t count) {
    if (!capturing()) return;
    Layout layout; unsigned next[16] = {0}; bool known[16]; for (bool& k : known) k = true;
    for (uint32_t i = 0; i < count; ++i) {
        const d3d::INPUT_ELEMENT_DESC& d = descs[i];
        if (d.InputSlot >= 16) continue;
        unsigned size = format_bytes(d.Format);
        unsigned offset = d.AlignedByteOffset == APPEND_ALIGNED ? (next[d.InputSlot] + 3u) & ~3u : d.AlignedByteOffset;
        bool located = d.AlignedByteOffset != APPEND_ALIGNED || known[d.InputSlot];
        if (size) next[d.InputSlot] = offset + size; else known[d.InputSlot] = false;
        if (!located || d.InputSlotClass != 0 || d.SemanticIndex || !d.SemanticName) continue;
        const char* name = reinterpret_cast<const char*>(d.SemanticName);
        Element element; element.present = true; element.slot = d.InputSlot; element.offset = offset; element.format = d.Format;
        if (!strcasecmp(name, "POSITION")) layout.position = element;
        else if (!strcasecmp(name, "NORMAL")) layout.normal = element;
        else if (!strcasecmp(name, "TEXCOORD")) layout.uv = element;
        else if (!strcasecmp(name, "BLENDINDICES")) layout.bones = element;
    }
    std::lock_guard<std::mutex> guard(lock);
    Layout& slot = layouts[object];
    slot = layout;
    object->geometry = &slot;
}

void forget(const Object* object) {
    std::lock_guard<std::mutex> guard(lock);
    auto vertex = vertex_copies.find(object);
    if (vertex != vertex_copies.end()) {
        copy_bytes -= vertex->second->bytes.size(); vertex_copies.erase(vertex);
        tracked_vertex_buffers.store(unsigned(vertex_copies.size()), std::memory_order_relaxed);
    }
    auto index = index_buffers.find(object);
    if (index != index_buffers.end()) {
        if (index->second.copy) copy_bytes -= index->second.copy->bytes.size();
        if (object->kind == Kind::Buffer) static_cast<Buffer*>(const_cast<Object*>(object))->geometry = nullptr;
        index_buffers.erase(index);
        tracked_index_buffers.store(unsigned(index_buffers.size()), std::memory_order_relaxed);
    }
    if (layouts.erase(object) && object->kind == Kind::InputLayout) static_cast<InputLayout*>(const_cast<Object*>(object))->geometry = nullptr;
}

uint32_t draw(uint32_t count, uint32_t start, int32_t base) {
    if (results_ready.load(std::memory_order_acquire)) apply_results();
    Buffer* index_buffer = g_ctx.index_buffer();
    auto* tracked = index_buffer ? static_cast<IndexBuffer*>(index_buffer->geometry) : nullptr;
    if (!tracked) return count;
    uint32_t index_bytes = g_ctx.index_format() == 57 ? 2 : g_ctx.index_format() == 42 ? 4 : 0;
    uint32_t index_offset = g_ctx.index_offset();
    InputLayout* input_layout = g_ctx.input_layout();
    uint32_t topology = g_ctx.topology();
    std::vector<Restore> restore;
    Job job; bool submit_job = false;
    {
        if (!index_bytes || index_offset % index_bytes || !count) return count;
        uint32_t first = start + index_offset / index_bytes;
        IndexBuffer& buffer = *tracked;
        if (buffer.atoms.empty() && (count < MIN_INDICES || buffer.frozen)) return count;
        const Layout* layout = input_layout ? static_cast<const Layout*>(input_layout->geometry) : nullptr;
        if (layout && !(layout->position.present && layout->position.slot < 8)) layout = nullptr;
        Source source;
        if (layout) {
            Buffer* vb = nullptr; uint32_t offset = 0, stride = 0;
            g_ctx.vertex_binding(layout->position.slot, &vb, &offset, &stride);
            source.buffer = vb; source.offset = offset; source.stride = stride; source.base = base; source.layout = input_layout;
        }
        auto exact = buffer.atoms.find(first);
        if (exact != buffer.atoms.end() && exact->second.count == count) {
            Atom& atom = exact->second;
            if (atom.state == Atom::Skip) return count;
            if (atom.source == source && topology == TOPOLOGY_TRIANGLELIST) {
                if (atom.state == Atom::Ready) { frame_before += count; frame_after += atom.reduced; return atom.reduced; }
                return count;
            }
            /* The same indices with other vertices or another topology: the rewrite is not valid for them. */
            if (atom.state == Atom::Ready) restore_atom(buffer, index_buffer, atom, restore);
            atom.state = Atom::Skip; atom.ticket = 0; atom.original.clear();
        } else {
            /* Atoms that this range touches without being one of them. */
            bool contains_only = true, touches = false;
            auto it = buffer.atoms.lower_bound(first);
            if (it != buffer.atoms.begin()) { auto before = std::prev(it); if (before->second.first + before->second.count > first) it = before; }
            std::vector<uint32_t> cut;
            for (; it != buffer.atoms.end() && it->second.first < first + count; ++it) {
                const Atom& atom = it->second; touches = true;
                if (atom.first < first || atom.first + atom.count > first + count) { contains_only = false; cut.push_back(atom.first); }
            }
            for (uint32_t key : cut) {
                Atom& atom = buffer.atoms[key];
                if (atom.state == Atom::Ready) restore_atom(buffer, index_buffer, atom, restore);
                buffer.atoms.erase(key);
            }
            if (touches && contains_only) return count;      /* a range around whole atoms: its indices are valid as they are */
            bool contains = false;
            if (touches) {
                auto again = buffer.atoms.lower_bound(first);
                contains = again != buffer.atoms.end() && again->second.first < first + count;
            }
            /* Small ranges are only checked against the atoms; they never become one. */
            if (!contains && count >= MIN_INDICES && !buffer.frozen) {
                Atom atom; atom.first = first; atom.count = count; atom.source = source;
                uint64_t end_byte = (uint64_t(first) + count) * index_bytes;
                bool eligible = g_ctx.world_pass() && layout && count % 3 == 0 && topology == TOPOLOGY_TRIANGLELIST && source.buffer &&
                                source.stride && buffer.copy && end_byte <= buffer.copy->bytes.size() &&
                                (layout->position.format == F_RGB32_FLOAT || layout->position.format == F_RGBA32_FLOAT) &&
                                layout->position.offset + 12 <= source.stride;
                std::shared_ptr<Copy> vertices;
                if (eligible) {
                    std::lock_guard<std::mutex> guard(lock);
                    auto known_vertices = vertex_copies.find(source.buffer);
                    if (known_vertices != vertex_copies.end()) vertices = known_vertices->second;
                }
                if (vertices) {
                    const uint8_t* data = buffer.copy->bytes.data() + size_t(first) * index_bytes;
                    atom.original.assign(data, data + size_t(count) * index_bytes);
                    atom.state = Atom::Pending; atom.ticket = next_ticket++;
                    job.ticket = atom.ticket; job.buffer = index_buffer; job.first = first; job.count = count; job.vertices = std::move(vertices);
                    job.source = source; job.layout = *layout; job.error = bound_of(error_pixels());
                    job.indices.resize(count);
                    for (uint32_t i = 0; i < count; ++i) { uint32_t index = 0; memcpy(&index, data + size_t(i) * index_bytes, index_bytes); job.indices[i] = index; }
                    submit_job = true; stat_jobs.fetch_add(1, std::memory_order_relaxed);
                }
                buffer.atoms[first] = std::move(atom);
            }
        }
    }
    for (Restore& r : restore) write_range(r.buffer, r.first_byte, r.bytes);
    if (submit_job) submit(std::move(job));
    return count;
}

void draw_unknown_range() {
    std::vector<Restore> restore;
    Buffer* index_buffer = g_ctx.index_buffer();
    auto* tracked = index_buffer ? static_cast<IndexBuffer*>(index_buffer->geometry) : nullptr;
    {
        if (!tracked || tracked->frozen) return;
        IndexBuffer& buffer = *tracked;
        buffer.frozen = true;
        for (auto& entry : buffer.atoms)
            if (entry.second.state == Atom::Ready) restore_atom(buffer, index_buffer, entry.second, restore);
        buffer.atoms.clear();
    }
    for (Restore& r : restore) write_range(r.buffer, r.first_byte, r.bytes);
}

void written(const Buffer* identity) {
    if (own_write || !(tracked_index_buffers.load(std::memory_order_relaxed) | tracked_vertex_buffers.load(std::memory_order_relaxed))) return;
    std::vector<Restore> restore;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto vertex = vertex_copies.find(identity);
        bool index = index_buffers.count(identity) != 0;
        if (vertex == vertex_copies.end() && !index) return;
        for (auto& entry : index_buffers) {
            IndexBuffer& buffer = entry.second;
            bool whole = entry.first == identity;
            for (auto it = buffer.atoms.begin(); it != buffer.atoms.end();) {
                Atom& atom = it->second;
                if (!whole && atom.source.buffer != identity) { ++it; continue; }
                if (atom.state == Atom::Ready) restore_atom(buffer, entry.first, atom, restore);
                if (whole) it = buffer.atoms.erase(it);
                else { atom.state = Atom::Skip; atom.ticket = 0; atom.original.clear(); ++it; }
            }
            if (whole) buffer.frozen = true;
        }
        if (vertex != vertex_copies.end()) {
            copy_bytes -= vertex->second->bytes.size(); vertex_copies.erase(vertex);
            tracked_vertex_buffers.store(unsigned(vertex_copies.size()), std::memory_order_relaxed);
        }
    }
    for (Restore& r : restore) write_range(r.buffer, r.first_byte, r.bytes);
}

void present() {
    float want = error_pixels();
    if (!primed) init();
    else if (want != applied_pixels) {
        retarget(want);
        applied_pixels = want;
        drawing.store(want > 0, std::memory_order_relaxed);
    }
    if (!(want > 0)) return;
    if (++stat_frame % 600 == 300)
        log("FP_GEOMETRY frame=%llu error=%.3f jobs=%llu ready=%llu skipped=%llu restored=%llu indices_before=%llu indices_after=%llu "
            "frame_indices_before=%llu frame_indices_after=%llu worker_ms=%llu copies_mb=%zu foreign_context=0",
            (unsigned long long)stat_frame, double(bound_of(want)), (unsigned long long)stat_jobs.load(), (unsigned long long)stat_ready.load(),
            (unsigned long long)stat_skipped.load(), (unsigned long long)stat_restored.load(), (unsigned long long)stat_before.load(),
            (unsigned long long)stat_after.load(), (unsigned long long)frame_before, (unsigned long long)frame_after,
            (unsigned long long)(stat_micros.load() / 1000), copy_bytes >> 20);
    frame_before = frame_after = 0;
}
}  // namespace geometry
}  // namespace rdvk
