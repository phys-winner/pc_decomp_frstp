"""Compile and exercise the static Win64 CPU/native ABI and alternate stack."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT, windows_toolchain


def main():
    binaries, env = windows_toolchain()
    out = ROOT / 'build/windows-bridge'
    out.mkdir(parents=True, exist_ok=True)
    inputs = [ROOT / name for name in ['native/windows/bridge.asm',
        'native/windows/bridge.h', 'tests/windows_bridge_fixture.c',
        'tests/windows_bridge_fixture.asm', 'native/runtime64/cpu64.h',
        'tools/test_windows_bridge.py']]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    commands = []
    def run(command):
        commands.append([str(x) for x in command])
        p = subprocess.run(commands[-1], env=env, cwd=out, capture_output=True,
            text=True, encoding='utf-8', errors='replace', timeout=60)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        return p.stdout
    for source in (inputs[0], inputs[3]):
        run([binaries / 'ml64.exe', '/nologo', '/c', '/Fo' + str(out / (source.stem + '.obj')), source])
    exe = out / 'windows-bridge.exe'
    run([binaries / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', '/W4',
        '/I' + str(ROOT / 'native/runtime64'), '/I' + str(ROOT / 'native/windows'),
        '/Fo' + str(out / 'fixture-c.obj'), inputs[2], out / 'bridge.obj',
        out / 'windows_bridge_fixture.obj', '/Fe' + str(exe)])
    result = json.loads(run([exe]))
    assert result['success']
    assert result['native_calls']==204 and result['guard_page_lengths']==33 and result['guard_MXCSR_states']==5
    assert result['guard_source_no_overread'] and result['LastError_input_output'] and result['guest_stack_unchanged']
    rejected = {}
    for case in ['--invalid-stack-bytes', '--unaligned-stack-bytes', '--invalid-alt-bounds']:
        p = subprocess.run([exe, case], env=env, cwd=out, capture_output=True, timeout=15)
        rejected[case] = p.returncode
        assert p.returncode & 0xffffffff == 0xc0000409, (case, p.returncode)
    for path, digest in hashes.items():
        assert hashlib.sha256((ROOT / path).read_bytes()).hexdigest() == digest, path
    report = dict(result=result, rejected_invalid_frames=rejected, sources=hashes,
        commands=commands, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
        game_boot_proven=False, native_unwind_through_alt_stack=False)
    (out / 'self-test.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
