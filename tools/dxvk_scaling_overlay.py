"""Android swapchain extent policy for DXVK 2.4.1.

The Android WSI composes any swapchain buffer size onto the window (the loader
sets SCALE_TO_WINDOW), so a back buffer smaller than the surface is presented
at its own size and scaled by the compositor instead of a full-screen DXVK blit.
At the native size the chosen extent equals currentExtent, as before. Compose
after dxvk_present_overlay.
"""
from pathlib import Path

FILES = ('src/dxvk/dxvk_presenter.cpp',)


def apply(work, source=None):
    work = Path(work)
    source = Path(source or work)
    name = FILES[0]
    text = (source / name).read_text(encoding='utf-8')
    before = '''          VkExtent2D                desired) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
      return caps.currentExtent;
'''
    after = '''          VkExtent2D                desired) {
#if defined(__ANDROID__)
    if (!desired.width || !desired.height)
#endif
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
      return caps.currentExtent;
'''
    if text.count(before) != 1:
        raise ValueError('Scaling overlay preimage mismatch: ' + name)
    (work / name).write_text(text.replace(before, after), encoding='utf-8')
