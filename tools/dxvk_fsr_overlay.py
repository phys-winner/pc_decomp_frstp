"""Optional AMD FSR 1 EASU in the Android presentation pass."""
from pathlib import Path
import hashlib

ROOT = Path(__file__).resolve().parents[1]
FILES = ('src/dxvk/shaders/dxvk_present_frag_fsr.frag',
         'src/dxvk/shaders/ffx_a.h', 'src/dxvk/shaders/ffx_fsr1.h',
         'src/dxvk/dxvk_swapchain_blitter.cpp', 'src/dxvk/dxvk_presenter.cpp',
         'src/d3d11/d3d11_swapchain.cpp', 'src/d3d11/d3d11_swapchain.h',
         'src/dxvk/dxvk_swapchain_blitter.h', 'src/dxvk/meson.build',
         'src/dxvk/shaders/dxvk_present_frag_blit.frag')
CALLBACK = '''#if defined(__ANDROID__)
extern "C" int frostpunk_dxvk_upscaler() __attribute__((weak));
#endif
'''


def apply(work):
    work = Path(work)
    def patch(name, before, after):
        path = work / name
        text = path.read_text(encoding='utf-8')
        if text.count(before) != 1:
            raise ValueError('FSR overlay preimage mismatch: ' + name + ': ' + before[:60])
        path.write_text(text.replace(before, after), encoding='utf-8')
    shader = work / FILES[0]
    shader.write_bytes((ROOT / 'android/dxvk/fsr1_present.frag').read_bytes())
    for name in ('ffx_a.h', 'ffx_fsr1.h'):
        source = ROOT / 'reference/fsr1/ffx-fsr' / name
        expected={'ffx_a.h':'c82c2092e0739121f9c26aee5b6afec765d44466f010518cbc43fdbc6dcbc7b7',
                  'ffx_fsr1.h':'6d16231b29f0537f7737aebdb4affabba6da114247fecb8da93b060511659ce0'}
        if hashlib.sha256(source.read_bytes()).hexdigest()!=expected[name]:
            raise ValueError('AMD FSR 1 input changed: '+name)
        (shader.parent / name).write_bytes(source.read_bytes())
    for name in (FILES[3], FILES[4], FILES[5]):
        patch(name, 'namespace dxvk {', CALLBACK + '\nnamespace dxvk {')
    patch(FILES[8], "  'shaders/dxvk_present_frag_blit.frag',", "  'shaders/dxvk_present_frag_blit.frag',\n  'shaders/dxvk_present_frag_fsr.frag',")
    patch(FILES[7], '    Rc<DxvkShader>      m_fsBlit;', '    Rc<DxvkShader>      m_fsBlit;\n    Rc<DxvkShader>      m_fsFsr;')
    patch(FILES[3], '#include <dxvk_present_frag_blit.h>', '#include <dxvk_present_frag_blit.h>\n#include <dxvk_present_frag_fsr.h>')
    patch(FILES[3], '    m_fsBlit = new DxvkShader(fsInfo, std::move(fsCodeBlit));', '''    m_fsBlit = new DxvkShader(fsInfo, std::move(fsCodeBlit));
    fsInfo.inputMask = 0;
    m_fsFsr = new DxvkShader(fsInfo, SpirvCodeBuffer(dxvk_present_frag_fsr));''')
    patch(FILES[3], '      this->draw(ctx, sameSize ? m_fsCopy : m_fsBlit,', '''      bool fsr = frostpunk_dxvk_upscaler && frostpunk_dxvk_upscaler() == 1
        && dstRect.extent.width > srcRect.extent.width && dstRect.extent.height > srcRect.extent.height
        && !dstRect.offset.x && !dstRect.offset.y && !srcRect.offset.x && !srcRect.offset.y;
      if (fsr) {
        static bool logged = false;
        if (!logged) { logged = true; Logger::info(str::format("FP_FSR1 EASU ", srcRect.extent.width, "x", srcRect.extent.height,
          " -> ", dstRect.extent.width, "x", dstRect.extent.height)); }
      }
      this->draw(ctx, fsr ? m_fsFsr : sameSize ? m_fsCopy : m_fsBlit,''')
    patch(FILES[4], '    if (!desired.width || !desired.height)',
          '    if (!desired.width || !desired.height || (frostpunk_dxvk_upscaler && frostpunk_dxvk_upscaler()))')
    patch(FILES[6], '    bool                      m_dirty = true;',
          '    bool                      m_dirty = true;\n    int                       m_fpUpscaler = 0;')
    patch(FILES[5], '    if (std::exchange(m_dirty, false))', '''#if defined(__ANDROID__)
    int upscaler = frostpunk_dxvk_upscaler && frostpunk_dxvk_upscaler() != 0;
    if (upscaler != m_fpUpscaler) { m_fpUpscaler = upscaler; m_dirty = true; }
#endif
    if (std::exchange(m_dirty, false))''')
    patch(FILES[5], '''    if (status != VK_SUCCESS)
      RecreateSwapChain();
  }


  void D3D11SwapChain::RecreateSwapChain()''', '''#if defined(DXVK_WSI_ANDROID)
    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
#else
    if (status != VK_SUCCESS)
#endif
      RecreateSwapChain();
  }


  void D3D11SwapChain::RecreateSwapChain()''')
    patch(FILES[3], '    ctx->bindResourceSampler(VK_SHADER_STAGE_FRAGMENT_BIT, BindingIds::Image, Rc<DxvkSampler>(m_samplerPresent));', '''    bool fsr = fs == m_fsFsr;
    ctx->bindResourceSampler(VK_SHADER_STAGE_FRAGMENT_BIT, BindingIds::Image,
      Rc<DxvkSampler>(fsr ? m_samplerGamma : m_samplerPresent));''')
    patch(FILES[3], '    ctx->draw(3, 1, 0, 0);', '''    if (fsr) {
      ctx->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, 3, dstRect.extent.width);
      ctx->setSpecConstant(VK_PIPELINE_BIND_POINT_GRAPHICS, 4, dstRect.extent.height);
    }
    ctx->draw(3, 1, 0, 0);''')
