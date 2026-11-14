"""Mechanical preimage and preservation checks; device fixtures prove rendering."""
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import dxvk_mali_overlay as M

with tempfile.TemporaryDirectory(prefix='frostpunk-mali-overlay-') as folder:
    work = Path(folder)
    for name in M.FILES:
        dst = work / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'reference/dxvk' / name, dst)
    M.apply(work)
    compiler = (work / M.FILES[4]).read_text()
    assert compiler.count('enableCapability(spv::CapabilityClipDistance)') == 1
    assert compiler.count('enableCapability(spv::CapabilityCullDistance)') == 1
    assert 'clips && !moduleInfo.options.supportsClipDistance' in compiler
    device = (work / M.FILES[0]).read_text()
    assert 'vendorID == 0x13b5 || Adapter->deviceProperties().vendorID == 0x1010' in device
    assert device.count('D3D11_FILL_WIREFRAME &&') == 3
    assert 'enabled.core.features.textureCompressionBC = supported.core.features.textureCompressionBC' in device
    assert 'return E_INVALIDARG;' in device
    assert '!m_dxvkDevice->features().extTransformFeedback.geometryStreams)' in device
    assert device.count('D3D11: dual-source blending is unavailable') == 2
    try:
        M.apply(work)
    except ValueError:
        pass
    else:
        raise AssertionError('Double application must reject an altered upstream preimage')
print('MALI_OVERLAY preimages=5 conditional_capabilities=1 raster_guards=3 drift_rejected=1 PASS')
