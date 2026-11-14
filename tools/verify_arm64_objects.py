"""Verify every cached AOT object, including rejection of mismatched compile inputs."""
import datetime
import hashlib
import json
from pathlib import Path
from unittest.mock import patch
import build_lifted
import build_startup

ROOT=Path(__file__).resolve().parents[1]

def main():
    config=json.loads((ROOT/'port.json').read_text())
    generated=ROOT/'generated/frostpunk-x64'
    metadata_path=generated/'recomp_meta.json'
    metadata=json.loads(metadata_path.read_text())
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    objects,_=build_startup.verified_full_objects(metadata,generated,clang)
    total=0;manifest=[]
    for obj in objects:
        with obj.open('rb') as stream:elf=stream.read(20)
        assert elf[:6]==b'\x7fELF\x02\x01' and int.from_bytes(elf[16:18],'little')==1 and int.from_bytes(elf[18:20],'little')==183,obj
        total+=obj.stat().st_size;manifest.append((str(obj.relative_to(ROOT)),build_startup.sha256(obj)))
    digest=build_startup.sha256
    header=ROOT/'native/runtime64/cpu64.h'
    with patch.object(build_startup,'sha256',side_effect=lambda path:'0'*64 if path==header else digest(path)):
        try:build_startup.verified_full_objects(metadata,generated,clang)
        except ValueError as e:assert 'compiler/options/headers changed' in str(e)
        else:raise AssertionError('Changed CPU header accepted')
    with patch.object(build_lifted,'fingerprint',return_value='0'*64):
        try:build_startup.verified_full_objects(metadata,generated,clang)
        except ValueError as e:assert 'object provenance mismatch' in str(e)
        else:raise AssertionError('Mismatched source/options fingerprint accepted')
    assert build_startup.verified_full_objects(metadata,generated,clang)[0]==objects
    receipt=json.loads((ROOT/'research/arm64-compile.json').read_text())
    report={'checked_at_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'scope':'Cached AOT object identity and compiler input verification; no game execution',
        'objects':len(objects),'total_object_bytes':total,'all_elf64_little_endian_aarch64_et_rel':True,
        'all_object_sha256_match_stamps':True,'all_source_options_fingerprints_match':True,
        'changed_runtime_header_rejected':True,'mismatched_object_fingerprint_rejected':True,
        'negative_checks_modify_files':False,'compile_counts':receipt['counts'],'compile_seconds':receipt['seconds'],
        'object_manifest_sha256':hashlib.sha256(json.dumps(manifest,sort_keys=True).encode()).hexdigest(),
        'compile_receipt_sha256':digest(ROOT/'research/arm64-compile.json'),
        'generator_metadata_sha256':digest(metadata_path),'game_boot_proven':False}
    (ROOT/'research/arm64-object-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
