"""Android Mali submission policy for DXVK 2.4.1.

On this driver every vkQueueSubmit drains the GPU pipeline, and the D3D11
flush hints split a frame into three or four submissions. Hints therefore do
not flush on Android; Present, explicit Flush and synchronization (Map of a
busy resource, query polling) still do, so readbacks and queries keep their
behaviour. Compose after the other overlays.
"""
from pathlib import Path

FILES = ('src/util/util_flush.cpp',)


def apply(work, source=None):
    work = Path(work)
    source = Path(source or work)
    name = FILES[0]
    text = (source / name).read_text(encoding='utf-8')
    before = '''    // Take any earlier missed flush with a stronger hint into account, so
    // that we still flush those as soon as possible. Ignore synchronization
    // commands since they will either perform a flush or not need it at all.
    flushType = std::min(flushType, m_lastMissedType);
'''
    after = '''#if defined(__ANDROID__)
    // One submission per frame on tilers: hints never flush here, and a
    // missed hint never weakens a later synchronization flush.
    if (flushType == GpuFlushType::ImplicitStrongHint || flushType == GpuFlushType::ImplicitWeakHint)
      return false;
#endif
''' + before
    if text.count(before) != 1:
        raise ValueError('Tiler overlay preimage mismatch: ' + name)
    (work / name).write_text(text.replace(before, after), encoding='utf-8')
