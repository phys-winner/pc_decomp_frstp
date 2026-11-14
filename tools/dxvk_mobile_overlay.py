"""Frostpunk mobile pass policy for DXVK 2.4.1 on Android.

Passes are recognised by the DXBC SHA-1 of their shaders (the DxvkShaderKey), tagged once in
D3D11CommonShader. Frame order after the deferred lighting: tone map (PostprocessStandard) ->
outline mask (pass-through colour into RGBA16F + scene depth) -> outline dilate -> outline
composite at output resolution -> UI blur chain -> UI. The full-screen compute is the first depth
min/max reduction for shadow-cascade fitting; it is dispatched for the screen grid although the
depth buffer has render-scale size.

Exact fixes (always on): the min/max dispatch is clamped to the depth extent (after one full grid
per binding change the skipped groups keep their constant result), the opaque full-target tone map
starts its pass with DONT_CARE instead of LOAD, and the outline dilate/composite are skipped when no
mask draw happened. "Mobile post-processing" (Graphics menu row 22 through the runtime's
frostpunk_dxvk_mobile_effects) also:
- drops hover/selection outlines;
- runs the opaque tone map at the scene (render-scale) resolution into a private target and
  scales it to the output with vkCmdBlitImage, when the scene has at most 60 % of the output
  pixels: a quarter of the tone-map invocations at Render scale 50 %. The game's own tone map
  magnifies the scene with point sampling, so the blit uses VK_FILTER_NEAREST as well.

Transient UI depth/stencil (bit 32) is diagnostic-only. On Mali-G715, physical A/B tests
show clipping/white quads with bit 32, while bit 16 alone preserves the complete UI. The
D3D11 clear recognizer cannot prove stencil lifetime across DXVK's internal render-pass splits.
debug.fp.fx overrides the policy with a bit mask for A/B tests; debug.fp.fxlog=1 logs counters
every 300 frames.
"""
from pathlib import Path

FILES = ('src/d3d11/d3d11_shader.h', 'src/d3d11/d3d11_shader.cpp',
         'src/d3d11/d3d11_context.h', 'src/d3d11/d3d11_context.cpp',
         'src/dxvk/dxvk_context.h', 'src/dxvk/dxvk_context.cpp')

