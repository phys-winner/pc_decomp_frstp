"""Transfer immediate-command buffer ownership into the legacy DXVK 2.4.1 tracker.

Adapted from upstream 5263307c: lvalue/deferred calls still retain a reference;
single-use immediate captures hand their existing reference to GPU lifetime tracking.
"""
from pathlib import Path

FILES = ('src/dxvk/dxvk_buffer.h', 'src/dxvk/dxvk_cmdlist.h',
         'src/dxvk/dxvk_context.h', 'src/dxvk/dxvk_context.cpp',
         'src/d3d11/d3d11_context_imm.cpp')


def apply(root):
    changes = {
        FILES[0]: [('void freeBufferSlice(const Rc<DxvkBuffer>& buffer, const DxvkBufferSliceHandle& slice) {\n'
                    '      m_entries.push_back({ buffer, slice });',
                    'void freeBufferSlice(Rc<DxvkBuffer> buffer, const DxvkBufferSliceHandle& slice) {\n'
                    '      m_entries.push_back({ std::move(buffer), slice });', 1)],
        FILES[1]: [('void freeBufferSlice(\n      const Rc<DxvkBuffer>&           buffer,',
                    'void freeBufferSlice(\n            Rc<DxvkBuffer>           buffer,', 1),
                   ('m_bufferTracker.freeBufferSlice(buffer, slice);',
                    'm_bufferTracker.freeBufferSlice(std::move(buffer), slice);', 1)],
        FILES[2]: [('void invalidateBuffer(\n      const Rc<DxvkBuffer>&           buffer,',
                    'void invalidateBuffer(\n            Rc<DxvkBuffer>           buffer,', 1)],
        FILES[3]: [('void DxvkContext::invalidateBuffer(\n    const Rc<DxvkBuffer>&           buffer,',
                    'void DxvkContext::invalidateBuffer(\n          Rc<DxvkBuffer>            bufferOwner,', 1),
                   ('// Allocate new backing resource\n    DxvkBufferSliceHandle prevSlice = buffer->rename(slice);\n'
                    '    m_cmd->freeBufferSlice(buffer, prevSlice);',
                    '// Allocate new backing resource\n    auto buffer = bufferOwner.ptr();\n'
                    '    DxvkBufferSliceHandle prevSlice = buffer->rename(slice);\n'
                    '    m_cmd->freeBufferSlice(std::move(bufferOwner), prevSlice);', 1)],
        FILES[4]: [],
    }
    for name, indent, count in (('cBuffer', 8, 2), ('cBuffer', 10, 1), ('cImageBuffer', 10, 1)):
        before = '] (DxvkContext* ctx) {\n' + ' ' * indent + 'ctx->invalidateBuffer(' + name + ', cBufferSlice);'
        after = '] (DxvkContext* ctx) mutable {\n' + ' ' * indent + 'ctx->invalidateBuffer(std::move(' + name + '), cBufferSlice);'
        changes[FILES[4]].append((before, after, count))
    for name, edits in changes.items():
        path = Path(root) / name
        text = path.read_text(encoding='utf-8')
        for before, after, count in edits:
            if text.count(before) != count:
                raise ValueError('DXVK buffer ownership overlay mismatch: ' + name)
            text = text.replace(before, after)
        path.write_text(text, encoding='utf-8', newline='\n')
