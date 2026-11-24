"""Drop the native pipeline cache file when the Vulkan device is lost.

On the Pixel 10a (Mali-G715, driver 54.3.0) a pipeline cache written by one run made later DXVK
starts lose the device at the title (VK_ERROR_DEVICE_LOST at about frame 20).  A lost device never
retires its command lists, so DXVK frees nothing any more while the game keeps creating render
targets; device memory grew by ~10 MiB per frame and lmkd killed the game (ApplicationExitInfo
LOW_MEMORY) within seconds.  Without the file the same start reached the city.  When a submission
or its fence reports VK_ERROR_DEVICE_LOST, the cache file is removed and not written again, so the
next start compiles its pipelines afresh instead of loading the same blob again.
"""
from pathlib import Path

FILES = ('src/dxvk/dxvk_queue.cpp',)

EDITS = (
    ('        m_lastError = entry.result;\n',
     '        m_lastError = entry.result;\n'
     '        if (entry.result == VK_ERROR_DEVICE_LOST)\n'
     '          m_device->pipelineCache().deviceLost();\n'),
    ('        if (status != VK_SUCCESS) {\n          m_lastError = status;\n',
     '        if (status != VK_SUCCESS) {\n          m_lastError = status;\n'
     '          if (status == VK_ERROR_DEVICE_LOST)\n'
     '            m_device->pipelineCache().deviceLost();\n'),
)


def apply(root):
    path = Path(root) / FILES[0]
    text = path.read_text(encoding='utf-8')
    for before, after in EDITS:
        if text.count(before) != 1:
            raise ValueError('DXVK device-lost cache overlay mismatch: %r' % before[:50])
        text = text.replace(before, after)
    path.write_text(text, encoding='utf-8', newline='\n')