# PostprocessStandard pixel shaders in common.dat (all quality permutations).
TONEMAPS = (
    '00a86a1d210c788c1b2cccbe41fb20465d3828e4', '031ee88a881bca22f25926a6c69e8022930693be',
    '0853ee91d8b04aa81d027b2bb6494812f8a3d5c4', '0ce8753aae32b1cedea7df4229078240d0900b6b',
    '0e6ca8cc16fcea13ef288fee3bdc6448eb2e6fca', '0f6aa47a1d716563fe9c6806cf3b597fd6871e1e',
    '0f8f751bdb5e0e26d0ee9d4591bb6d632461d1b3', '11b801f6ff4dba12808eeffedb740a539e7e86b4',
    '1213a073840ba310e2c85cf439953b4f17abd80a', '150cb965549cc3dca4f40745d0c2020dfb19b773',
    '170b0417051f1c33b4e50d460c3e88e572703ef5', '17ca160c3b13b406378831960f06d12c7a1b217e',
    '18cfa915555dbb6ae962174471c8f5e43122fc4f', '19a656521465ddca4dae6b00694df768428d6030',
    '1d543705914c6393b7b0c9ce8daadfcd91c5d270', '1f89bc504ec7caa5cccf5b908bb5cbd70a1cb34a',
    '22930fd18b906ffb067d4b913d5f8fb52368d13c', '238a098348ce414fcdb739fd31b986be1fc07f40',
    '23d8ca192da8de9f6a6f193ab88d0429c6159455', '242014da62789f5179f8412039b62723f8543c9c',
    '24c50c473eed776698596eee71909713d0e71375', '2720a3fee0ed1cf3f7ffda96f18ae75c204cefb0',
    '290b438c294c748c6aaf5ea5a3b20ca3942c7691', '2baabeb26a23635923926872201fbca90a1cded1',
    '2bd2f46ae03b29646b9a19154288f9b07c25e880', '2ce817df820ad2a357504cded4f8c64c54b427ab',
    '2d82c31768390bf6a8938f8a93b164b74155003d', '2e73ced9243867a1ce1a699988378b1a6e9d56fd',
    '2ed6e3d6e5ba672637519746b95d259193d5f765', '2f308453f70145a04420725d599553eff250813a',
    '317e496ba9c3c4c14010b9e63b6363d9c4b0b478', '31a77c2cbf41c7dd42c9ae227303bca5cefcac04',
    '33ee4cda53b761c7ce4ac3d6fb91e3baaec3d613', '3685c276a98744447cdfa4f4059faa60f469b3a3',
    '37b973211d3bd3f6c10420a1f3f8f2f0def0b6c0', '38974d974f520ac8692c0c6e06909a4a26daf2db',
    '3ac01d14df06efa96ba1747f5ea47949d3e73cab', '3d0e4721593e8e3a64ac75d1903370896e88d8c2',
    '437a45067cc6be4dae138226ecbbc45fe0e17c82', '44fdd70051de3494f6c7b2b115d828298d10d5ac',
    '45029b7d25e436772fb572ea5ed904f78f379eb7', '4737dd4910d3ab8832d55b90bdaebca24db52d7d',
    '4767ef098a88071bb96f2607af4b0be2af33e039', '488237e0e7720e37d6f01b219f9a57b961d8de23',
    '48e3b199b407b4c8ee58fd208800df88983e2969', '4b396161cb022157113734363e7bf65521a55a62',
    '4d79660fdf721d29405ab6ab8fb92eb730c2150a', '4e1c87d388bd613d6378a9b2c485d03554f9f844',
    '4e974ef8759183769ddd3a93d8187799f7cb9b02', '53a2483ca2d109cb8860657c50f573c8781bc57a',
    '5afd322ba08ec5d6716f2bbd96ac85c568e33acf', '5d934073a12bc58e114abe67fd1a9e8f47a7a660',
    '5db2f8542f90dfb84f657748c7581ea759faf596', '6156605d56ce476495937417a01cef630180b859',
    '61804d8318650795c3f8fe582842aeb307856692', '653f5550380cffe414fb679f30a85a54ed02f56c',
    '6669ef451e218db0c7a1cb85e6890e9403350677', '680e5f0c5c72eed64e93083a806d058b0c7f8a17',
    '681b3fb5e760ac342b2608e85dad02837cec3f91', '687e80c243c0064b8b46e080f862788bea424390',
    '6a6263470679f2a3bfe35a4138b22005d3f9f8c9', '6d6a4e7fcce2b7fa7bbfe1c05cc3e1bf20e7a98f',
    '6d7c9fa33e1dcf228bcfd33c16b74c17ce49b6d7', '71549febbb2f2ef50307bde4f05853ab950e54c3',
    '74a6c1148d1317ad33355514cb6b26e6d3b1289b', '7af3908cecc6d6720dba0f0908745afff79763e0',
    '7b2f96741c892350efa99474e949196a2c19571f', '7d244935b552a73cce03b69c2b20d6e724f9f38c',
    '7d43a175fe3cd345b54953709bc7538574f49f23', '7e1daae89277a5dce70b7b3cfd202697aaaa7584',
    '7fb1e433f8901d96eac1ebcdc2e80a4a7a382a8f', '818487c0a728aa7341fec69af4f44f1462ffe517',
    '820c657fc04d699307cee8f354549ac3e0691f81', '82d76848884427f1c81455c955bae502872cc450',
    '835bdb027a36161d687a9847674401d1c5674e65', '8361199ee76013ed8474f963c0b677e970d06331',
    '84a4f729d97cc20ef027965bfc73d4bc569a0adb', '883239ff461976e1efdc2b027ffded92098c90d0',
    '89a19a8f87758bab7cd3e57b86456db0d8c71bcc', '89af2dba4a29809f06dacd20c7ea9387355003e2',
    '8d22a70625add3cb88b56913373309026a42e074', '9102b4104ec0f4d04811eaca14c4a20aeaeeafe7',
    '916e736582fbd2836e8d651eb6befa25711d9910', '92a7170ddcaaa5838385fe9db3e6412a30f702e6',
    '92be73ec1d0859a07d5a470fe4785cc539067e93', '96cb4e2251c8a7142c999df5652918173901cfeb',
    '970d83e048d38ef21483ebe8b86a56dcc9c27941', '9a372f4692be200a7ab275d011aee7326091290b',
    '9a532c65ca1ad21bdf58e3f332acedf946ea4ce4', '9f5d74033fc99bfb674fcd208052bc253ebe3829',
    '9fec61539a3a4be0e9f416b0ee4488b0719ec004', 'a0d36ac7ae4d04c405faccdb76a7bc7d28e58add',
    'a3d0fd7fe1cdee2834c8238d9fd1f8c5be486203', 'a5a622b3e5127e9eda9cda2820211ad7f547a75a',
    'a61f0338a9ef976736b39989581199d6b94e782a', 'aa26515d1f0abfed1e6a8ad3b56d86ca19f4135b',
    'aa785c9499599ad5525979d85cd2276bb6d7bc62', 'ae8cbd9edd01cac212fcd8cd0322692952e72979',
    'aea38a2f43e96549634176032259221efe76f1e5', 'aed814f75a3b88b83e4ecfd5e62256b721d91797',
    'af0358f1f155e130727ff723c21f6736ee855a04', 'b2f21bc02febf8b4dc5f3f0e03361fb5caa5cb36',
    'b7bbeccdcb3d5a97e161cec2be85dd68f58c5183', 'b85ed418eef219b8f728147218be3b22ba6d38f6',
    'bb0d1b792388c53d0d31007623b16f20db26c35d', 'be310fe77c15e5bfa77bae8f73dfb53a47623b91',
    'c1eea27355b008932c3a339b67f7b917638c3cc3', 'c51ab6acb0024203e9ba7e5e40676d4ffc74e130',
    'ca8d9a9867300639c41998a98502985623cc8394', 'cad2251efb01328fe895c308a6005ee70888fc33',
    'cc2c1b0934c1b96177ac7e5b13483ece8f00d888', 'cd004970a14ea7ebc7b966bcd009b69e52dda354',
    'cf6362c59ea92fbb7ba4619eb2f1e8c5c60e150a', 'd0cf62e47c51594514b574cc3376a81368827a7c',
    'd4680e79da2f3ec34f7a257dac65c3193ec7c195', 'd62c967949209567ab28664ecda06836eae1f6af',
    'd68661c4f160b49de7badb67147d2a773f9a5799', 'd6f7a80eaa8d6a16f2548b70e863b3e254b3c755',
    'da5ce902b1353105f1e84c5754b30fb8b54eca66', 'dbbe437048fd4650365026df995a90259e5d19f8',
    'deda2f994f92bdcfb5cdcafdce2ed751201dd49e', 'e1eec3fd0aca953e81c7965f909971ab099ea7b2',
    'e4da7f0f3d24120ecc8f43fc0e8dbe63a03b2e87', 'e612b208a650615829b56baefd81ca15a134395d',
    'e9b7b10ee4e8295a456dc275195bf8415b4689fb', 'e9fbcddc175a4c427d0e7b0947b092cc87ba1780',
    'ed6ee10be98d2ea2fe0ab6c2b7a88281b295f5a7', 'ed90de822005cf3789e68345b1509e520431c5d4',
    'ee5c0b939f12a81f5569d1dad9e416dff057d997', 'fc0ea3c3a18630407fbfe83982af793ccc092b51',
    'fc11a073b575e33d2e246c3d8156ceb0eb4200cd', 'fe34d0da4a6b447c83343cac1f0dad33d349371d',
)
OPAQUE_TONEMAP = '238a098348ce414fcdb739fd31b986be1fc07f40'
ROLES = (('FS_' + OPAQUE_TONEMAP, 'D3D11FpRoleTonemapOpaque'),
         ('FS_c720d4239319e69819be2f7eebabd44802fd9732', 'D3D11FpRoleOutlineMask'),
         ('FS_50253911a82865dfbfe32985083cf89c2856e4bd', 'D3D11FpRoleOutlineMask'),
         ('FS_5acd0fdf37c79d843b92570d6dc73b5bc2a30b9d', 'D3D11FpRoleOutlineDilate'),
         ('FS_83543b9835a9b0c1f783ded43240920c216853fc', 'D3D11FpRoleOutlineComposite'),
         ('FS_6d2f7a7d18cc0a5099b9b0636ca885e732c42c4a', 'D3D11FpRoleOutlineComposite'),
         ('CS_2be32e59774ee0b3dbdefdd334b79232261547b0', 'D3D11FpRoleDepthMinMax'))

