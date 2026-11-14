"""Copy the already verified phone game into this APK's own data directory.

All original host inputs are read-only. Existing destination files are preserved
and checked, never silently replaced. The phone GfxConfig.ini override is copied
as-is and separately pinned. Hashes cover all 38 destination files after copy.
"""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
from build_android_game import ROOT, PACKAGE


def main():
    config=json.loads((ROOT/'port.json').read_text());proof=json.loads((ROOT/'research/device-input-verification.json').read_text())
    build=json.loads((ROOT/'research/android-game-build.json').read_text());game=Path(config['game_directory']);serial=config['device']
    adb=[str(Path(config['android_sdk'])/'platform-tools/adb.exe'),'-s',serial]
    target='/sdcard/Android/data/'+PACKAGE+'/files/game';source=proof['remote_root'].rstrip('/')
    if proof['serial']!=serial or source!='/sdcard/Frostpunk/game':raise ValueError('Unexpected source receipt/device')
    names=[item['name']for item in build['game_inputs']]
    if len(names)!=38 or set(names)!=set(proof['files']) or any(not re.fullmatch(r'[A-Za-z0-9._-]+',name)for name in names):raise ValueError('Unexpected game input list')
    def run(args,timeout=180):
        result=subprocess.run([*adb,*args],capture_output=True,text=True,timeout=timeout)
        if result.returncode:raise RuntimeError(str(args)+': '+result.stdout+result.stderr)
        return result.stdout.strip()
    def device_hashes(root,selected):
        lines=run(['shell','sha256sum',*[root+'/'+name for name in selected]])
        hashes={line.split()[1].rsplit('/',1)[1]:line.split()[0]for line in lines.splitlines()}
        if set(hashes)!=set(selected)or any(not re.fullmatch('[0-9a-f]{64}',value)for value in hashes.values()):raise ValueError('Incomplete device SHA-256 output')
        return hashes
    begin=time.monotonic();host={}
    print('Hashing all 38 original host inputs read-only',flush=True)
    for name in names:
        path=game/name;before=path.stat();digest=hashlib.sha256()
        with path.open('rb')as stream:
            while block:=stream.read(4*1024*1024):digest.update(block)
        after=path.stat()
        if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):raise ValueError('Host source changed during hash: '+name)
        actual=digest.hexdigest()
        if actual!=proof['files'][name]['sha256']or before.st_size!=proof['files'][name]['bytes']:raise ValueError('Original host input differs from independent receipt: '+name)
        host[name]={'bytes':before.st_size,'sha256':actual,'mtime_ns':before.st_mtime_ns}
    if host['Frostpunk.exe']['sha256']!=config['original_exe_sha256']:raise ValueError('Original EXE pin differs')
    source_config=device_hashes(source,['GfxConfig.ini'])['GfxConfig.ini']
    if source_config!=proof['files']['GfxConfig.ini']['device_sha256']:raise ValueError('Phone GfxConfig.ini override changed; preserve it and review the updated configuration')
    run(['shell','am','force-stop',PACKAGE]);run(['shell','mkdir','-p',target])
    copied=[];preserved=[]
    for name in names:
        destination=target+'/'+name
        exists=subprocess.run([*adb,'shell','test','-e',destination],capture_output=True).returncode
        if not exists:preserved.append(name);continue
        if exists!=1:raise RuntimeError('Could not inspect target input '+name)
        run(['shell','cp',source+'/'+name,destination+'.fpstage'])
        run(['shell','mv','-n',destination+'.fpstage',destination]);copied.append(name)
        if name.endswith('.dat'):print('Copied '+name,flush=True)
    print('Hashing all 38 staged device inputs',flush=True);destination_hashes=device_hashes(target,names)
    rows={}
    for name in names:
        expected=source_config if name=='GfxConfig.ini'else host[name]['sha256'];actual=destination_hashes[name]
        rows[name]={**host[name],'expected_android_sha256':expected,'device_sha256':actual,'matches':actual==expected,'android_override':name=='GfxConfig.ini'}
    report={'timestamp_utc':datetime.now(timezone.utc).isoformat(),'serial':serial,'package':PACKAGE,'source':source,'destination':target,
        'original_exe_sha256':config['original_exe_sha256'],'host_originals_read_only':True,'files':rows,'copied':copied,'preserved_existing':preserved,
        'all_38_destination_hashes_match':all(item['matches']for item in rows.values()),'GfxConfig_ini_override_preserved':True,
        'source_profile_or_saves_modified':False,'game_boot_proven':False,'seconds':round(time.monotonic()-begin,3)}
    path=ROOT/'research/android-game-data.json';path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if not report['all_38_destination_hashes_match']:raise ValueError('Staged input identity mismatch; existing files preserved, see '+str(path))
    print(json.dumps({'all_38_destination_hashes_match':True,'copied_files':len(copied),'preserved_existing':len(preserved),'seconds':report['seconds'],'receipt':str(path)},indent=2))


if __name__=='__main__':main()
