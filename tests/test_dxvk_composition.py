"""Exercise the actual producer order and preserve incremental-build mtimes."""
from pathlib import Path
import hashlib
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_dxvk as producer

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

originals = {name: digest(producer.SOURCE / name) for name in producer.OVERLAY_FILES
             if (producer.SOURCE / name).exists()}
with tempfile.TemporaryDirectory(prefix='frostpunk-dxvk-compose-test-') as directory:
    producer.WORK = Path(directory)
    shutil.copytree(producer.SOURCE, producer.WORK, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('.git'))
    # Real non-overlay Android input must survive both the first and repeated
    # composition and participate in the content-derived GPU cache identity.
    wsi = producer.WORK / 'src/wsi/wsi_android.cpp'
    shutil.copy2(ROOT / 'android/dxvk/wsi_android.cpp', wsi)
    wsi_hash = digest(wsi)
    initial = producer.prepare_gpu_overlays()
    first = {name: (digest(producer.WORK / name), (producer.WORK / name).stat().st_mtime_ns)
             for name in producer.OVERLAY_FILES}
    compiler = (producer.WORK / 'src/dxbc/dxbc_compiler.cpp').read_text()
    assert 'FROSTPUNK_ROBUSTNESS_ABI_1' in compiler
    assert 'supportsClipDistance' in compiler
    assert 'FP_DXGI_FORMAT_ASTC_6X6_SRGB' in (producer.WORK / 'src/dxgi/dxgi_astc_formats.h').read_text()
    assert 'createRobustnessMetadata' in (producer.WORK / 'src/dxvk/dxvk_context.cpp').read_text()
    assert 'frostpunk_frame_profile_present' in (producer.WORK / 'src/dxvk/dxvk_presenter.cpp').read_text()
    repeated = producer.prepare_gpu_overlays()
    assert initial['fingerprint_sha256'] == repeated['fingerprint_sha256']
    assert initial['outputs'] == repeated['outputs']
    assert initial['gpu_source_hashes']['src/wsi/wsi_android.cpp'] == wsi_hash == digest(wsi)
    assert first == {name: (digest(producer.WORK / name), (producer.WORK / name).stat().st_mtime_ns)
                     for name in producer.OVERLAY_FILES}
    wsi.write_bytes(wsi.read_bytes() + b'\n')
    changed = producer.prepare_gpu_overlays()
    assert changed['fingerprint_sha256'] != initial['fingerprint_sha256']
    after_change = {name: (digest(producer.WORK / name), (producer.WORK / name).stat().st_mtime_ns)
                    for name in producer.OVERLAY_FILES}
    assert producer.prepare_gpu_overlays()['fingerprint_sha256'] == changed['fingerprint_sha256']
    assert after_change == {name: (digest(producer.WORK / name), (producer.WORK / name).stat().st_mtime_ns)
                            for name in producer.OVERLAY_FILES}
assert originals == {name: digest(producer.SOURCE / name) for name in originals}
print(f'DXVK_COMPOSITION overlays={len(producer.OVERLAYS)} files={len(first)} repeated_mtimes_unchanged=1 original_preserved=1 PASS')