SHADER_H = (
    '  class D3D11Device;\n  \n',
    '''  class D3D11Device;

#if defined(__ANDROID__)
  /**
   * \\brief Frostpunk passes of the Android mobile policy
   *
   * Assigned by DXBC SHA-1 when the shader is created
   * (tools/dxvk_mobile_overlay.py).
   */
  enum D3D11FpRole : uint32_t {
    D3D11FpRoleNone,
    D3D11FpRoleTonemap,
    D3D11FpRoleTonemapOpaque,
    D3D11FpRoleOutlineMask,
    D3D11FpRoleOutlineDilate,
    D3D11FpRoleOutlineComposite,
    D3D11FpRoleDepthMinMax,
  };
#endif

''')
SHADER_ACCESSOR = (
    '''    std::string GetName() const {
      return m_shader->debugName();
    }
''',
    '''    std::string GetName() const {
      return m_shader->debugName();
    }

#if defined(__ANDROID__)
    uint32_t GetFpRole() const {
      return m_fpRole;
    }
#endif
''')
SHADER_MEMBER = (
    '    Rc<DxvkShader> m_shader;\n    Rc<DxvkBuffer> m_buffer;\n',
    '    Rc<DxvkShader> m_shader;\n    Rc<DxvkBuffer> m_buffer;\n#if defined(__ANDROID__)\n    uint32_t       m_fpRole = 0;\n#endif\n')
SHADER_ROLE = (
    '    const std::string name = pShaderKey->toString();\n',
    '    const std::string name = pShaderKey->toString();\n#if defined(__ANDROID__)\n    m_fpRole = fpShaderRole(name);\n#endif\n')
SHADER_TABLE_ANCHOR = '  D3D11CommonShader:: D3D11CommonShader() { }\n'

CONTEXT_H = (
    '    D3D11CmdData*               m_cmdData;\n',
    '''    D3D11CmdData*               m_cmdData;

#if defined(__ANDROID__)
    // Frostpunk mobile pass policy (tools/dxvk_mobile_overlay.py).
    uint32_t                    m_fpMode        = 0;
    uint32_t                    m_fpStage       = 0;
    uint32_t                    m_fpMaskDraws   = 0;
    bool                        m_fpSkipOutline = false;
    uint64_t                    m_fpMinMaxKey   = 0;
    // Tone-map output of the current frame, which the UI draws onto.
    Rc<DxvkImage>               m_fpTarget;
    // Render-scale tone-map target and the output view of a pending blit.
    Rc<DxvkImage>               m_fpLowResImage;
    Rc<DxvkImageView>           m_fpLowResView;
    Rc<DxvkImageView>           m_fpLowResOut;
    bool                        m_fpLowRes      = false;
    // Transient UI depth-stencil (registered at the first UI draw of a frame).
    Rc<DxvkImage>               m_fpUiDepth;
    float                       m_fpUiDepthValue   = 1.0f;
    bool                        m_fpUiDepthCleared = false;
    bool                        m_fpStencilReady   = false;

    bool FpSkipDraw();

    void FpAfterDraw();

    void FpBeginFrame();

    bool FpBeginLowResTonemap();

    void FpTrackUiDraw();

    void FpTrackDepthClear(D3D11DepthStencilView* pView, UINT ClearFlags, FLOAT Depth);

    void FpTransientUnsafe(const char* pReason);

    bool FpOpaqueFullTarget() const;

    bool FpOutlineMaskTarget() const;

    void FpClampDispatch(UINT& X, UINT& Y);
#endif
''')

