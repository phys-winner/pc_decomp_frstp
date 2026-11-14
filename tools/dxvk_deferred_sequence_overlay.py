"""Keep deferred staging reads behind the actual command-stream chunk that writes them."""
from pathlib import Path

FILES = ('src/d3d11/d3d11_cmdlist.cpp',)


def apply(root):
    path = Path(root) / FILES[0]
    text = path.read_text(encoding='utf-8')
    edits = (
        ('return m_chunks.size() - 1;', 'return m_chunks.size();', 2),
        ('m_resources[j].chunkId == i', 'm_resources[j].chunkId <= i + 1', 2),
        ('// This will be the chunk ID of the first chunk\n    // added, for the purpose of resource tracking.',
         '// Resource IDs are one-based; nested IDs are offset by the recorded chunk count.', 1),
        ('// If there are resources to track for the current chunk,\n      // use a strong flush hint to dispatch GPU work quickly.',
         '// Include initial no-op ID zero so it cannot block later resource tracking.', 1),
    )
    for before, after, count in edits:
        if text.count(before) != count:
            raise ValueError('DXVK deferred sequence overlay mismatch')
        text = text.replace(before, after)
    path.write_text(text, encoding='utf-8', newline='\n')
