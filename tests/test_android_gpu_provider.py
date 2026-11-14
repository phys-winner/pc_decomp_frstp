"""Reject stale custom drivers and mismatched Vulkan providers in sealed APKs."""
import hashlib
import io
from pathlib import Path
import sys
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_android_game_package import verify_provider_assets

def archive(files):
 stream=io.BytesIO()
 with zipfile.ZipFile(stream,'w')as z:
  for name,data in files.items():z.writestr(name,data)
 stream.seek(0)
 return zipfile.ZipFile(stream)

def rejected(files,runtime,build):
 with archive(files)as z:
  try:verify_provider_assets(z,runtime,build)
  except (AssertionError,KeyError):return
 raise AssertionError('Unexpected provider assets were accepted')

system={'native_d3d11_bridge':{'gpu_provider':'system'},'runtime_dependencies':{}}
with archive({})as z:assert verify_provider_assets(z,system,{'gpu_provider':'system'})=='system'
rejected({'assets/turnip/vulkan.ad07xx.so':b'stale'},system,{'gpu_provider':'system'})
rejected({},dict(system,runtime_dependencies={'libadrenotools.so':{}}),{'gpu_provider':'system'})
rejected({},system,{'gpu_provider':'turnip'})
rejected({},system,{'gpu_provider':'automatic'})
driver=b'pinned-custom-driver'
turnip={'native_d3d11_bridge':{},'runtime_dependencies':{'vulkan.ad07xx.so':{'sha256':hashlib.sha256(driver).hexdigest()}}}
with archive({'assets/turnip/vulkan.ad07xx.so':driver})as z:assert verify_provider_assets(z,turnip,{})=='turnip'
rejected({'assets/turnip/vulkan.ad07xx.so':b'changed'},turnip,{})
print('GPU_PROVIDER_ASSETS tests=7 PASS')

# Execute the production environment block, including its setenv failure boundary.
import json
import subprocess
from leaf_probe import windows_toolchain

root=Path(__file__).resolve().parents[1]
runtime=(root/'android/host/android_runtime.c').read_text()
start=runtime.index('    const char *active_driver=getenv(')
end=runtime.index('    /* The guest desktop',start)
policy=runtime[start:end]
policy=policy.replace('getenv(', 'test_getenv(').replace('setenv(', 'test_setenv(')
fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define PATH_MAX 4096
#define PROP_VALUE_MAX 92
static const char *active,*gpu;
static unsigned state_calls,path_calls,released,fail_state;
static void release_window(void *window){assert(window==(void*)1);++released;}
static const char *test_getenv(const char *key){assert(!strcmp(key,"FROSTPUNK_VULKAN_DRIVER"));return active;}
static int __system_property_get(const char *key,char *value){assert(!strcmp(key,"ro.hardware.vulkan"));strcpy(value,gpu);return (int)strlen(gpu);}
static int test_setenv(const char *key,const char *value,int overwrite){
    assert(overwrite==1);
    if(!strcmp(key,"DXVK_STATE_CACHE")){assert(!strcmp(value,"0"));++state_calls;return fail_state?-1:0;}
    if(!strcmp(key,"DXVK_STATE_CACHE_PATH")){assert(!strcmp(value,"profile"));++path_calls;}
    return 0;
}
static int run(void){
    struct Host {const char *game_path,*profile_path,*native_library_path,*gpu_driver_path;void (*release_window)(void*);};
    const struct Host value={"game","profile","libraries","driver",release_window},*host=&value;
    void *window=(void*)1;
POLICY
    return 0;
}
int main(void){
    const char *drivers[]={"system","turnip","system","system",NULL,"system"};
    const char *hardware[]={"mali","mali","adreno","", "mali","mali"};
    for(unsigned i=0;i<6;++i){
        active=drivers[i];gpu=hardware[i];fail_state=i==5;state_calls=path_calls=released=0;
        int result=run();assert(result==(fail_state?71:0));assert(released==fail_state);
        assert(path_calls==1&&state_calls==(i==0||i==5));
    }
    puts("MALI_WARM_STATE_CACHE policy_cases=6 native_cache_path_preserved=1 failure_releases_window=1 PASS");
}
'''.replace('POLICY',policy)
out=root/'build/android-gpu-provider-policy-test';out.mkdir(parents=True,exist_ok=True)
source=out/'runtime-cache-policy.c';source.write_text(fixture,encoding='utf-8')
bins,env=windows_toolchain();binary=out/'runtime-cache-policy.exe'
subprocess.run([str(bins/'cl.exe'),'/nologo','/O2','/std:c11',str(source),'/Fo'+str(out/'runtime-cache-policy.obj'),'/Fe'+str(binary)],check=True,env=env,capture_output=True,text=True)
checked=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
# The native Vulkan cache uses the retained path independently of the state-cache switch.
cache=(root/'tools/dxvk_pipeline_cache/dxvk_pipeline_cache.cpp').read_text()
overlay=(root/'tools/dxvk_pipeline_cache_overlay.py').read_text()
assert 'DXVK_STATE_CACHE' not in cache and 'env::getEnvVar("DXVK_STATE_CACHE_PATH"), true)' in overlay
(out/'result.json').write_text(json.dumps(dict(success=True,cases=6,production_block_sha256=hashlib.sha256(runtime[start:end].encode()).hexdigest(),stdout=checked.stdout,native_cache_independent=True),indent=2)+'\n')
print(checked.stdout.strip())
