"""Check the production QA environment/profile/Vulkan startup dependency order."""
import hashlib
import json
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
source=root/'android/app/app.c';text=source.read_text()
def function(name):
    start=re.search(r'^static [^\n]*\b'+name+r'\([^;\n]*\) \{',text,re.M)
    assert start,name
    end=text.find('\nstatic ',start.end())
    return text[start.start():end if end>=0 else len(text)]
worker=function('game_worker');prepare=function('prepare_game_runtime');vulkan=function('open_vulkan')
assert worker.count('apply_debug_environment();')==1
assert text.count('    apply_debug_environment();')==1
assert worker.index('apply_debug_environment();')<worker.index('prepare_game_runtime(')<worker.index('open_vulkan(')<worker.index('dlopen(')
assert prepare.index('"prepareNativeProfile"')<prepare.index('"prepareNativeRuntime"')
assert 'apply_debug_environment();' not in vulkan
assert 'c->profile' in function('turnip_requested') and 'c->private_path' not in function('turnip_requested')
result=dict(success=True,production_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
            debug_environment_before_profile=True,profile_before_vulkan=True,vulkan_before_runtime_load=True,
            single_debug_environment_application=True,driver_preferences_use_active_profile=True,
            scope='source dependency order; physical environment/profile selection requires device acceptance')
out=root/'build/audit-20261108/app-startup-order.json';out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
