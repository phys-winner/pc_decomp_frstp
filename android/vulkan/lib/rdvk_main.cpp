/* libfrostpunk_rdvk.so: native Vulkan rendering device of the Liquid Engine.  Entry point and
 * the routing check (debug.fp.rdvk_mode=passthrough: every hook logs its first calls and runs the
 * original D3D11 body, so the game keeps rendering through DXVK). */
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <sys/system_properties.h>
#include "rdvk.h"
#include "pipeline.h"
#include "shaderlib_device.h"
#include "stability.h"
/* The shared launcher's renderer failure record (owner's decision of 24 November 2026): the start stops and
   the launcher offers the next renderer of its list; nothing here switches to another driver. */
#include "../../launcher-base/native/replacements/renderer_failure.h"

namespace rdvk {
const rdvk_host* host = nullptr;

namespace {
char gpu_note[256];   /* the last "RDVK_GPU ..." line: why Gpu::init refused this driver */
/* QA switch debug.fp.renderer_fail=start:<id> | lost:<id>, read once in init(); it names the renderer so that the
   renderer the launcher offers next starts normally. */
bool qa_start = false, qa_lost = false;
char qa_value[PROP_VALUE_MAX];
}  // namespace

void log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (!strncmp(line, "RDVK_GPU ", 9)) snprintf(gpu_note, sizeof(gpu_note), "%s", line + 9);
    if (host && host->log) host->log(line);
}

[[noreturn]] void stop(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (host && host->log) host->log(line);
    fprintf(stderr, "STOP rdvk %s\n", line);
    fflush(stderr);
    guard_fatal();
    _Exit(73);
}

[[noreturn]] void renderer_stop(bool device_lost, const char* format, ...) {
    char line[PORT_RENDERER_FAILURE_REASON_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (device_lost) pipeline_cache_drop_device_lost();
    /* Written first, with open/write/close only: the game ends right after, and the crash watcher
       (GameWatchActivity) then opens the launcher, which offers the next renderer. */
    int recorded = port_renderer_failed(line, device_lost ? 1 : 0);
    char note[PORT_RENDERER_FAILURE_REASON_MAX + 96];
    snprintf(note, sizeof(note), "RDVK_RENDERER_FAILURE kind=%s recorded=%d %s", device_lost ? "lost" : "start", recorded, line);
    if (host && host->log) host->log(note);
    fprintf(stderr, "STOP rdvk %s\n", line);
    fflush(stderr);
    guard_fatal();
    _Exit(73);
}

void renderer_presented() { port_renderer_presented(); }

bool renderer_has_presented() { return __atomic_load_n(&port_renderer_failure_global.presented_ns, __ATOMIC_RELAXED) != 0; }

const char* gpu_failure_note() { return gpu_note[0] ? gpu_note : "see RDVK_GPU"; }

void renderer_qa_start() {
    if (qa_start) renderer_stop(false, "QA switch debug.fp.renderer_fail=%s: simulated start failure (vkCreateDevice: VK_ERROR_INITIALIZATION_FAILED)", qa_value);
}

void renderer_qa_frame() {
    static uint64_t frames_presented = 0;
    if (!qa_lost || !renderer_has_presented()) return;
    if (++frames_presented >= 60)
        renderer_stop(true, "QA switch debug.fp.renderer_fail=%s: simulated vkQueueSubmit: VK_ERROR_DEVICE_LOST (-4)", qa_value);
}

/* Normal thread (the runtime's constructor): the record's path and renderer, the QA switch. */
static void renderer_failure_setup() {
    port_renderer_failure_init();
    std::string qa = property("debug.fp.renderer_fail");
    const char* renderer = port_renderer_failure_global.renderer;
    snprintf(qa_value, sizeof(qa_value), "%s", qa.c_str());
    size_t colon = qa.find(':');
    if (colon != std::string::npos && renderer[0] && qa.compare(colon + 1, std::string::npos, renderer) == 0) {
        qa_start = qa.compare(0, colon, "start") == 0;
        qa_lost = qa.compare(0, colon, "lost") == 0;
    }
    if (!qa.empty()) log("RDVK_RENDERER_FAILURE_QA debug.fp.renderer_fail=%s renderer=%s start=%d lost=%d", qa.c_str(), renderer, qa_start, qa_lost);
}

std::string property(const char* name) {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get(name, value);
    return value;
}

namespace {
std::atomic<uint64_t> passthrough_calls[RDVK_HOOK_MAX];
const char* const hook_names[] = {
#include "rdvk_hook_names.inc"
};
static_assert(sizeof(hook_names) / sizeof(*hook_names) == RDVK_HOOK_COUNT, "hook names");

template <unsigned I> void passthrough(CPU* c) {
    uint64_t n = passthrough_calls[I].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 2 || (n & (n - 1)) == 0)
        log("RDVK_PASSTHROUGH hook=%u name=%s va=0x%llx calls=%llu ret=0x%llx", I, hook_names[I] ? hook_names[I] : "?",
            (unsigned long long)host->hook_va[I], (unsigned long long)n, (unsigned long long)*reinterpret_cast<uint64_t*>(c->rsp));
    host->original[I](c);
}

template <size_t... I> void fill_passthrough(rdvk_fn* table, std::index_sequence<I...>) {
    ((table[I] = passthrough<I>), ...);
}

int init(const rdvk_host* h, rdvk_fn table[RDVK_HOOK_MAX]) {
    if (!h || h->abi != RDVK_ABI || h->size != sizeof(rdvk_host)) return 0;
    host = h;
    renderer_failure_setup();
    if (h->hook_count != RDVK_HOOK_COUNT) {
        log("RDVK_HOOK_MISMATCH runtime=%u library=%u", h->hook_count, RDVK_HOOK_COUNT);
        return 0;
    }
    std::string mode = property("debug.fp.rdvk_mode");
    if (mode == "passthrough") {
        fill_passthrough(table, std::make_index_sequence<RDVK_HOOK_COUNT>());
        log("RDVK_MODE passthrough hooks=%u", RDVK_HOOK_COUNT);
        return 1;
    }
    if (!shaderlib_open()) return 0;   /* no build-time shader library in the APK -> D3D11/DXVK */
    if (!guard_init()) return 0;   /* safe mode: two crashed native sessions in a row -> D3D11/DXVK */
    return native_init(table);
}

int com_dispatch(CPU* c, uint64_t target) { return com_call(c, target); }
uint32_t read_texture_entry(uint64_t texture, uint32_t* width, uint32_t* height, uint32_t* format, uint8_t** rgba) {
    return read_texture(texture, width, height, format, rgba);
}

const rdvk_library library = {RDVK_ABI, sizeof(rdvk_library), init, com_dispatch, read_texture_entry};
}  // namespace
}  // namespace rdvk

extern "C" __attribute__((visibility("default"))) const rdvk_library* frostpunk_rdvk_library(void) { return &rdvk::library; }
