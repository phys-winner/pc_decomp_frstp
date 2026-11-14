"""Prepare bounded bridge diagnostics and test/compile them; no production writes/ADB."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-bridge-gfx-test'
BRIDGE = ROOT / 'android/dxvk/bridge.cpp'
BASE_SHA = '9d8ae33844ff434a7301a369c83135ce6255cad6dd99e80097f48d0a233617de'
TEMPLATE = ROOT / 'tools/bridge_gfx_diagnostics.inc'
FIXTURE = ROOT / 'tests/android_bridge_gfx_fixture.cpp'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def prepare(raw):
    if hashlib.sha256(raw).hexdigest() != BASE_SHA:
        raise ValueError('Bridge source preimage changed')
    text = raw.decode('utf-8')
    newline = '\r\n' if b'\r\n' in raw else '\n'
    edits = []

    def replace(before, after):
        nonlocal text
        before = before.replace('\n', newline)
        after = after.replace('\n', newline)
        if text.count(before) != 1:
            raise ValueError('Diagnostic insertion preimage changed')
        text = text.replace(before, after)
        edits.append((before, after))

    replace('#include <cstdio>\n', '#include <cstdio>\n#include <cerrno>\n#include <cfenv>\n')
    marker = 'template<class T, class R, class... A, size_t... I>\n'
    replace(marker, TEMPLATE.read_text().rstrip() + '\n\n' + marker)
    replace('        else cpu->rax = uint64_t(result);\n', '''        else cpu->rax = uint64_t(result);
        if constexpr (std::is_same_v<decltype(method),decltype(&ID3D11Device::CreateTexture2D)>) {
            if(method==&ID3D11Device::CreateTexture2D && SUCCEEDED(result)) {
                auto output=std::get<2>(arguments).get();
                if(output)gfx_observe_created_texture(*output);
            }
        }
''')
    replace('    iid_output(c,2,3,[&](REFIID iid,void** p) { return object->GetBuffer(unsigned(arg(c,1)),iid,p); });\n',
            '''    iid_output(c,2,3,[&](REFIID iid,void** p) {
        UINT index=unsigned(arg(c,1));HRESULT hr=object->GetBuffer(index,iid,p);
        if(SUCCEEDED(hr)&&*p)gfx_observe_backbuffer(static_cast<IUnknown*>(*p),index);
        return hr;
    });
''')
    reverse = text
    for before, after in reversed(edits):
        if reverse.count(after) != 1:
            raise ValueError('Ambiguous reverse provenance')
        reverse = reverse.replace(after, before)
    if reverse.encode() != raw:
        raise ValueError('Outside candidate insertions changed')
    return text.encode()


def run(command, name, env=None):
    result = subprocess.run([str(x) for x in command], cwd=OUT, env=env,
                            capture_output=True, text=True, encoding='utf-8', errors='replace')
    (OUT / (name + '.stdout')).write_text(result.stdout)
    (OUT / (name + '.stderr')).write_text(result.stderr)
    if result.returncode:
        raise RuntimeError(f'{name} failed ({result.returncode}): ' + (result.stdout + result.stderr)[-7000:])
    return result


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    names = [BRIDGE, TEMPLATE, FIXTURE, Path(__file__), ROOT / 'port.json',
             ROOT / 'android/dxvk/bridge.h', ROOT / 'android/host/win64.h',
             ROOT / 'android/host/texture_formats.h', ROOT / 'native/runtime64/cpu64.h',
             ROOT / 'tools/build_dxvk_bridge.py', ROOT / 'tools/build_startup.py',
             ROOT / 'build/dxvk-bridge/bridge_metadata.h', ROOT / 'build/dxvk-bridge/bridge_dispatch.inc']
    names += sorted((ROOT / 'reference/dxvk/include/native').rglob('*.h'))
    pins = {str(p.relative_to(ROOT)).replace('\\', '/'): sha(p) for p in names}
    raw = BRIDGE.read_bytes()
    candidate = OUT / 'bridge.cpp'
    candidate.write_bytes(prepare(raw))
    rejected = 0
    for changed in [raw + b'\n', raw.replace(b'void dxgi_get_buffer(', b'void changed_dxgi_get_buffer(', 1)]:
        try:
            prepare(changed)
        except ValueError:
            rejected += 1
        else:
            raise ValueError('Changed production preimage accepted')
    try:
        prepare(candidate.read_bytes())
    except ValueError:
        rejected += 1
    else:
        raise ValueError('Repeated candidate application accepted')
    (OUT / 'candidate.patch').write_text(''.join(difflib.unified_diff(
        raw.decode().replace('\r\n', '\n').splitlines(True), candidate.read_text().splitlines(True),
        fromfile='a/android/dxvk/bridge.cpp', tofile='b/android/dxvk/bridge.cpp')))
    (OUT / 'gfx_diagnostics.inc').write_bytes(TEMPLATE.read_bytes())
    binaries, env = windows_toolchain()
    host = OUT / 'bridge-gfx-fixture.exe'
    host_command = [binaries / 'cl.exe', '/nologo', '/O2', '/EHsc', '/std:c++17', '/fp:strict',
                    '/I' + str(OUT), FIXTURE, '/Fe' + str(host)]
    run(host_command, 'host-build', env)
    ran = run([host], 'host')
    host_result = json.loads(ran.stdout)
    rows = ran.stderr.splitlines()
    texture_rows = [r for r in rows if r.startswith('BRIDGE_GFX_TEXTURE ')]
    limits = [r for r in rows if r.startswith('BRIDGE_GFX_LIMIT ')]
    if len(texture_rows) != 80 or len(limits) != 2 or len(rows) != 82:
        raise ValueError('Repeated/capped observations leaked logs')
    if 'width=960 height=540' not in texture_rows[0]:
        raise ValueError('Descriptor log interpretation changed')
    cfg = json.loads((ROOT / 'port.json').read_text())
    llvm = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    includes = [ROOT / 'reference/dxvk/include/native/windows', ROOT / 'reference/dxvk/include/native/directx',
                ROOT / 'reference/dxvk/include/vulkan/include', ROOT / 'native/runtime64', ROOT / 'android/host',
                ROOT / 'build/dxvk-bridge', ROOT / 'android/dxvk']
    obj = OUT / 'bridge-arm64.o'
    arm_command = [llvm / 'clang++.exe', '--target=aarch64-linux-android30', '-std=c++17', '-O2', '-fPIC',
                   '-Wall', '-Wextra', '-Wno-extern-c-compat', '-Wno-missing-field-initializers',
                   *['-I' + str(p) for p in includes], '-c', candidate, '-o', obj]
    run(arm_command, 'arm-bridge-build')
    for name, digest in pins.items():
        if sha(ROOT / name) != digest:
            raise ValueError('Input changed during proof: ' + name)
    report = {'success': True, 'production_changed': False, 'gpu_or_game_executed': False,
              'source_hashes': pins, 'candidate': {'path': str(candidate.relative_to(ROOT)), 'sha256': sha(candidate)},
              'patch_sha256': sha(OUT / 'candidate.patch'), 'outside_insertions_byte_identical': True,
              'drift_rejections': rejected, 'host': host_result, 'host_executable_sha256': sha(host),
              'observed_log_lines': len(rows), 'caps': {'backbuffer': 16, 'rt_ds': 64},
              'arm': {'compiled': True, 'executed': False, 'object': str(obj.relative_to(ROOT)), 'sha256': sha(obj)},
              'compiler_sha256': {'host': sha(binaries / 'cl.exe'), 'arm': sha(llvm / 'clang++.exe')},
              'commands': {'host': list(map(str, host_command)), 'arm': list(map(str, arm_command))},
              'scope': 'Native D3D descriptors only; no per-draw hook, FPS or actual scene evidence until game run'}
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['success', 'production_changed', 'drift_rejections', 'host', 'arm']}))


if __name__ == '__main__':
    main()
