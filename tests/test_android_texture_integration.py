"""The full runtime must reject stale private-format sources and proof inputs."""
from pathlib import Path
import json
import sys
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import build_startup as build

original=json.loads((ROOT/'port.json').read_text())['original_exe_sha256']
source,pins,proof=build.verified_texture_formats(original)
assert source==ROOT/'android/host/texture_formats.c' and len(pins)>12
assert proof['targets']==['0x140d95650','0x140f499a0']
def rejects(action):
    try: action()
    except ValueError: return
    raise AssertionError('Changed private-format proof was accepted')
rejects(lambda:build.verified_texture_formats('0'*64))
real=build.sha256
for name in ['android/host/texture_formats.h','generated/frostpunk-x64/recomp_meta.json',
             'build/android-texture-formats/texture-formats-arm64']:
    target=(ROOT/name).resolve()
    with patch.object(build,'sha256',side_effect=lambda p: '0'*64 if Path(p).resolve()==target else real(p)):
        rejects(lambda:build.verified_texture_formats(original))
print('PRIVATE_TEXTURE_INTEGRATION positive=1 changed_inputs_rejected=4 PASS')
