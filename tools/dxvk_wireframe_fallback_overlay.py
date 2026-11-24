"""Solid fill instead of E_INVALIDARG for wireframe rasterizer states on devices without fillModeNonSolid.

DXVK 2.4.1 refuses D3D11_FILL_WIREFRAME when the Vulkan device lacks fillModeNonSolid (Mali-G715 on
the Pixel 10a).  Frostpunk creates its rasterizer-state table (cull x scissor x bias x wire) with the
renderer; when the wireframe entries fail, the title scene keeps rebuilding render resources: in the
runs killed by lmkd the log held ~1000 "wireframe rasterization is unavailable" errors, a new 16 MiB
512x512 RGBA16F cube target every second frame, four more render targets each time, and device memory
grew 1.0 -> 2.9 GiB in 160 frames (runs that survived had 24 such errors).  Wireframe is a debug view
only, so the state is created with solid fill and the call succeeds.
"""
from pathlib import Path

FILES = ('src/d3d11/d3d11_device.cpp',)

BEFORE = ('    if (desc.FillMode == D3D11_FILL_WIREFRAME && !m_dxvkDevice->features().core.features.fillModeNonSolid) {\n'
          '      Logger::err("D3D11: wireframe rasterization is unavailable on this Vulkan device");\n'
          '      return E_INVALIDARG;\n'
          '    }\n')
AFTER = ('    if (desc.FillMode == D3D11_FILL_WIREFRAME && !m_dxvkDevice->features().core.features.fillModeNonSolid) {\n'
         '      // Frostpunk Android: see tools/dxvk_wireframe_fallback_overlay.py\n'
         '      static std::atomic<bool> s_fpWireframeLogged = { false };\n'
         '      if (!s_fpWireframeLogged.exchange(true))\n'
         '        Logger::warn("D3D11: wireframe rasterization is unavailable on this Vulkan device, using solid fill");\n'
         '      desc.FillMode = D3D11_FILL_SOLID;\n'
         '    }\n')


def apply(root):
    path = Path(root) / FILES[0]
    text = path.read_text(encoding='utf-8')
    if text.count(BEFORE) != 3:  # CreateRasterizerState, 1 and 2
        raise ValueError('DXVK wireframe fallback overlay mismatch: %d sites' % text.count(BEFORE))
    path.write_text(text.replace(BEFORE, AFTER), encoding='utf-8', newline='\n')