CONTEXT_INCLUDES = (
    '#include "d3d11_context.h"\n',
    '''#include "d3d11_context.h"

#if defined(__ANDROID__)
#include <cstdio>
#include <cstdlib>
#include <sys/system_properties.h>

extern "C" int frostpunk_dxvk_mobile_effects() __attribute__((weak));
extern "C" int frostpunk_dxvk_object_highlight() __attribute__((weak));
#endif
''')

SKIP = '#if defined(__ANDROID__)\n    if (FpSkipDraw())\n      return;\n#endif\n'
AFTER_DRAW = '#if defined(__ANDROID__)\n    FpAfterDraw();\n#endif\n'
DRAW_END = '    });\n  }\n'
DEPTH_CLEAR = (
    '''    auto dsv = static_cast<D3D11DepthStencilView*>(pDepthStencilView);

    if (!dsv)
      return;

    // Figure out which aspects to clear based on''',
    '''    auto dsv = static_cast<D3D11DepthStencilView*>(pDepthStencilView);

    if (!dsv)
      return;
#if defined(__ANDROID__)
    FpTrackDepthClear(dsv, ClearFlags, Depth);
#endif

    // Figure out which aspects to clear based on''')

# DXVK core: the transient UI depth-stencil is cleared at every pass start and never stored.
DXVK_H = (
    'namespace dxvk {\n',
    '''namespace dxvk {

#if defined(__ANDROID__)
  // Frostpunk "Mobile post-processing" (tools/dxvk_mobile_overlay.py): the UI depth-stencil,
  // whose contents never cross a render pass, and its clear depth; set per frame by D3D11.
  extern const DxvkImage* g_fpTransientDepth;
  extern float            g_fpTransientDepthValue;
#endif
''')
DXVK_GLOBALS = (
    '#include "dxvk_device.h"\n#include "dxvk_context.h"\n',
    '''#include "dxvk_device.h"
#include "dxvk_context.h"

#if defined(__ANDROID__)
namespace dxvk {
  const DxvkImage* g_fpTransientDepth = nullptr;
  float            g_fpTransientDepthValue = 1.0f;
}
#endif
''')
DXVK_BIND = (
    '''      this->renderPassBindFramebuffer(
        m_state.om.framebufferInfo,
        m_state.om.renderPassOps);

      // Track the final layout of each render target''',
    '''#if defined(__ANDROID__)
      const auto& fpDepth = m_state.om.framebufferInfo.getDepthTarget();

      if (fpDepth.view != nullptr && fpDepth.view->image().ptr() == g_fpTransientDepth) {
        // Folded game clears keep their value. A resumed pass was not stored either; the D3D11
        // side switches the policy off when a UI pass tests stencil it did not clear itself.
        auto& ops = m_state.om.renderPassOps.depthOps;

        if (ops.loadOpD != VK_ATTACHMENT_LOAD_OP_CLEAR)
          ops.clearValue.depth = g_fpTransientDepthValue;

        ops.loadOpD = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ops.loadOpS = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ops.clearValue.stencil = 0u;
      }
#endif
      this->renderPassBindFramebuffer(
        m_state.om.framebufferInfo,
        m_state.om.renderPassOps);

      // Track the final layout of each render target''')
