/* Android WSI for the native DXVK port. HWND is an acquired ANativeWindow*.
 * An embedding application's explicit guest-display policy may expose one
 * logical monitor. Its dimensions do not change the real Vulkan surface.
 * Offscreen/no-policy callers expose no monitor. Exclusive mode is unsupported.
 */
#include "wsi_platform.h"
#include "../util/util_error.h"
#include <android/native_window.h>
#include <dlfcn.h>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <vulkan/vulkan_android.h>

namespace dxvk::wsi {
static HMONITOR virtualMonitor() { return reinterpret_cast<HMONITOR>(uintptr_t(1)); }
static bool displayMode(WsiMode* mode) {
  using GetMode = int (*)(uint32_t*, uint32_t*, uint32_t*);
  auto query = reinterpret_cast<GetMode>(dlsym(RTLD_DEFAULT, "frostpunk_dxvk_get_display_mode"));
  uint32_t width = 0, height = 0, refresh = 0;
  if (!mode || !query || query(&width, &height, &refresh) != 1 || !width || !height || !refresh
      || width > uint32_t(std::numeric_limits<int32_t>::max())
      || height > uint32_t(std::numeric_limits<int32_t>::max())) return false;
  *mode = {width, height, {refresh, 1}, 32, false};
  return true;
}
static bool monitorMode(HMONITOR monitor, WsiMode* mode) {
  return monitor == virtualMonitor() && displayMode(mode);
}
static ANativeWindow* acquireCurrent(HWND window, uint64_t* generation = nullptr) {
  using Acquire = ANativeWindow* (*)(void*, uint64_t*);
  auto acquire = reinterpret_cast<Acquire>(dlsym(RTLD_DEFAULT, "frostpunk_dxvk_acquire_window"));
  if (acquire) return acquire(window, generation);
  if (generation) *generation = 0;
  auto* native = static_cast<ANativeWindow*>(window);
  if (native) ANativeWindow_acquire(native);
  return native;
}
class AndroidDriver final : public WsiDriver {
  std::mutex m_mutex;
  std::unordered_map<HWND, ANativeWindow*> m_windows;
public:
  ~AndroidDriver() {
    for (auto& entry : m_windows) ANativeWindow_release(entry.second);
  }
  std::vector<const char*> getInstanceExtensions() override {
    return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
  }
  HMONITOR getDefaultMonitor() override { WsiMode mode; return displayMode(&mode) ? virtualMonitor() : nullptr; }
  HMONITOR enumMonitors(uint32_t index) override { return index ? nullptr : getDefaultMonitor(); }
  HMONITOR enumMonitors(const LUID*[], uint32_t, uint32_t index) override { return enumMonitors(index); }
  bool getDisplayName(HMONITOR monitor, WCHAR (&name)[32]) override {
    WsiMode mode;if (!monitorMode(monitor, &mode)) return false;
    for (auto& c : name) c = 0;
    const char text[] = "\\\\.\\DISPLAY1";
    for (size_t i = 0; i < sizeof(text) - 1; ++i) name[i] = WCHAR(text[i]);
    return true;
  }
  bool getDesktopCoordinates(HMONITOR monitor, RECT* rect) override {
    WsiMode mode;if (!rect || !monitorMode(monitor, &mode)) return false;
    *rect = {0, 0, LONG(mode.width), LONG(mode.height)};return true;
  }
  bool getDisplayMode(HMONITOR monitor, uint32_t index, WsiMode* mode) override {
    return !index && monitorMode(monitor, mode);
  }
  bool getCurrentDisplayMode(HMONITOR monitor, WsiMode* mode) override { return monitorMode(monitor, mode); }
  bool getDesktopDisplayMode(HMONITOR monitor, WsiMode* mode) override { return monitorMode(monitor, mode); }
  WsiEdidData getMonitorEdid(HMONITOR) override { return {}; }
  void getWindowSize(HWND w, uint32_t* width, uint32_t* height) override {
    WsiMode mode;
    if (w && displayMode(&mode)) { if (width) *width = mode.width; if (height) *height = mode.height; return; }
    auto* window = static_cast<ANativeWindow*>(w);
    if (width) *width = window ? std::max(0, ANativeWindow_getWidth(window)) : 0;
    if (height) *height = window ? std::max(0, ANativeWindow_getHeight(window)) : 0;
  }
  void resizeWindow(HWND window, DxvkWindowState*, uint32_t width, uint32_t height) override {
    WsiMode mode;
    if (window && displayMode(&mode) && width == mode.width && height == mode.height) return;
    throw DxvkError("Android WSI: application-controlled resize is unsupported");
  }
  bool setWindowMode(HMONITOR, HWND, const WsiMode&) override { return false; }
  bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState*, bool) override { return false; }
  bool leaveFullscreenMode(HWND, DxvkWindowState*, bool) override { return false; }
  bool restoreDisplayMode() override { return false; }
  HMONITOR getWindowMonitor(HWND window) override { return window ? getDefaultMonitor() : nullptr; }
  bool isWindow(HWND w) override { return w != nullptr; }
  bool isMinimized(HWND w) override {
    auto* native = acquireCurrent(w);
    bool minimized = !native || ANativeWindow_getWidth(native) <= 0 || ANativeWindow_getHeight(native) <= 0;
    if (native) ANativeWindow_release(native);
    return minimized;
  }
  bool isOccluded(HWND w) override { return isMinimized(w); }
  void updateFullscreenWindow(HMONITOR, HWND, bool) override {
    throw DxvkError("Android WSI: fullscreen display control is unsupported");
  }
  VkResult createSurface(HWND w, PFN_vkGetInstanceProcAddr gipa,
                         VkInstance instance, VkSurfaceKHR* surface) override {
    if (!w || !surface || !gipa) return VK_ERROR_INITIALIZATION_FAILED;
    auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
      gipa(instance, "vkCreateAndroidSurfaceKHR"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    // A retained old ANativeWindow survives as an object after its consumer is gone.
    // Rebind a lost Vulkan surface to the replacement delivered by NativeActivity.
    uint64_t generation = 0;
    while (!(info.window = acquireCurrent(w, &generation)))
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VkResult result = create(instance, &info, nullptr, surface);
    if (result == VK_SUCCESS) {
      using SurfaceState = int (*)(uint64_t, void*, uint64_t, unsigned);
      auto state = reinterpret_cast<SurfaceState>(dlsym(RTLD_DEFAULT, "frostpunk_dxvk_surface_state"));
      if (state) state(uint64_t(*surface), w, generation, 0);
      std::lock_guard<std::mutex> lock(m_mutex);
      auto old = m_windows.find(w);
      if (old != m_windows.end()) ANativeWindow_release(old->second);
      m_windows[w] = info.window;
    } else {
      ANativeWindow_release(info.window);
    }
    return result;
  }
};
static bool createAndroidDriver(WsiDriver** driver) {
  *driver = new AndroidDriver();
  return true;
}
WsiBootstrap AndroidWSI = {"Android", createAndroidDriver};
}
