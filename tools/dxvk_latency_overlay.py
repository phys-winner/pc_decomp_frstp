"""Android frame queue depth for DXVK 2.4.1 D3D11 swap chains.

DXGI swap chains here are emulated: the game's back buffer is a DXVK image that the presenter
blits into a separate Vulkan swap chain (6 images, mailbox), so upstream's clamp of the queued
frames to the D3D11 BufferCount is not needed on Android. Measured 2026-11-06 (Mali-G615, paused
city): Frostpunk creates BufferCount=2 and itself calls SetMaximumFrameLatency(1), so the queue
depth stays 1; forcing 2 did not remove the ~3 ms of GPU idle per frame (those gaps are inside
the frame, between the serialized UI layer passes) and gave no FPS, so it is not the default.

Android: queue depth = min(device maximum frame latency, 2) instead of the buffer count, which
keeps the game's own choice here; `debug.fp.frame_latency` = 1..3 overrides it for A/B
measurements. The first swap chain logs `DXVK_FRAME_LATENCY buffers=.. device=.. actual=..`.
Diagnostics: every 10 s of presents, `DXVK_MEMORY heapN_alloc_mb=.. heapN_used_mb=..` reports the
allocator's device-memory chunks against the memory its resources use (leak/fragmentation checks).
Compose after the FSR/scaling overlays.
"""
from pathlib import Path

FILES = ('src/d3d11/d3d11_swapchain.cpp',)

BEFORE = '''    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount);
    return maxFrameLatency;
  }
'''

AFTER = '''#if defined(__ANDROID__)
    {
      static int s_override = -1;
      if (s_override < 0) {
        char value[PROP_VALUE_MAX] = { };
        int parsed = __system_property_get("debug.fp.frame_latency", value) > 0 ? std::atoi(value) : 0;
        s_override = (parsed >= 1 && parsed <= 3) ? parsed : 0;
      }
      uint32_t queued = s_override ? uint32_t(s_override) : std::min(maxFrameLatency, 2u);
      static std::atomic<bool> s_logged = { false };
      if (!s_logged.exchange(true)) {
        Logger::info(str::format("DXVK_FRAME_LATENCY buffers=", m_desc.BufferCount, " device=", maxFrameLatency,
          " cap=", m_frameLatencyCap, " override=", s_override, " actual=", queued));
      }
      return queued;
    }
#endif
    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount);
    return maxFrameLatency;
  }
'''

INCLUDE_BEFORE = '#include "d3d11_swapchain.h"\n'
INCLUDE_AFTER = '''#include "d3d11_swapchain.h"
#if defined(__ANDROID__)
#include <atomic>
#include <cstdlib>
#include <time.h>
#include <sys/system_properties.h>
namespace dxvk {
  static void frostpunkLogMemory(const Rc<DxvkDevice>& device) {
    static std::atomic<int64_t> s_last = { 0 };
    timespec now = { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t last = s_last.load();
    if (int64_t(now.tv_sec) < last + 10 || !s_last.compare_exchange_strong(last, int64_t(now.tv_sec)))
      return;
    VkPhysicalDeviceMemoryProperties props = device->adapter()->memoryProperties();
    std::string line = "DXVK_MEMORY";
    for (uint32_t i = 0; i < props.memoryHeapCount; i++) {
      DxvkMemoryStats stats = device->getMemoryStats(i);
      line += str::format(" heap", i, "_alloc_mb=", stats.memoryAllocated >> 20, " heap", i, "_used_mb=", stats.memoryUsed >> 20);
    }
    Logger::info(line);
  }
}
#endif
'''

PRESENT_BEFORE = '''    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    if (!(PresentFlags & DXGI_PRESENT_TEST))
      m_dirty |= m_presenter->setSyncInterval(SyncInterval) != VK_SUCCESS;
'''
PRESENT_AFTER = '''    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
#if defined(__ANDROID__)
    frostpunkLogMemory(m_device);
#endif
    if (!(PresentFlags & DXGI_PRESENT_TEST))
      m_dirty |= m_presenter->setSyncInterval(SyncInterval) != VK_SUCCESS;
'''


def apply(work, source=None):
    work = Path(work)
    source = Path(source or work)
    name = FILES[0]
    text = (source / name).read_text(encoding='utf-8')
    for before, after in ((BEFORE, AFTER), (INCLUDE_BEFORE, INCLUDE_AFTER), (PRESENT_BEFORE, PRESENT_AFTER)):
        if text.count(before) != 1:
            raise ValueError('Latency overlay preimage mismatch: ' + name + ': ' + before[:60])
        text = text.replace(before, after, 1)
    (work / name).write_text(text, encoding='utf-8')
