/* Stability guards of the native device.
 *
 * Safe mode across launches (external-theories T05 g, doom-port SafeMode.java; our variant):
 * files/rdvk-guard.txt counts native sessions that ended in a fatal signal or a STOP of this
 * library.  One such session -> the next launch serializes the driver's compiler calls on every
 * GPU (level 1); two in a row -> the library declines and the game renders through D3D11/DXVK
 * (level 2, logged as RDVK_SAFE_MODE).  Five minutes of native frames reset the counter, and a
 * new library build starts from zero.  debug.fp.rdvk_guard=0 disables the counter;
 * debug.fp.renderer=vulkan ignores level 2.  LMK and swipe-away kills are SIGKILL: not counted.
 *
 * Crash report (T10 c): a handler for fatal signals, chained in front of the app's crash marker,
 * writes the signal, the fault address and a backtrace as library+offset (symbol) to native.log
 * (the app's handler resets to the default action, so no tombstone is written for the game).
 *
 * Driver serialization (T05 e, doom-port DM_GFX_PIPE_SERIAL): one mutex around the driver calls
 * that run its compiler.  The PowerVR DXT-48 driver crashed in PSCCompileToHw under vkCmdCopyImage
 * while loader threads recorded uploads. */
#include "stability.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <string>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>
#include "device.h"
#include "rdvk.h"

namespace rdvk {
namespace {
const char BUILD[] = __DATE__ " " __TIME__;
const int FATAL[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP};
int guard_fd = -1;
int level = 0;
bool stable = false;
uint64_t start_ns = 0;
/* The marker the signal handler writes, prepared in advance (two slots: switched atomically). */
char crash_text[2][160];
size_t crash_len[2] = {0, 0};
std::atomic<int> crash_slot{0};
std::atomic<bool> handling{false};
struct sigaction previous[NSIG];

uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
}

/* Async-signal-safe: pwrite and ftruncate. */
void write_guard(const char* text, size_t len) {
    if (guard_fd < 0 || !len) return;
    if (pwrite(guard_fd, text, len, 0) == ssize_t(len) && ftruncate(guard_fd, off_t(len))) { /* best effort */ }
}

void prepare_crash_text(unsigned crashes) {
    int slot = crash_slot.load() ^ 1;
    int n = snprintf(crash_text[slot], sizeof(crash_text[slot]), "crashes=%u build=%s state=crashed\n", crashes + 1, BUILD);
    crash_len[slot] = n > 0 ? size_t(n) : 0;
    crash_slot.store(slot);
}

void write_crash_marker() {
    int slot = crash_slot.load();
    write_guard(crash_text[slot], crash_len[slot]);
}

void out(const char* text) {
    size_t n = strlen(text);
    while (n) {
        ssize_t w = write(STDERR_FILENO, text, n);
        if (w <= 0) break;
        text += w;
        n -= size_t(w);
    }
}

void describe(const char* prefix, uintptr_t pc) {
    char line[384];
    Dl_info info{};
    if (pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
        const char* name = strrchr(info.dli_fname, '/');
        name = name ? name + 1 : info.dli_fname;
        size_t offset = size_t(pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
        if (info.dli_sname)
            snprintf(line, sizeof(line), "%s pc %016zx %s+0x%zx (%s+%zu)\n", prefix, size_t(pc), name, offset, info.dli_sname,
                     size_t(pc - reinterpret_cast<uintptr_t>(info.dli_saddr)));
        else
            snprintf(line, sizeof(line), "%s pc %016zx %s+0x%zx\n", prefix, size_t(pc), name, offset);
    } else {
        snprintf(line, sizeof(line), "%s pc %016zx\n", prefix, size_t(pc));
    }
    out(line);
}

struct Trace { uintptr_t pc[40]; int count; };
_Unwind_Reason_Code unwind_step(_Unwind_Context* context, void* data) {
    auto* t = static_cast<Trace*>(data);
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc) t->pc[t->count++] = pc;
    return t->count < 40 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void on_fatal(int sig, siginfo_t* info, void* context) {
    if (!handling.exchange(true)) {
        write_crash_marker();
        char thread[17] = {};
        prctl(PR_GET_NAME, thread, 0, 0, 0);
        char line[320];
        snprintf(line, sizeof(line), "RDVK_CRASH signal=%d code=%d addr=%p tid=%ld thread=%s guard_level=%d\n", sig, info ? info->si_code : 0,
                 info ? info->si_addr : nullptr, long(syscall(SYS_gettid)), thread, level);
        out(line);
#if defined(__aarch64__)
        if (const auto* uc = static_cast<const ucontext_t*>(context)) {
            describe("RDVK_CRASH fault", uintptr_t(uc->uc_mcontext.pc));
            describe("RDVK_CRASH lr   ", uintptr_t(uc->uc_mcontext.regs[30]));
        }
#else
        (void)context;
#endif
        Trace trace{{}, 0};
        _Unwind_Backtrace(unwind_step, &trace);
        for (int i = 0; i < trace.count; ++i) {
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "RDVK_CRASH #%02d", i);
            describe(prefix, trace.pc[i] - (i ? 1 : 0));
        }
        out("RDVK_CRASH end\n");
    }
    /* The previous handler (the app's crash marker, then the default action) gets the signal. */
    sigaction(sig, &previous[sig], nullptr);
    raise(sig);
}

void install_crash_report() {
    struct sigaction action{};
    action.sa_sigaction = on_fatal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : FATAL) sigaction(sig, &action, &previous[sig]);
}