DXVK_STORE_D = (
    '''      depthInfo.loadOp = ops.depthOps.loadOpD;
      depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
''',
    '''      depthInfo.loadOp = ops.depthOps.loadOpD;
      depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
#if defined(__ANDROID__)
      if (depthTarget.view->image().ptr() == g_fpTransientDepth)
        depthInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
#endif
''')
DXVK_STORE_S = (
    '''      stencilInfo.loadOp = ops.depthOps.loadOpS;
      stencilInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
''',
    '''      stencilInfo.loadOp = ops.depthOps.loadOpS;
      stencilInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
#if defined(__ANDROID__)
      if (framebufferInfo.getDepthTarget().view->image().ptr() == g_fpTransientDepth)
        stencilInfo.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
#endif
''')
DRAWS = (
    '''  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::Draw(
          UINT            VertexCount,
          UINT            StartVertexLocation) {
    D3D10DeviceLock lock = LockContext();
''',
    '''  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawIndexed(
          UINT            IndexCount,
          UINT            StartIndexLocation,
          INT             BaseVertexLocation) {
    D3D10DeviceLock lock = LockContext();
''',
    '''  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawInstanced(
          UINT            VertexCountPerInstance,
          UINT            InstanceCount,
          UINT            StartVertexLocation,
          UINT            StartInstanceLocation) {
    D3D10DeviceLock lock = LockContext();
''',
    '''  void STDMETHODCALLTYPE D3D11CommonContext<ContextType>::DrawIndexedInstanced(
          UINT            IndexCountPerInstance,
          UINT            InstanceCount,
          UINT            StartIndexLocation,
          INT             BaseVertexLocation,
          UINT            StartInstanceLocation) {
    D3D10DeviceLock lock = LockContext();
''')
DISPATCH = (
    '''          UINT            ThreadGroupCountZ) {
    D3D10DeviceLock lock = LockContext();

    EmitCs([=] (DxvkContext* ctx) {
      ctx->dispatch(
''',
    '''          UINT            ThreadGroupCountZ) {
    D3D10DeviceLock lock = LockContext();
#if defined(__ANDROID__)
    FpClampDispatch(ThreadGroupCountX, ThreadGroupCountY);
#endif

    EmitCs([=] (DxvkContext* ctx) {
      ctx->dispatch(
''')
CONTEXT_TAIL = '  template class D3D11CommonContext<D3D11DeferredContext>;\n'
CONTEXT_CODE = r'''#if defined(__ANDROID__)
  enum : uint32_t {
    FpOutlinesOff      = 1u << 0,  // "Mobile post-processing": no hover/selection outlines
    FpClampMinMax      = 1u << 1,  // depth min/max grid sized to the depth buffer, not the screen
    FpDiscardTonemap   = 1u << 2,  // opaque full-target tone map: DONT_CARE instead of LOAD
    FpSkipEmptyOutline = 1u << 3,  // no outline dilate/composite when nothing was masked
    FpLowResTonemap    = 1u << 4,  // "Mobile post-processing": tone map at scene resolution + blit
    FpTransientUiDepth = 1u << 5,  // "Mobile post-processing": UI depth-stencil not kept across passes
    FpExact            = FpClampMinMax | FpDiscardTonemap | FpSkipEmptyOutline,
    FpMobile           = FpLowResTonemap,
  };

  struct FpStats {
    uint32_t frames, maskDraws, outlinePasses, outlinesSkipped, discards, kept, dispatches, clamped;
    uint32_t gridX, gridY, clampX, clampY, depthW, depthH, lowres, transient;
  };

  static FpStats  g_fpStats = { };
  static uint32_t g_fpPolls = 0;
  static uint32_t g_fpOverride = ~0u;
  static bool     g_fpLog = false;
  static bool     g_fpTransientUnsafe = false;

  static uint32_t fpPolicy() {
    if (!(g_fpPolls++ % 30u)) {
      char value[PROP_VALUE_MAX] = { };
      g_fpOverride = __system_property_get("debug.fp.fx", value) > 0
        ? uint32_t(std::strtoul(value, nullptr, 0)) : ~0u;
      value[0] = '\0';
      g_fpLog = __system_property_get("debug.fp.fxlog", value) > 0 && value[0] == '1';
    }

    if (g_fpOverride != ~0u)
      return g_fpOverride;

    // Stencil masks can survive DXVK pass splits; bit 32 requires an explicit diagnostic override.
    uint32_t mobile = FpMobile;
    uint32_t outlines = frostpunk_dxvk_object_highlight && !frostpunk_dxvk_object_highlight() ? FpOutlinesOff : 0u;
    return FpExact | outlines | (frostpunk_dxvk_mobile_effects && frostpunk_dxvk_mobile_effects() ? mobile : 0u);
  }

  static void fpFrame(uint32_t mode) {
    if (++g_fpStats.frames < 300u)
      return;

    if (g_fpLog) {
      std::fprintf(stderr, "FP_FX frames=%u mode=0x%x mask_draws=%u outline_passes=%u outlines_skipped=%u "
        "tonemap_discards=%u tonemap_kept=%u tonemap_lowres=%u ui_depth_transient=%u unsafe=%d "
        "minmax=%u clamped=%u grid=%ux%u->%ux%u depth=%ux%u\n",
        g_fpStats.frames, mode, g_fpStats.maskDraws, g_fpStats.outlinePasses, g_fpStats.outlinesSkipped,
        g_fpStats.discards, g_fpStats.kept, g_fpStats.lowres, g_fpStats.transient, int(g_fpTransientUnsafe),
        g_fpStats.dispatches, g_fpStats.clamped,
        g_fpStats.gridX, g_fpStats.gridY, g_fpStats.clampX, g_fpStats.clampY, g_fpStats.depthW, g_fpStats.depthH);
    }

    g_fpStats = FpStats();
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::FpSkipDraw() {
    uint32_t role = m_state.ps != nullptr
      ? m_state.ps->GetCommonShader()->GetFpRole()
      : uint32_t(D3D11FpRoleNone);

    if (role == D3D11FpRoleTonemap || role == D3D11FpRoleTonemapOpaque) {
      m_fpMode = fpPolicy();
      m_fpStage = 1;
      m_fpMaskDraws = 0;
      fpFrame(m_fpMode);
      FpBeginFrame();

      if (role == D3D11FpRoleTonemapOpaque) {
        bool full = FpOpaqueFullTarget();

        if ((m_fpMode & FpLowResTonemap) && full && FpBeginLowResTonemap()) {
          g_fpStats.lowres += 1;
        } else if ((m_fpMode & FpDiscardTonemap) && full) {
          EmitCs([cView = m_state.om.rtvs[0]->GetImageView()] (DxvkContext* ctx) {
            ctx->discardImageView(cView, VK_IMAGE_ASPECT_COLOR_BIT);
          });
          g_fpStats.discards += 1;
        } else {
          g_fpStats.kept += 1;
        }
      }
      return false;
    }

    // The hover/selection mask follows the tone map: pass-through colour into
    // an RGBA16F target with the scene depth bound.
    if (role == D3D11FpRoleOutlineMask && m_fpStage == 1 && FpOutlineMaskTarget()) {
      m_fpMaskDraws += 1;
      g_fpStats.maskDraws += 1;
      return (m_fpMode & FpOutlinesOff) != 0;
    }

    if (role == D3D11FpRoleOutlineDilate && m_fpStage == 1) {
      m_fpStage = 2;
      m_fpSkipOutline = (m_fpMode & FpOutlinesOff)
        || ((m_fpMode & FpSkipEmptyOutline) && !m_fpMaskDraws);
      g_fpStats.outlinePasses += 1;
      g_fpStats.outlinesSkipped += m_fpSkipOutline ? 1 : 0;
      return m_fpSkipOutline;
    }

    if (role == D3D11FpRoleOutlineComposite && m_fpStage == 2) {
      m_fpStage = 0;
      return m_fpSkipOutline;
    }

    m_fpStage = 0;
    FpTrackUiDraw();
    return false;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpBeginFrame() {
    m_fpTarget = m_state.om.rtvs[0] != nullptr
      ? m_state.om.rtvs[0]->GetImageView()->image()
      : nullptr;

    if (m_fpUiDepth != nullptr) {
      m_fpUiDepth = nullptr;
      EmitCs([] (DxvkContext* ctx) {
        g_fpTransientDepth = nullptr;
      });
    }

    m_fpUiDepthCleared = false;
    m_fpStencilReady = false;
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::FpBeginLowResTonemap() {
    const auto& srv = m_state.srv[DxbcProgramType::PixelShader].views[0];

    if (srv == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return false;

    Rc<DxvkImageView> input = srv->GetImageView();
    Rc<DxvkImageView> output = m_state.om.rtvs[0]->GetImageView();

    if (input == nullptr || output == nullptr)
      return false;

    // Worth it when the scene has at most 60 % of the output pixels (Render scale below 100 %).
    VkExtent3D in = input->mipLevelExtent(0);
    VkExtent3D out = output->mipLevelExtent(0);

    if (uint64_t(in.width) * in.height * 5u > uint64_t(out.width) * out.height * 3u)
      return false;

    const DxvkImageCreateInfo& outInfo = output->image()->info();

    if (m_fpLowResImage == nullptr
     || m_fpLowResImage->info().extent.width != in.width
     || m_fpLowResImage->info().extent.height != in.height
     || m_fpLowResImage->info().format != outInfo.format
     || m_fpLowResView->info().format != output->info().format) {
      // Same format and view-format list as the output, so the tone-map pipeline is unchanged.
      DxvkImageCreateInfo info = outInfo;
      info.extent = { in.width, in.height, 1u };
      info.mipLevels = 1;
      info.numLayers = 1;
      info.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      info.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      info.access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      info.shared = VK_FALSE;
      info.sharing = DxvkSharedHandleInfo();
      m_fpLowResImage = m_device->createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      DxvkImageViewCreateInfo view = output->info();
      view.type = VK_IMAGE_VIEW_TYPE_2D;
      view.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      view.minLevel = 0;
      view.numLevels = 1;
      view.minLayer = 0;
      view.numLayers = 1;
      m_fpLowResView = m_device->createImageView(m_fpLowResImage, view);
    }

    // D3D11 viewports are flipped by DXVK; same depth range as the game's.
    const D3D11_VIEWPORT& d3dvp = m_state.rs.viewports[0];
    VkViewport vp = { 0.0f, float(in.height), float(in.width), -float(in.height), d3dvp.MinDepth, d3dvp.MaxDepth };
    VkRect2D sc = { { 0, 0 }, { in.width, in.height } };

    EmitCs([cView = m_fpLowResView, vp, sc] (DxvkContext* ctx) {
      DxvkRenderTargets targets;
      targets.color[0] = { cView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
      ctx->bindRenderTargets(std::move(targets), 0u);
      ctx->discardImageView(cView, VK_IMAGE_ASPECT_COLOR_BIT);
      ctx->setViewports(1, &vp, &sc);
    });

    m_fpLowResOut = output;
    m_fpLowRes = true;
    return true;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpAfterDraw() {
    if (!m_fpLowRes)
      return;

    m_fpLowRes = false;

    // Back to the game's render target and viewport, then scale the tone map onto it.
    BindFramebuffer();
    ApplyViewportState();

    VkExtent3D src = m_fpLowResImage->info().extent;
    VkExtent3D dst = m_fpLowResOut->mipLevelExtent(0);

    VkImageBlit region = { };
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
    region.srcOffsets[1] = { int32_t(src.width), int32_t(src.height), 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m_fpLowResOut->info().minLevel, m_fpLowResOut->info().minLayer, 1u };
    region.dstOffsets[1] = { int32_t(dst.width), int32_t(dst.height), 1 };

    EmitCs([cDst = m_fpLowResOut->image(), cSrc = m_fpLowResImage, region] (DxvkContext* ctx) {
      VkComponentMapping identity = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
      ctx->blitImage(cDst, identity, cSrc, identity, region, VK_FILTER_NEAREST);
    });

    m_fpLowResOut = nullptr;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpTrackUiDraw() {
    if (!(m_fpMode & FpTransientUiDepth) || g_fpTransientUnsafe || m_fpTarget == nullptr)
      return;

    const auto& rtv = m_state.om.rtvs[0];
    Rc<DxvkImage> depth = m_state.om.dsv != nullptr
      ? m_state.om.dsv->GetImageView()->image()
      : nullptr;

    if (rtv == nullptr || depth == nullptr || rtv->GetImageView()->image() != m_fpTarget) {
      if (depth != nullptr && depth == m_fpUiDepth)
        FpTransientUnsafe("UI depth-stencil bound with another target");

      // Alpha-mask draws split the UI: the next UI pass starts with stencil 0.
      m_fpStencilReady = false;
      return;
    }

    if (m_fpUiDepth == nullptr) {
      VkExtent3D a = depth->info().extent;
      VkExtent3D b = m_fpTarget->info().extent;

      if (!m_fpUiDepthCleared || a.width != b.width || a.height != b.height
       || depth->info().sampleCount != VK_SAMPLE_COUNT_1_BIT)
        return;

      m_fpUiDepth = depth;
      g_fpStats.transient += 1;

      EmitCs([cImage = depth, cValue = m_fpUiDepthValue] (DxvkContext* ctx) {
        g_fpTransientDepth = cImage.ptr();
        g_fpTransientDepthValue = cValue;
      });
    } else if (depth != m_fpUiDepth) {
      FpTransientUnsafe("second UI depth-stencil");
      return;
    }

    if (m_state.om.dsState == nullptr) {
      FpTransientUnsafe("UI draw with the default depth-stencil state");
      return;
    }

    D3D11_DEPTH_STENCIL_DESC desc = { };
    m_state.om.dsState->GetDesc(&desc);

    if (desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL)
      FpTransientUnsafe("UI depth write");
    else if (desc.StencilEnable && !m_fpStencilReady)
      FpTransientUnsafe("UI stencil test before a stencil clear");
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpTrackDepthClear(D3D11DepthStencilView* pView, UINT ClearFlags, FLOAT Depth) {
    if (m_fpTarget == nullptr)
      return;

    Rc<DxvkImage> image = pView->GetImageView()->image();
    VkExtent3D a = image->info().extent;
    VkExtent3D b = m_fpTarget->info().extent;

    if (a.width != b.width || a.height != b.height)
      return;

    if (ClearFlags & D3D11_CLEAR_STENCIL)
      m_fpStencilReady = true;

    if (ClearFlags & D3D11_CLEAR_DEPTH) {
      if (image == m_fpUiDepth && Depth != m_fpUiDepthValue) {
        EmitCs([cValue = Depth] (DxvkContext* ctx) {
          g_fpTransientDepthValue = cValue;
        });
      }

      m_fpUiDepthValue = Depth;
      m_fpUiDepthCleared = true;
    }
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpTransientUnsafe(const char* pReason) {
    if (!g_fpTransientUnsafe) {
      g_fpTransientUnsafe = true;
      std::fprintf(stderr, "FP_FX transient_ui_depth=off reason=%s\n", pReason);
    }

    m_fpUiDepth = nullptr;
    EmitCs([] (DxvkContext* ctx) {
      g_fpTransientDepth = nullptr;
    });
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::FpOpaqueFullTarget() const {
    if (m_state.om.rtvs[0] == nullptr || m_state.om.dsv != nullptr)
      return false;

    for (uint32_t i = 1; i < m_state.om.rtvs.size(); i++) {
      if (m_state.om.rtvs[i] != nullptr)
        return false;
    }

    Rc<DxvkImageView> view = m_state.om.rtvs[0]->GetImageView();

    if (view == nullptr || view->info().numLayers != 1)
      return false;

    VkExtent3D extent = view->mipLevelExtent(0);
    const D3D11_VIEWPORT& vp = m_state.rs.viewports[0];

    if (!m_state.rs.numViewports || vp.TopLeftX > 0.0f || vp.TopLeftY > 0.0f
     || vp.TopLeftX + vp.Width < float(extent.width) || vp.TopLeftY + vp.Height < float(extent.height))
      return false;

    if (m_state.rs.state != nullptr && m_state.rs.state->Desc()->ScissorEnable) {
      const D3D11_RECT& sc = m_state.rs.scissors[0];

      if (!m_state.rs.numScissors || sc.left > 0 || sc.top > 0
       || sc.right < LONG(extent.width) || sc.bottom < LONG(extent.height))
        return false;
    }

    if (m_state.om.cbState != nullptr) {
      D3D11_BLEND_DESC1 desc = { };
      m_state.om.cbState->GetDesc1(&desc);

      const D3D11_RENDER_TARGET_BLEND_DESC1& rt = desc.RenderTarget[0];

      if (rt.BlendEnable || rt.LogicOpEnable || (rt.RenderTargetWriteMask & 0xf) != 0xf)
        return false;
    }

    return true;
  }


  template<typename ContextType>
  bool D3D11CommonContext<ContextType>::FpOutlineMaskTarget() const {
    if (m_state.om.rtvs[0] == nullptr || m_state.om.dsv == nullptr)
      return false;

    D3D11_RENDER_TARGET_VIEW_DESC desc = { };
    m_state.om.rtvs[0]->GetDesc(&desc);
    return desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  }


  template<typename ContextType>
  void D3D11CommonContext<ContextType>::FpClampDispatch(UINT& X, UINT& Y) {
    if (m_state.cs == nullptr || m_state.cs->GetCommonShader()->GetFpRole() != D3D11FpRoleDepthMinMax)
      return;

    g_fpStats.dispatches += 1;
    g_fpStats.gridX = X;
    g_fpStats.gridY = Y;

    if (!(m_fpMode & FpClampMinMax))
      return;

    const auto& srv = m_state.srv[DxbcProgramType::ComputeShader].views[10];
    const auto& uav = m_state.uav.views[0];

    if (srv == nullptr || uav == nullptr || srv->GetResourceType() != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;

    Rc<DxvkImageView> depth = srv->GetImageView();
    Rc<DxvkImageView> target = uav->GetImageView();

    if (depth == nullptr || target == nullptr)
      return;

    VkExtent3D extent = depth->mipLevelExtent(0);
    g_fpStats.depthW = extent.width;
    g_fpStats.depthH = extent.height;

    // 16x16 threads per group. Groups past the depth extent only write a constant
    // result (neutral, or the out-of-bounds depth 0); after one full grid for each
    // depth/target/grid combination those texels hold it, so later frames skip them.
    uint64_t key = uint64_t(uintptr_t(depth->image().ptr()));
    key = key * 1000003u ^ uint64_t(uintptr_t(target->image().ptr()));
    key = key * 1000003u ^ (uint64_t(extent.width) << 32 | extent.height);
    key = key * 1000003u ^ (uint64_t(X) << 32 | Y);

    if (key != m_fpMinMaxKey) {
      m_fpMinMaxKey = key;
      return;
    }

    X = std::min(X, UINT((extent.width + 15u) / 16u));
    Y = std::min(Y, UINT((extent.height + 15u) / 16u));
    g_fpStats.clamped += 1;
    g_fpStats.clampX = X;
    g_fpStats.clampY = Y;
  }
#endif


'''


