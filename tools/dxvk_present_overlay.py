"""Pin one DXVK presenter source and observe accepted unique Android frames."""
import hashlib
from pathlib import Path

FILES = ('src/dxvk/dxvk_presenter.cpp',)
PREIMAGE = 'c718fe6807e480117ed8410f30ec5242803be276aa73db6f966ed81358763048'

HELPER = '''#if defined(__ANDROID__)
#include <cerrno>
#include <time.h>
extern "C" void frostpunk_frame_profile_present(const void*, uint64_t, uint64_t, int32_t) __attribute__((weak));
extern "C" int frostpunk_dxvk_surface_state(uint64_t, void*, uint64_t, unsigned) __attribute__((weak));

static void frostpunkProfilePresent(const void* presenter, uint64_t frameId, VkResult result) {
  if (!frostpunk_frame_profile_present) return;
  int savedErrno = errno;
  struct timespec now = { };
  uint64_t ns = 0;
  if (!clock_gettime(CLOCK_MONOTONIC, &now) && now.tv_sec >= 0 && now.tv_nsec >= 0
      && now.tv_nsec < 1000000000 && uint64_t(now.tv_sec) <= (UINT64_MAX-uint64_t(now.tv_nsec))/1000000000)
    ns = uint64_t(now.tv_sec)*1000000000+uint64_t(now.tv_nsec);
  frostpunk_frame_profile_present(presenter, frameId, ns, int32_t(result));
  errno = savedErrno;
}
#endif

'''


def apply(work, source=None):
    work, source = Path(work), Path(source or work)
    path = source / FILES[0]
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != PREIMAGE:
        raise ValueError('DXVK presenter preimage drift')
    text = raw.decode('utf-8').replace('\r\n', '\n')

    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('DXVK presenter exact source anchor drift')
        text = text.replace(old, new)

    replace('namespace dxvk {', HELPER + 'namespace dxvk {')
    replace('    swapInfo.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;', '''    swapInfo.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (auto alpha : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR }) {
      if (caps.surfaceCapabilities.supportedCompositeAlpha & alpha) {
        swapInfo.compositeAlpha = alpha;
        break;
      }
    }''')
    replace('''  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index) {
''', '''  VkResult Presenter::acquireNextImage(PresenterSync& sync, uint32_t& index) {
#if defined(__ANDROID__)
    if (frostpunk_dxvk_surface_state && frostpunk_dxvk_surface_state(uint64_t(m_surface), nullptr, 0, 1))
      return VK_ERROR_SURFACE_LOST_KHR;
#endif
''')
    replace('''  void Presenter::destroySurface() {
''', '''  void Presenter::destroySurface() {
#if defined(__ANDROID__)
    if (m_surface && frostpunk_dxvk_surface_state)
      frostpunk_dxvk_surface_state(uint64_t(m_surface), nullptr, 0, 2);
#endif
''')
    replace('''    if (!m_surface)
      return VK_ERROR_SURFACE_LOST_KHR;''', '''    if (!m_surface)
      return VK_ERROR_SURFACE_LOST_KHR;
#if defined(__ANDROID__)
    if (frostpunk_dxvk_surface_state && frostpunk_dxvk_surface_state(uint64_t(m_surface), nullptr, 0, 1))
      return VK_ERROR_SURFACE_LOST_KHR;
#endif''')
    # Present completion belongs to the acquired image, not the cyclic CPU frame.
    # https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
    replace('''    index = m_imageIndex;
    return m_acquireStatus;''', '''    sync.present = m_semaphores.at(m_imageIndex).present;
    index = m_imageIndex;
    return m_acquireStatus;''')
    replace('''    info.pWaitSemaphores    = &sync.present;''', '''    sync.present = m_semaphores.at(m_imageIndex).present;
    info.pWaitSemaphores    = &sync.present;''')
    replace('  Presenter::~Presenter() {\n', '''  Presenter::~Presenter() {
#if defined(__ANDROID__)
    // frameId zero + NOT_READY retires this metrics generation only.
    frostpunkProfilePresent(this, 0, VK_NOT_READY);
#endif
''')
    replace('''    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      return status;

    // Try to acquire next image already''', '''    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
      return status;

#if defined(__ANDROID__)
    // Repeated SyncInterval submissions use frameId zero, not a new game frame.
    if (frameId) frostpunkProfilePresent(this, frameId, status);
#endif

    // Try to acquire next image already''')
    target = work / FILES[0]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8', newline='\n')
