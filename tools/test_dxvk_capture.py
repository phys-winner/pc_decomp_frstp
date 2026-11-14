"""Recompile a paired game DXBC/SPIR-V capture and validate every module offline."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    capture, out = args.capture.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    inputs, before, after = [out / name for name in ('dxbc', 'captured', 'rebuilt')]
    for path in (inputs, before, after):
        path.mkdir(exist_ok=True)
    pairs = {}
    with tarfile.open(capture) as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            name = Path(member.name).name
            if not re.fullmatch(r'(?:VS|FS|CS|TCS|TES|GS)_[0-9a-f]{40}\.(?:dxbc|spv)', name):
                raise ValueError('Unexpected capture member: ' + member.name)
            data = archive.extractfile(member).read()
            stem, suffix = name.rsplit('.', 1)
            if suffix in pairs.setdefault(stem, {}):
                raise ValueError('Duplicate capture member: ' + name)
            if suffix == 'dxbc' and hashlib.sha1(data).hexdigest() != stem.split('_', 1)[1]:
                raise ValueError('DXBC hash/name mismatch: ' + name)
            target = (inputs if suffix == 'dxbc' else before) / name
            target.write_bytes(data)
            pairs[stem][suffix] = sha(target)
    if not pairs or any(set(row) != {'dxbc', 'spv'} for row in pairs.values()):
        raise ValueError('Capture does not contain complete DXBC/SPIR-V pairs')
    cfg = json.loads((ROOT / 'port.json').read_text())
    shader_tools = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'shader-tools/windows-x86_64'
    validator, assembler = [shader_tools / (name + '.exe') for name in ('spirv-val', 'spirv-as')]
    compiler = ROOT / 'build/dxvk-robustness-shader-test/compiler-fixture.exe'
    compiler_proof = ROOT / 'build/dxvk-robustness-shader-test/result.json'
    proof = json.loads(compiler_proof.read_text())
    if not proof['success'] or any(sha(ROOT / name) != digest for name, digest in proof['inputs'].items()):
        raise ValueError('Compiler fixture source/proof drift; rerun test_dxvk_robustness_shader.py')
    production_compiler = ROOT / 'build/dxvk-source/src/dxbc/dxbc_compiler.cpp'
    fixture_compiler = ROOT / 'build/dxvk-robustness-shader-test/source/src/dxbc/dxbc_compiler.cpp'
    if production_compiler.read_bytes() != fixture_compiler.read_bytes():
        raise ValueError('Host oracle compiler differs from composed production compiler')
    manifest = out / 'dxbc-manifest.txt'
    manifest.write_text(''.join(str(inputs / (name + '.dxbc')) + '\n' for name in sorted(pairs)))
    result = subprocess.run([str(compiler), '--compile-dxbc', str(manifest), str(after)],
                            check=True, capture_output=True, text=True)
    if json.loads(result.stdout)['generated'] != len(pairs):
        raise ValueError('Rebuilt shader count differs from capture')

    def validate(path):
        run = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(path)],
                             capture_output=True, text=True)
        return dict(shader=path.stem, exit=run.returncode, error=(run.stdout + run.stderr).strip())

    with ThreadPoolExecutor(max_workers=12) as pool:
        original = list(pool.map(validate, (before / (name + '.spv') for name in sorted(pairs))))
        rebuilt = list(pool.map(validate, (after / (name + '.spv') for name in sorted(pairs))))
    failed_before = [row for row in original if row['exit']]
    failed_after = [row for row in rebuilt if row['exit']]
    # Positive/negative controls ensure the validator checks scalar and vector Select types.
    controls = []
    base = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%true = OpConstantTrue %bool
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2int = OpTypeVector %int 2
%v2float = OpTypeVector %float 2
%i0 = OpConstant %int 0
%u0 = OpConstant %uint 0
%f0 = OpConstant %float 0
%vi0 = OpConstantComposite %v2int %i0 %i0
%vf0 = OpConstantComposite %v2float %f0 %f0
%main = OpFunction %void None %fn
%entry = OpLabel
%select = OpSelect {result} %true {value} {zero}
OpReturn
OpFunctionEnd
'''
    for kind, value, wrong in [('int', 'i0', 'u0'), ('uint', 'u0', 'i0'), ('float', 'f0', 'u0'),
                               ('v2int', 'vi0', 'vf0'), ('v2float', 'vf0', 'vi0')]:
        for valid in (True, False):
            name = 'select-' + kind + ('-valid' if valid else '-invalid')
            assembly, binary = out / (name + '.spvasm'), out / (name + '.spv')
            assembly.write_text(base.format(result='%' + kind, value='%' + value,
                                            zero='%' + (value if valid else wrong)))
            subprocess.run([str(assembler), '--target-env', 'vulkan1.3', str(assembly), '-o', str(binary)],
                           check=True, capture_output=True)
            row = validate(binary)
            if (row['exit'] == 0) != valid or (not valid and 'Expected both objects to be of Result Type: Select' not in row['error']):
                raise ValueError('Select validator control failed: ' + str(row))
            controls.append(dict(kind=kind, valid=valid, expected_result=True))
    for name in pairs:
        pairs[name]['rebuilt_spv'] = sha(after / (name + '.spv'))
    report = dict(success=not failed_after, target_env='vulkan1.3', capture_sha256=sha(capture),
                  pairs=len(pairs), stages=dict(Counter(name.split('_')[0] for name in pairs)),
                  original_failures=failed_before, rebuilt_failures=failed_after,
                  original_invalid_by_stage=dict(Counter(row['shader'].split('_')[0] for row in failed_before)),
                  select_signature_controls=controls, shader_hashes=pairs,
                  compiler_fixture_sha256=sha(compiler), compiler_proof_sha256=sha(compiler_proof),
                  validator_sha256=sha(validator), source_sha256=sha(Path(__file__)),
                  production_compiler_sha256=sha(production_compiler),
                  production_compiler_matches=True,
                  game_executed=False, shader_rewrite=False,
                  compiler_options=dict(emulateRobustness2=True, minSsboAlignment=4,
                                        supportsTypedUavLoadR32=True, float32_denorm_flush=True,
                                        float32_preserve_nan=True, invariantPosition=True))
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('success', 'pairs', 'stages', 'original_invalid_by_stage')}))
    if failed_after:
        raise ValueError('Rebuilt SPIR-V validation failed: ' + str(failed_after[:3]))


if __name__ == '__main__':
    main()
