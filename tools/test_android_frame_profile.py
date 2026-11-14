"""Deterministic actual collector fixture and ARM compile; no device actions."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from leaf_probe import windows_toolchain
import dxvk_present_overlay

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'build/android-frame-profile-test'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_output(output):
    rows = []
    for line in output.splitlines():
        if not line.startswith('PRESENT_SAMPLE '):
            continue
        row = dict(word.split('=', 1) for word in line.split()[1:])
        for key in ('generation', 'start_ns', 'end_ns', 'duration_ns', 'count',
                    'max_interval_ns', 'idle_ns', 'cumulative', 'first_frame_id',
                    'last_frame_id', 'complete', 'compositor_delivery_proven'):
            row[key] = int(row[key])
        row['presenter'] = int(row['presenter'], 16)
        assert row['method'] == 'accepted_unique_vkQueuePresent' and row['compositor_delivery_proven'] == 0
        assert row['end_ns']-row['start_ns'] == row['duration_ns']
        if row['complete']:
            assert row['duration_ns'] == 1_000_000_000
        rows.append(row)
    a = [r for r in rows if r['presenter'] == 0x100]
    assert len(a) == 5, a
    assert [r['count'] for r in a] == [2, 1, 0, 1, 1], a
    assert [r['start_ns'] for r in a[:4]] == [1_000_000_000, 2_000_000_000, 3_000_000_000, 4_000_000_000]
    assert a[0]['last_frame_id'] == 2 and a[0]['cumulative'] == 2
    assert a[3]['max_interval_ns'] == 2_500_000_000 and a[3]['cumulative'] == 4
    assert a[-1]['generation'] != a[0]['generation'] and a[-1]['cumulative'] == 1
    assert [r['count'] for r in rows if r['presenter'] in (0x200, 0x300)] == [2, 2]
    assert not any(r['presenter'] in (0x400, 0x500, 0x600, 0x700) for r in rows)
    idle = [r for r in rows if r['presenter'] == 0x800 and r['complete']]
    assert [r['count'] for r in idle] == [1, 0, 0]
    assert idle[-1]['idle_ns'] == 3_000_000_000
    assert sum(r['presenter'] == 0x2000 for r in rows) == 1
    assert [r['count'] for r in rows if r['presenter'] == 0x3000] == [30, 1]
    assert [r['count'] for r in rows if r['presenter'] == 0x4000] == [60, 1]
    for reason in ('clock', 'frame_id_regression', 'unobserved_gap', 'capacity_or_initial_clock'):
        assert 'reason='+reason in output, reason
    return rows


def overlay_checks():
    source = ROOT/'reference/dxvk'
    with tempfile.TemporaryDirectory(prefix='frostpunk-present-overlay-') as temp:
        work = Path(temp)
        dxvk_present_overlay.apply(work, source)
        target = work/dxvk_present_overlay.FILES[0]
        generated = target.read_text()
        assert generated.count('frostpunkProfilePresent(this, frameId, status);') == 1
        assert generated.count('frostpunkProfilePresent(this, 0, VK_NOT_READY);') == 1
        assert generated.index('if (frameId) frostpunkProfilePresent') < generated.index('// Try to acquire next image already')
        assert generated.index('return status;', generated.index('vkQueuePresentKHR')) < generated.index('if (frameId) frostpunkProfilePresent')
        generated_sha = sha(target)
        # Compile the actual overlaid TU, resolving unchanged relative headers
        # through the frozen working tree used by the existing DXVK build.
        cfg = json.loads((ROOT/'port.json').read_text())
        llvm = Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
        include = [ROOT/'build/dxvk-arm64/include', ROOT/'build/dxvk-source/src/dxvk',
                   ROOT/'build/dxvk-source/include', ROOT/'build/dxvk-source/include/vulkan/include',
                   ROOT/'build/dxvk-source/include/spirv/include', ROOT/'build/dxvk-source/include/native',
                   ROOT/'build/dxvk-source/include/native/windows', ROOT/'build/dxvk-source/include/native/directx',
                   ROOT/'build/dxvk-arm64']
        obj = OUT/'dxvk_presenter.cpp.o'
        command = [str(llvm/'clang++.exe'), '--target=aarch64-linux-android30', '-std=c++17', '-O2', '-fPIC',
                   '-DDXVK_WSI_ANDROID', '-D_FILE_OFFSET_BITS=64', '-Wno-microsoft-exception-spec',
                   '-Wno-extern-c-compat', *['-I'+str(p) for p in include], '-c', str(target), '-o', str(obj)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        (OUT/'dxvk_presenter.cpp.log').write_text(compiled.stdout+compiled.stderr)
        assert compiled.returncode == 0, compiled.stdout+compiled.stderr
        compiled_sha = sha(obj)
        # A reapplication and every single-byte source mutation must fail closed.
        rejected = 0
        try:
            dxvk_present_overlay.apply(work)
        except ValueError:
            rejected += 1
        else:
            raise AssertionError('Overlay reapplication accepted')
        pristine = (source/dxvk_present_overlay.FILES[0]).read_bytes()
        for position in (0, len(pristine)//2, len(pristine)-1):
            changed = bytearray(pristine); changed[position] ^= 1
            target.write_bytes(changed)
            try:
                dxvk_present_overlay.apply(work)
            except ValueError:
                rejected += 1
            else:
                raise AssertionError('Source drift accepted')
        return generated_sha, rejected, compiled_sha


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    config = json.loads((ROOT/'port.json').read_text())
    ndk = Path(config['android_sdk'])/'ndk'/config['android_ndk_version']
    clang = ndk/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    sources = [ROOT/'android/host/frame_profile.c', ROOT/'android/host/frame_profile.h',
               ROOT/'tests/android_frame_profile_fixture.c', Path(__file__).resolve(),
               ROOT/'tools/dxvk_present_overlay.py']
    before = {str(p.relative_to(ROOT)): sha(p) for p in sources}
    binaries, env = windows_toolchain()
    objects = []
    for source in (sources[0], sources[2]):
        obj = OUT/(source.stem+'.obj'); objects.append(obj)
        subprocess.run([str(clang), '--target=x86_64-pc-windows-msvc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'android/host'), '-c', str(source), '-o', str(obj)], env=env, check=True)
    binary = OUT/'fixture.exe'
    subprocess.run([str(binaries/'link.exe'), '/nologo', '/OUT:'+str(binary), *map(str, objects),
                    'libcmt.lib', 'oldnames.lib', 'kernel32.lib'], env=env, check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0 and 'FRAME_PROFILE_FIXTURE_PASS' in run.stdout, (run.returncode, run.stdout, run.stderr)
    rows = verify_output(run.stderr)
    arm = OUT/'frame_profile_fixture'
    subprocess.run([str(clang), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I'+str(ROOT/'android/host'), str(sources[0]), str(sources[2]), '-o', str(arm)], check=True)
    generated_sha, drift_rejections, presenter_object_sha = overlay_checks()
    assert before == {str(p.relative_to(ROOT)): sha(p) for p in sources}
    result = dict(success=True, host=dict(executed=True, binary_sha256=sha(binary),
                  sample_rows=len(rows), output=run.stdout, stderr=run.stderr),
                  arm=dict(compiled=True, executed=False, binary=str(arm.relative_to(ROOT)), binary_sha256=sha(arm)),
                  overlay=dict(preimage_sha256=dxvk_present_overlay.PREIMAGE, output_sha256=generated_sha,
                               drift_rejections=drift_rejections, arm_object_sha256=presenter_object_sha), source_hashes=before,
                  game_executed=False, compositor_delivery_proven=False)
    (OUT/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('host','source_hashes')}, indent=2))


if __name__ == '__main__':
    main()
