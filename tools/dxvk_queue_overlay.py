"""Android submission-queue recovery for DXVK 2.4.1 (no self-deadlock after a recovered GPU hang).

Measured on Adreno 650 / Turnip (KGSL), 2026-11-06: twice in two city sessions the kernel
logged `kgsl: MISC: GPU hang detected ... falut=TIMEOUTFAULT ... gpu skipcmd ctx 15
policy 80`: KGSL skipped the hung IB, retired its timestamp and kept the context. The next
timestamp wait of that context returns -EPROTO once ("faulted since the last check");
Turnip turns every failed KGSL wait into VK_TIMEOUT, also for the infinite
vkWaitForFences of DxvkCommandList::synchronizeFence(). Upstream then calls
DxvkDevice::waitForIdle() from the finish thread; that waits for m_finishQueue to drain,
which only the finish thread itself does, so the game froze forever (Renderer in
SyncFrameLatency, main thread in WaitForSingleObject) instead of losing ~2 s.

Android changes:
  * finishCmdLists: an infinite fence wait that returns VK_TIMEOUT is repeated (bounded);
    a wait that then succeeds is a recovered fault and leaves m_lastError at VK_SUCCESS, so
    Present does not start returning DXGI_ERROR_DEVICE_RESET;
  * a persistent failure becomes the queue error as before, but the device is idled with
    vkDeviceWaitIdle under the queue lock instead of the self-waiting waitForIdle();
  * submitCmdLists: the same self-wait on a failed submission (m_submitQueue still holds
    the failed entry) is replaced by logging; nothing of that submission reached the GPU.
Each recovery is logged as `DXVK_GPU_FAULT_RECOVERED count=N retries=R` for the port's
perf/hang tools. Compose after the other overlays.
"""
from pathlib import Path

FILES = ('src/dxvk/dxvk_queue.cpp',)

FINISH_BEFORE = '''        if (status != VK_ERROR_DEVICE_LOST)
          status = entry.submit.cmdList->synchronizeFence();
        
        if (status != VK_SUCCESS) {
          m_lastError = status;

          if (status != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }
'''

FINISH_AFTER = '''        if (status != VK_ERROR_DEVICE_LOST)
          status = entry.submit.cmdList->synchronizeFence();
#if defined(__ANDROID__)
        // An infinite wait cannot time out: VK_TIMEOUT here is KGSL's one-shot fault
        // marker (-EPROTO) after a recovered GPU hang, or a transient ioctl failure.
        uint32_t retries = 0;
        while (status == VK_TIMEOUT && retries < 16) {
          retries += 1;
          status = entry.submit.cmdList->synchronizeFence();
        }
        if (retries) {
          static std::atomic<uint32_t> s_recovered = { 0u };
          if (status == VK_SUCCESS) {
            uint32_t count = ++s_recovered;
            Logger::warn(str::format("DXVK_GPU_FAULT_RECOVERED count=", count, " retries=", retries));
          } else {
            Logger::err(str::format("DXVK_GPU_FAULT_UNRECOVERED status=", status, " retries=", retries));
          }
        }
#endif
        
        if (status != VK_SUCCESS) {
          m_lastError = status;

          if (status != VK_ERROR_DEVICE_LOST) {
#if defined(__ANDROID__)
            // DxvkDevice::waitForIdle() would wait for this thread's own queue.
            this->lockDeviceQueue();
            if (m_device->vkd()->vkDeviceWaitIdle(m_device->handle()) != VK_SUCCESS)
              Logger::err("DxvkSubmissionQueue: vkDeviceWaitIdle failed after a fence error");
            this->unlockDeviceQueue();
#else
            m_device->waitForIdle();
#endif
          }
        }
'''

SUBMIT_BEFORE = '''        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
        m_lastError = entry.result;

        if (m_lastError != VK_ERROR_DEVICE_LOST)
          m_device->waitForIdle();
'''

SUBMIT_AFTER = '''        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
        m_lastError = entry.result;

#if !defined(__ANDROID__)
        if (m_lastError != VK_ERROR_DEVICE_LOST)
          m_device->waitForIdle();
#endif
'''


def apply(work, source=None):
    work = Path(work)
    source = Path(source or work)
    name = FILES[0]
    text = (source / name).read_text(encoding='utf-8')
    for before, after in ((FINISH_BEFORE, FINISH_AFTER), (SUBMIT_BEFORE, SUBMIT_AFTER)):
        if text.count(before) != 1:
            raise ValueError('Queue overlay preimage mismatch: ' + name + ': ' + before[:60])
        text = text.replace(before, after)
    (work / name).write_text(text, encoding='utf-8')
