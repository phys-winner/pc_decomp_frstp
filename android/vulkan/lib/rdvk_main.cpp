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
#include "shaderlib_device.h"
#include "stability.h"

namespace rdvk {
const rdvk_host* host = nullptr;

void log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
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
