"""Check SDK upscaling, optical flow and UI composition on a selected GPU/device."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/fsr3-android-sdk'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial',required=True)
    parser.add_argument('--driver',choices=('system','turnip'),default='system')
    args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True)
    config=json.loads((ROOT/'port.json').read_text())
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    production=(ROOT/'android/dxvk/fsr3_dxvk.cpp').read_text();start=production.index('bool DxvkFsr3::projection(')
    brace=production.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(production[end]=='{')-(production[end]=='}');end+=1
    (OUT/'fsr3_projection_production.inc').write_text(production[start:end]+'\n')
    projection=OUT/'fsr3-projection-fixture'
    subprocess.run([str(clang),'--target=aarch64-linux-android35','-O2','-std=c++17','-static-libstdc++',
                    '-I'+str(OUT),str(ROOT/'tests/fsr3_projection_fixture.cpp'),'-o',str(projection)],check=True)
    adb=['adb','-s',args.serial];projection_remote='/data/local/tmp/fp-fsr3-projection-test'
    subprocess.run(adb+['push',str(projection),projection_remote],check=True,capture_output=True)
    try:
        subprocess.run(adb+['shell','chmod','700',projection_remote],check=True)
        projection_result=subprocess.run(adb+['shell',projection_remote],capture_output=True,text=True,timeout=30)
        if projection_result.returncode:raise RuntimeError(projection_result.stdout+projection_result.stderr)
        camera=json.loads(projection_result.stdout);assert camera['success']
    finally:subprocess.run(adb+['shell','rm','-f',projection_remote],check=True)
    binary=OUT/'fsr3-vulkan-fixture';library=OUT/'libfrostpunk_fsr3.so'
    shader=OUT/'fsr3-prepare-test.spv'
    subprocess.run([str(ROOT/'build/dxvk-tools/glslang/bin/glslang.exe'),'-V','--target-env','vulkan1.2',
                    '-S','comp',str(ROOT/'android/dxvk/fsr3_prepare.comp'),'-o',str(shader)],check=True)
    driver=OUT/'fixture-driver.o'
    subprocess.run([str(clang.with_name('clang.exe')),'--target=aarch64-linux-android35','-O2','-DFP_GPU_SYSTEM=1',
                    '-c',str(ROOT/'android/gpu/driver.c'),'-o',str(driver)],check=True)
    subprocess.run([str(clang),'--target=aarch64-linux-android35','-O2','-std=c++17','-static-libstdc++',
                    '-I'+str(ROOT/'android/dxvk'),'-I'+str(ROOT/'android/gpu'),
                    str(ROOT/'tests/fsr3_vulkan_fixture.cpp'),str(driver),'-ldl','-o',str(binary)],check=True)
    remote='/data/local/tmp/fp-fsr3-sdk-test'
    subprocess.run(adb+['push',str(binary),remote],check=True,capture_output=True)
    subprocess.run(adb+['push',str(library),remote+'.so'],check=True,capture_output=True)
    subprocess.run(adb+['push',str(shader),remote+'.spv'],check=True,capture_output=True)
    provider=[]
    provider_files=('libadrenotools.so','libhook_impl.so','libmain_hook.so','libfile_redirect_hook.so','libgsl_alloc_hook.so','libc++_shared.so','vulkan.ad07xx.so')
    if args.driver=='turnip':
        runtime=remote+'-driver/'
        subprocess.run(adb+['shell','mkdir','-p',runtime],check=True)
        for name in provider_files:
            subprocess.run(adb+['push',str(ROOT/'build/gpu-runtime/runtime'/name),runtime+name],check=True,capture_output=True)
        provider=[runtime,runtime]
    try:
        subprocess.run(adb+['shell','chmod','700',remote],check=True)
        environment=['env','LD_LIBRARY_PATH='+provider[0]] if provider else []
        result=subprocess.run(adb+['shell',*environment,remote,remote+'.so',remote+'.spv',*provider],capture_output=True,text=True,timeout=60)
        (OUT/'test-stdout.txt').write_text(result.stdout);(OUT/'test-stderr.txt').write_text(result.stderr)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        receipt=json.loads(result.stdout);assert receipt['success']
        receipt.update(driver=args.driver,camera=camera,library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                       shader_sha256=hashlib.sha256(shader.read_bytes()).hexdigest())
        (OUT/'test.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt,indent=2))
    finally:
        subprocess.run(adb+['shell','rm','-f',remote,remote+'.so',remote+'.spv'],check=True)
        if provider:
            subprocess.run(adb+['shell','rm','-f',*[provider[0]+name for name in provider_files]],check=True)
            subprocess.run(adb+['shell','rmdir',provider[0]],check=True)

if __name__=='__main__':main()