/* ---------------------------------------------------------------- driver serialization */
std::mutex& driver_mutex() {
    static std::mutex m;
    return m;
}
template <typename F> struct Serial;
template <typename R, typename... A> struct Serial<R (*)(A...)> {
    template <R (**Real)(A...)> static R call(A... a) {
        std::lock_guard<std::mutex> lock(driver_mutex());
        return (*Real)(a...);
    }
};
#define RDVK_SERIAL_LIST(X)                                                                                                   \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkCreateShaderModule) X(vkCmdCopyImage) X(vkCmdBlitImage)     \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdClearColorImage)                          \
    X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdBeginRendering)
#define RDVK_SERIAL_SLOT(name) PFN_##name real_##name = nullptr;
RDVK_SERIAL_LIST(RDVK_SERIAL_SLOT)
#undef RDVK_SERIAL_SLOT
}  // namespace

int guard_init() {
    install_crash_report();
    start_ns = now_ns();
    if (property("debug.fp.rdvk_guard") == "0") {
        log("RDVK_GUARD disabled (debug.fp.rdvk_guard=0) crash_report=1");
        return 1;
    }
    const char* renderer = std::getenv("PORT_RENDERER");
    bool panvk = renderer && (!strcmp(renderer, "vulkan-panvk") || !strcmp(renderer, "native_vulkan_panvk") || !strcmp(renderer, "dxvk_panvk"));
    bool turnip = renderer && (!strcmp(renderer, "vulkan-turnip") || !strcmp(renderer, "native_vulkan_turnip") || !strcmp(renderer, "turnip"));
    std::string path = std::string(host->files_dir) + (panvk ? "/rdvk-panvk-guard.txt" : turnip ? "/rdvk-turnip-guard.txt" : "/rdvk-guard.txt");
    unsigned crashes = 0;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[256] = {};
        if (fgets(line, sizeof(line), f)) {
            unsigned n = 0;
            const char* b = strstr(line, " build=");
            const char* e = b ? strstr(b, " state=") : nullptr;
            if (sscanf(line, "crashes=%u", &n) == 1 && b && e && size_t(e - b - 7) == strlen(BUILD) && !strncmp(b + 7, BUILD, strlen(BUILD)))
                crashes = n;
        }
        fclose(f);
    }
    // An explicit native selection can retry after a previous startup crash.
    /* The APK carries no other renderer (DXVK left on 24 November 2026): every launch is native, and a
     * session after crashes runs with the serialized compiler of level 1. */
    bool forced = true;
    if (crashes >= 2 && !forced) {
        log("RDVK_SAFE_MODE level=2 crashes=%u: the native Vulkan renderer crashed in %u sessions in a row; this launch uses D3D11/DXVK "
            "(a new build, debug.fp.rdvk_guard=0 or debug.fp.renderer=vulkan tries it again)", crashes, crashes);
        return 0;
    }
    level = crashes >= 1 ? 1 : 0;
    guard_fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    char text[160];
    int n = snprintf(text, sizeof(text), "crashes=%u build=%s state=running\n", crashes, BUILD);
    if (n > 0) write_guard(text, size_t(n));
    prepare_crash_text(crashes);
    if (level) log("RDVK_SAFE_MODE level=1 crashes=%u: the previous native session crashed; driver compiler calls are serialized", crashes);
    log("RDVK_GUARD crashes=%u level=%d build=%s crash_report=1", crashes, level, BUILD);
    return 1;
}

int guard_level() { return level; }

void guard_fatal() { write_crash_marker(); }

void guard_unsupported() {
    prepare_crash_text(1);   /* "crashes=2": level 2 on the next launch */
    write_crash_marker();
    log("RDVK_SAFE_MODE unsupported driver: this build has no other renderer; choose another driver in the launcher");
}

void guard_present(uint64_t frames) {
    if (stable || guard_fd < 0 || frames % 600 || frames < 6000 || now_ns() - start_ns < 300ull * 1000000000ull) return;
    stable = true;
    char text[160];
    int n = snprintf(text, sizeof(text), "crashes=0 build=%s state=stable\n", BUILD);
    if (n > 0) write_guard(text, size_t(n));
    prepare_crash_text(0);
    log("RDVK_GUARD stable: five minutes of native frames, crash counter reset");
}

void serialize_driver(const char* reason) {
#define RDVK_SERIAL_WRAP(name) \
    if (vk.name && !real_##name) { real_##name = vk.name; vk.name = &Serial<PFN_##name>::template call<&real_##name>; }
    RDVK_SERIAL_LIST(RDVK_SERIAL_WRAP)
#undef RDVK_SERIAL_WRAP
    log("RDVK_DRIVER_SERIAL %s: pipeline/shader creation and transfer recording under one mutex", reason);
}

bool first_time(const char* key) {
    static std::mutex m;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(m);
    return seen.insert(key).second;
}

void check_image_format(VkFormat format, VkImageUsageFlags usage) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!seen.insert((uint64_t(format) << 32) | usage).second) return;
    }
    VkFormatProperties p{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &p);
    VkFormatFeatureFlags need = 0;
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) need |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) need |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormatFeatureFlags missing = need & ~p.optimalTilingFeatures;
    if (missing) log("RDVK_FORMAT_LIMITED image format=%d usage=0x%x missing_features=0x%x", int(format), usage, missing);
}

void check_vertex_format(VkFormat format) {
    static std::mutex m;
    static std::set<int> seen;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!seen.insert(int(format)).second) return;
    }
    VkFormatProperties p{};
    vk.vkGetPhysicalDeviceFormatProperties(gpu.physical, format, &p);
    if (!(p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
        log("RDVK_FORMAT_LIMITED vertex format=%d: no VERTEX_BUFFER support on this driver", int(format));
}
}  // namespace rdvk
