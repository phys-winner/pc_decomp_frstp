"""Verify APK metadata omits host/device details while retaining every input identity."""
import hashlib
import json
from pathlib import Path
import re
from public_metadata import game_contract,runtime_contract,fsr3_contract

root=Path(__file__).resolve().parents[1]
paths=[root/'research/game-host-data-port-settings.json',root/'research/runtime-library.json',root/'build/fsr3-android-sdk/build.json']
before={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
game,runtime,fsr3=[json.loads(p.read_text()) for p in paths]
public_game,public_runtime=game_contract(game),runtime_contract(runtime)
public_fsr3=fsr3_contract(fsr3);assert public_fsr3['sha256']==fsr3['sha256'] and public_fsr3['archive_sha256']==fsr3['archive_sha256']
for old,new in [(game,public_game),(runtime,public_runtime)]:assert old['original_exe_sha256']==new['original_exe_sha256']
assert set(game['files'])==set(public_game['files'])
for name,row in game['files'].items():
    assert (row['bytes'],row['sha256'])==(public_game['files'][name]['bytes'],public_game['files'][name]['sha256'])
for name,row in runtime['runtime_dependencies'].items():assert row['sha256']==public_runtime['runtime_dependencies'][name]['sha256']
private_ids={str(v) for v in (game.get('intended_serial'),runtime.get('serial')) if v}
def check(value):
    if isinstance(value,dict):
        for key,item in value.items():
            assert not any(term in key.lower() for term in ('serial','path','game_root','stage_destination')),key
            check(item)
    elif isinstance(value,list):
        for item in value:check(item)
    elif isinstance(value,str):
        assert not re.match(r'[A-Za-z]:[\\/]|/(?:storage|data|home|Users)/',value)
        assert not any(identifier in value for identifier in private_ids)
for value in (public_game,public_runtime,public_fsr3):check(value)
assert game_contract(public_game)==public_game
override=runtime_contract(runtime,binary_sha256='f'*64,binary_bytes=1234)
assert override['binary_sha256']=='f'*64 and override['binary_bytes']==1234
for path,digest in before.items():assert hashlib.sha256(path.read_bytes()).hexdigest()==digest
result=dict(success=True,no_serial_or_private_paths=True,all_file_and_executable_identities_unchanged=True,
            private_receipts_unchanged=True,actual_runtime_identity_override=True,
            game_files=len(public_game['files']),runtime_dependencies=len(public_runtime['runtime_dependencies']))
out=root/'build/audit-20261108/public-metadata-check.json';out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
