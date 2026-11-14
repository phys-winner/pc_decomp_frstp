"""Execute the real translated UI traversal/flag/attachment guards; no device calls."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

import pefile
from fastbuild import ROOT, CLANG
from leaf_probe import windows_toolchain
from test_android_render_scale import function

out = ROOT / 'build/mobile-20261108/ui-self-draw-check'
out.mkdir(parents=True, exist_ok=True)
source = ROOT / 'generated/frostpunk-x64/recomp_funcs_0823.c'
text = source.read_text(encoding='utf-8')
names = [n for n in re.findall(r'^void L_000([0-9A-F]+)\(CPU \*c\)', text, re.M)
         if 0x140F9B260 <= int(n, 16) < 0x140F9CFE0]
names += ['140F994A0', '140F994DF']
bodies = '\n'.join(function(text, n) for n in names)
attachment_source = ROOT / 'generated/frostpunk-x64/recomp_funcs_0822.c'
bodies += '\n' + function(attachment_source.read_text(encoding='utf-8'), '140F951F0')
names += ['140F951F0']
matrix_source = ROOT / 'generated/frostpunk-x64/recomp_funcs_0800.c'
bodies += '\n' + function(matrix_source.read_text(encoding='utf-8'), '140DFA630')
names += ['140DFA630']
(out / 'ui_self_original.inc').write_text('\n'.join('void L_000' + n + '(CPU*);' for n in names) + '\n' + bodies, encoding='utf-8')
(out / 'ui_self_dispatch.inc').write_text('\n'.join('case UINT64_C(0x' + n + '):L_000' + n + '(c);return;' for n in names), encoding='utf-8')
exe = ROOT / 'build/mobile-20261108/input-photos/analysis/Frostpunk.exe'
assert hashlib.sha256(exe.read_bytes()).hexdigest() == '45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc'
pe = pefile.PE(str(exe))
addresses = sorted(set(int(a, 16) for a in re.findall(r'GVA\((0x[0-9A-Fa-f]+)', bodies)))
constants = []
for address in addresses:
    data = pe.get_data(address - pe.OPTIONAL_HEADER.ImageBase, 64)
    if data:
        constants.append('{ const unsigned char b[]={' + ','.join(str(x) for x in data) + '};memcpy((void*)(uintptr_t)GVA(UINT64_C(' + hex(address) + ')),b,sizeof(b));}')
(out / 'ui_self_constants.inc').write_text('\n'.join(constants), encoding='utf-8')
flags = ['-I' + str(ROOT / 'native/runtime64'), '-I' + str(out)]
fixture = ROOT / 'tests/android_ui_self_draw_fixture.c'
arm = out / 'ui-self-draw-arm64'
subprocess.run([str(CLANG), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wl,-z,max-page-size=16384',
                *flags, '-Wno-parentheses-equality', str(fixture), '-lm', '-o', str(arm)], check=True)
bins, env = windows_toolchain()
host = out / 'ui-self-draw-host.exe'
run = subprocess.run([str(bins / 'cl.exe'), '/nologo', '/O2', '/std:c11', '/W3',
                      *['/I' + f[2:] for f in flags], str(fixture), '/Fo' + str(out / 'ui-self-draw-host.obj'), '/Fe' + str(host)],
                     env=env, capture_output=True, text=True)
assert run.returncode == 0, run.stdout + run.stderr
run = subprocess.run([str(host)], capture_output=True, text=True)
assert run.returncode == 0, run.stdout + run.stderr
result = json.loads(run.stdout)
assert result['success']
result.update(original_functions=names, original_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
              attachment_source_sha256=hashlib.sha256(attachment_source.read_bytes()).hexdigest(),
              matrix_source_sha256=hashlib.sha256(matrix_source.read_bytes()).hexdigest(),
              extracted_original_sha256=hashlib.sha256((out / 'ui_self_original.inc').read_bytes()).hexdigest(),
              fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
              arm_bytes=arm.stat().st_size, arm_sha256=hashlib.sha256(arm.read_bytes()).hexdigest(),
              device_executed=False, dispatch_side_effects_stubbed=True)
(out / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result))