def shader_table():
    lines = ['#if defined(__ANDROID__)',
             '  // Frostpunk passes by DXBC SHA-1 for the Android mobile policy (tools/dxvk_mobile_overlay.py).',
             '  static const char* const g_fpTonemaps[] = {']
    lines += ['    "%s",' % value for value in TONEMAPS]
    lines += ['  };', '', '  static uint32_t fpShaderRole(const std::string& name) {',
              '    static const std::pair<const char*, uint32_t> roles[] = {']
    lines += ['      { "%s", %s },' % row for row in ROLES]
    lines += ['    };', '',
              '    for (const auto& role : roles) {',
              '      if (name == role.first)',
              '        return role.second;',
              '    }', '',
              '    if (name.size() == 43 && !name.compare(0, 3, "FS_")) {',
              '      for (const char* hash : g_fpTonemaps) {',
              '        if (!name.compare(3, 40, hash))',
              '          return D3D11FpRoleTonemap;',
              '      }',
              '    }', '',
              '    return D3D11FpRoleNone;',
              '  }',
              '#endif', '', '']
    return '\n'.join(lines)


def apply(work, source=None):
    if len(TONEMAPS) != 132 or OPAQUE_TONEMAP not in TONEMAPS:
        raise ValueError('Mobile overlay tone-map table is incomplete')
    work = Path(work)
    source = Path(source or work)
    contents = {name: (source / name).read_text(encoding='utf-8') for name in FILES}

    def change(name, before, after):
        if contents[name].count(before) != 1:
            raise ValueError('Mobile overlay preimage mismatch: ' + name + ': ' + before[:70])
        contents[name] = contents[name].replace(before, after)

    for pair in (SHADER_H, SHADER_ACCESSOR, SHADER_MEMBER):
        change(FILES[0], *pair)
    change(FILES[1], *SHADER_ROLE)
    change(FILES[1], SHADER_TABLE_ANCHOR, shader_table() + SHADER_TABLE_ANCHOR)
    change(FILES[2], *CONTEXT_H)
    change(FILES[3], *CONTEXT_INCLUDES)
    for draw in DRAWS:
        change(FILES[3], draw, draw + SKIP)
        # The low-resolution tone map restores the game's target and blits after its draw.
        text = contents[FILES[3]]
        end = text.index(DRAW_END, text.index(draw))
        if '\n  }\n' in text[text.index(draw):end]:
            raise ValueError('Mobile overlay draw function end not found: ' + draw.splitlines()[0])
        end += len('    });\n')
        contents[FILES[3]] = text[:end] + AFTER_DRAW + text[end:]
    change(FILES[3], *DISPATCH)
    change(FILES[3], *DEPTH_CLEAR)
    change(FILES[3], CONTEXT_TAIL, CONTEXT_CODE + CONTEXT_TAIL)
    change(FILES[4], *DXVK_H)
    for pair in (DXVK_GLOBALS, DXVK_BIND, DXVK_STORE_D, DXVK_STORE_S):
        change(FILES[5], *pair)
    for name, text in contents.items():
        path = work / name
        data = text.encode('utf-8')
        if not path.exists() or path.read_bytes() != data:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
