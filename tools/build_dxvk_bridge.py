"""Generate exact COM slot metadata and compile the native Win64 D3D11 bridge."""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import shutil
import sys
import uuid

import build_dxvk as build

ROOT = build.ROOT
OUT = ROOT / "build/dxvk-bridge"
INTERFACES = [
    "IUnknown", "ID3D11Device", "ID3D11DeviceContext", "ID3D11DeviceChild", "ID3D11Resource",
    "ID3D11Buffer", "ID3D11Texture1D", "ID3D11Texture2D", "ID3D11Texture3D", "ID3D11View",
    "ID3D11ShaderResourceView", "ID3D11UnorderedAccessView", "ID3D11RenderTargetView", "ID3D11DepthStencilView",
    "ID3D11InputLayout", "ID3D11VertexShader", "ID3D11PixelShader", "ID3D11GeometryShader",
    "ID3D11HullShader", "ID3D11DomainShader", "ID3D11ComputeShader", "ID3D11BlendState",
    "ID3D11DepthStencilState", "ID3D11RasterizerState", "ID3D11SamplerState", "ID3D11Asynchronous",
    "ID3D11Query", "ID3D11Predicate", "ID3D11Counter", "ID3D11CommandList", "ID3D11ClassLinkage", "ID3D11ClassInstance",
    "IDXGIObject", "IDXGIDeviceSubObject", "IDXGIDevice", "IDXGIAdapter", "IDXGIAdapter1", "IDXGIFactory", "IDXGIFactory1",
    "IDXGISwapChain", "IDXGIOutput", "ID3D11Device1", "ID3D11Device2", "ID3D11Device3", "ID3D11Device4", "ID3D11Device5",
    "ID3D11DeviceContext1", "ID3D11DeviceContext2", "ID3D11DeviceContext3", "ID3D11DeviceContext4",
    # Append to preserve the existing sentinel numbering. The original game
    # queries Device2 immediately after D3D11CreateDevice, then calls inherited
    # Device1 slot12 SetMaximumFrameLatency(1).
    "IDXGIDevice1", "IDXGIDevice2",
    # Preserve kinds0..51. The original post-SDL screen constructor requests
    # Factory2 and its real CreateSwapChainForHwnd result is SwapChain1.
    "IDXGIFactory2", "IDXGISwapChain1",
]

# These are semantic bindings, not guessed argument counts. Numbers are the
# zero-based Win64 argument positions, including COM this at position0.
COUNTS = {
    "CreateInputLayout": {1: "uint32_t(arg(c,2))", 3: "arg(c,4)"},
    "CreateGeometryShaderWithStreamOutput": {1: "arg(c,2)", 3: "uint32_t(arg(c,4))", 5: "uint32_t(arg(c,6))"},
    "CreateVertexShader": {1: "arg(c,2)"}, "CreatePixelShader": {1: "arg(c,2)"},
    "CreateGeometryShader": {1: "arg(c,2)"}, "CreateHullShader": {1: "arg(c,2)"},
    "CreateDomainShader": {1: "arg(c,2)"}, "CreateComputeShader": {1: "arg(c,2)"},
    "CheckFeatureSupport": {2: "uint32_t(arg(c,3))"}, "GetData": {2: "uint32_t(arg(c,3))"},
    "IASetVertexBuffers": {3: "uint32_t(arg(c,2))", 4: "uint32_t(arg(c,2))", 5: "uint32_t(arg(c,2))"},
    "IAGetVertexBuffers": {3: "uint32_t(arg(c,2))", 4: "uint32_t(arg(c,2))", 5: "uint32_t(arg(c,2))"},
    "OMSetRenderTargets": {2: "uint32_t(arg(c,1))"}, "OMGetRenderTargets": {2: "uint32_t(arg(c,1))"},
    "OMSetRenderTargetsAndUnorderedAccessViews": {2: "uint32_t(arg(c,1))==0xffffffff ? 0 : uint32_t(arg(c,1))", 6: "uint32_t(arg(c,5))==0xffffffff ? 0 : uint32_t(arg(c,5))", 7: "uint32_t(arg(c,5))==0xffffffff ? 0 : uint32_t(arg(c,5))"},
    "OMGetRenderTargetsAndUnorderedAccessViews": {2: "uint32_t(arg(c,1))", 6: "uint32_t(arg(c,5))"},
    "CSSetUnorderedAccessViews": {3: "uint32_t(arg(c,2))", 4: "uint32_t(arg(c,2))"},
    "CSGetUnorderedAccessViews": {3: "uint32_t(arg(c,2))"},
    "OMSetBlendState": {2: "4"}, "OMGetBlendState": {2: "4"},
    "SOSetTargets": {2: "uint32_t(arg(c,1))", 3: "uint32_t(arg(c,1))"}, "SOGetTargets": {2: "uint32_t(arg(c,1))"},
    "RSSetViewports": {2: "uint32_t(arg(c,1))"}, "RSSetScissorRects": {2: "uint32_t(arg(c,1))"},
    "RSGetViewports": {2: "ref<UINT>(arg(c,1))"}, "RSGetScissorRects": {2: "ref<UINT>(arg(c,1))"},
    "ClearRenderTargetView": {2: "4"}, "ClearUnorderedAccessViewUint": {2: "4"}, "ClearUnorderedAccessViewFloat": {2: "4"},
    "QueryResourceResidency": {1: "uint32_t(arg(c,3))", 2: "uint32_t(arg(c,3))"},
    "GetInstanceName": {1: "ref<SIZE_T>(arg(c,2))"}, "GetTypeName": {1: "ref<SIZE_T>(arg(c,2))"},
}
for stage in ["VS", "PS", "GS", "HS", "DS", "CS"]:
    for action in ["Set", "Get"]:
        for what in ["ConstantBuffers", "ShaderResources", "Samplers"]:
            COUNTS[stage + action + what] = {3: "uint32_t(arg(c,2))"}
    COUNTS[stage + "SetShader"] = {2: "uint32_t(arg(c,3))"}
    COUNTS[stage + "GetShader"] = {2: "arg(c,3) ? ref<UINT>(arg(c,3)) : 0"}

BLOCKED = {"SetResourceMinLOD", "GetResourceMinLOD", "OpenSharedResource", "CreateCounter", "CheckCounter", "CheckCounterInfo",
           "CreateSurface", "FindClosestMatchingMode", "TakeOwnership", "ReleaseOwnership",
           "GetGammaControlCapabilities", "SetGammaControl", "GetGammaControl", "SetDisplaySurface",
           "GetDisplaySurfaceData", "WaitForVBlank", "EnumOutputs", "GetDisplayModeList",
           "OfferResources", "ReclaimResources", "EnqueueSetEvent"}


def dispatch_metadata(result):
    cases = []
    coverage = {}
    for interface, methods in result.items():
        cases.append(f"case Kind::{interface}: switch (slot) {{")
        bound = []
        for slot, name in enumerate(methods):
            if slot < 3 or name in BLOCKED:
                continue
            # New DXGI1.2 slots need individual guest/native pointer contracts;
            # CoreWindow/HANDLE/Present1 nested pointers are not generic ABI.
            if interface == "IDXGIFactory2" and slot >= 14 and name != "CreateSwapChainForHwnd":
                continue
            if interface == "IDXGISwapChain1" and slot >= 18:
                continue
            # Extended device/context slots are intentionally unrepresented,
            # rather than exposing guessed contracts for 11.1+ operations.
            if (interface.startswith("ID3D11DeviceContext") and slot >= 115) or (interface.startswith("ID3D11Device") and interface != "ID3D11DeviceChild" and not interface.startswith("ID3D11DeviceContext") and slot >= 43):
                continue
            statement = None
            if name in ["GetPrivateData", "SetPrivateData", "SetPrivateDataInterface"]:
                op = ["GetPrivateData", "SetPrivateData", "SetPrivateDataInterface"].index(name)
                statement = f"private_data<{interface}>(c,{op});"
            elif name == "Map": statement = "map_resource(c);"
            elif name == "Unmap": statement = "unmap_resource(c);"
            elif name == "UpdateSubresource": statement = "update_resource(c);"
            elif name in ["CreateBuffer", "CreateTexture1D", "CreateTexture2D", "CreateTexture3D", "CreateInputLayout", "CreateGeometryShaderWithStreamOutput"]:
                statement = f"create_resource(c,{slot});"
            elif name == "GetDesc" and interface in ["IDXGIAdapter", "IDXGIAdapter1", "IDXGIOutput", "IDXGISwapChain", "IDXGISwapChain1"]:
                statement = f"dxgi_description(c,Kind::{interface});"
            elif name == "GetDesc1" and interface == "IDXGIAdapter1": statement = "dxgi_description(c,Kind::IDXGIAdapter1,true);"
            elif name == "GetParent": statement = "dxgi_parent(c);"
            elif name == "GetDevice" and interface in ["IDXGIDeviceSubObject", "IDXGISwapChain", "IDXGISwapChain1"]: statement = "dxgi_get_device(c);"
            elif name == "GetBuffer" and interface in ["IDXGISwapChain", "IDXGISwapChain1"]: statement = "dxgi_get_buffer(c);"
            elif name == "CreateSwapChain": statement = "dxgi_create_swapchain(c);"
            elif name == "CreateSwapChainForHwnd": statement = "dxgi_create_swapchain_for_hwnd(c);"
            elif name == "MakeWindowAssociation": statement = "dxgi_window_association(c);"
            elif name == "GetWindowAssociation": statement = "dxgi_get_window_association(c);"
            elif name == "CreateSoftwareAdapter": continue
            if statement is None:
                count = COUNTS.get(name, {})
                expression = ",counts({" + ",".join("{" + str(k) + "," + v + "}" for k,v in count.items()) + "})" if count else ""
                statement = f"invoke(c,&{interface}::{name}{expression});"
            cases.append(f"case {slot}: {statement} return 1;")
            bound.append(name)
        cases.append("default: break; } break;")
        coverage[interface] = {"bound": ["QueryInterface", "AddRef", "Release", *bound],
                               "unsupported": [n for n in methods if n not in bound and n not in ["QueryInterface", "AddRef", "Release"]]}
    build.write_changed(OUT / "bridge_dispatch.inc", "\n".join(cases) + "\n")
    build.write_changed(OUT / "bridge_coverage.json", json.dumps(coverage,indent=2)+"\n")


def metadata():
    directory = ROOT / "reference/dxvk/include/native"
    source = "\n".join(p.read_text(encoding="utf-8") for p in
        [directory / "windows/unknwn.h", directory / "directx/d3d11.h", directory / "directx/d3d11_1.h",
         directory / "directx/d3d11_2.h", directory / "directx/d3d11_3.h", directory / "directx/d3d11_4.h",
         directory / "directx/dxgi.h", directory / "directx/dxgi1_2.h"])
    result = {}
    for interface in INTERFACES:
        found = re.search(r"typedef struct " + interface + r"Vtbl\s*\{(.*?)\}\s*" + interface + r"Vtbl;", source, re.S)
        if interface == "IUnknown" and not found:
            result[interface] = ["QueryInterface", "AddRef", "Release"]
            continue
        if not found:
            raise ValueError("Missing actual interface vtable " + interface)
        names = re.findall(r"\(STDMETHODCALLTYPE\s*\*(\w+)\)", found.group(1))
        if names[:3] != ["QueryInterface", "AddRef", "Release"] or len(names) > 256:
            raise ValueError("Unexpected vtable layout " + interface)
        result[interface] = names
    lines = ["// Generated from pinned DXVK Native DirectX headers; do not hand-edit.",
             "enum class Kind : unsigned {" + ",".join(INTERFACES) + ",Count};",
             "template<class T> struct KindOf;", "static Kind kind_for_iid(REFIID iid) {"]
    lines += [f"if (iid == __uuidof({i})) return Kind::{i};" for i in INTERFACES]
    lines += ["return Kind::Count; }", "static const char* kind_names[] = {" + ",".join(json.dumps(i) for i in INTERFACES) + "};"]
    for i in INTERFACES:
        lines += [f"template<> struct KindOf<{i}> {{ static constexpr Kind value = Kind::{i}; }};",
                  f"static const char* names_{i}[] = {{" + ",".join(json.dumps(n) for n in result[i]) + "};"]
    lines += ["static const char** slot_names[] = {" + ",".join("names_"+i for i in INTERFACES) + "};",
              "static const unsigned slot_counts[] = {" + ",".join(str(len(result[i])) for i in INTERFACES) + "};"]
    build.write_changed(OUT / "bridge_metadata.h", "\n".join(lines) + "\n")
    build.write_changed(OUT / "bridge_slots.json", json.dumps(result, indent=2) + "\n")
    dispatch_metadata(result)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--probe",action="store_true",help="Also build the synthetic Win64 ABI GPU fixture")
    ap.add_argument("--run-only",action="store_true",help="Verify and run the existing fixture on the configured device")
    arguments=ap.parse_args()
    if arguments.run_only:
        run_probe()
        return
    OUT.mkdir(parents=True, exist_ok=True)
    metadata()
    cfg = json.loads((ROOT / "port.json").read_text())
    paths=[ROOT/"android/dxvk/bridge.cpp",ROOT/"android/dxvk/bridge.h",ROOT/"android/dxvk/buffer_map_count.h",Path(__file__),
           ROOT/"android/host/win64.h",ROOT/"android/host/texture_formats.h",ROOT/"native/runtime64/cpu64.h",ROOT/"port.json",
           OUT/"bridge_metadata.h",OUT/"bridge_dispatch.inc",OUT/"bridge_slots.json",
           ROOT/"reference/dxvk/src/d3d11/d3d11_device.cpp",ROOT/"reference/goldberg/stb/stb_image_write.h",
           ROOT/"android/host/win64_kernel.h"]
    paths+=sorted((ROOT/"reference/dxvk/include/native").rglob("*.h"))
    if arguments.probe:paths+=[ROOT/"tests/nativeD3D11_bridge.cpp",ROOT/"tests/buffer_map_bridge.inc",ROOT/"android/gpu/driver.c",ROOT/"android/gpu/driver.h"]
    source_pins={str(p.relative_to(ROOT)).replace("\\","/"):build.sha(p)for p in paths}
    ndk = Path(cfg["android_sdk"]) / "ndk" / cfg["android_ndk_version"]
    llvm = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    includes = [ROOT / "reference/dxvk/include/native/windows", ROOT / "reference/dxvk/include/native/directx",
                ROOT / "reference/dxvk/include/vulkan/include", ROOT / "native/runtime64", ROOT / "android/host", OUT]
    args = [llvm / "clang++.exe", "--target=aarch64-linux-android30", "-std=c++17", "-O2", "-fPIC", "-Wall", "-Wextra",
            "-Wno-extern-c-compat", "-Wno-missing-field-initializers"]
    for p in includes:
        args += ["-I", p]
    build.run([*args, "-c", ROOT / "android/dxvk/bridge.cpp", "-o", OUT / "bridge.o"])
    build.run([llvm / "llvm-ar.exe", "rcs", OUT / "libwin64_d3d11.a", OUT / "bridge.o"])
    receipt={"revision":build.REVISION,"target":"aarch64-linux-android30","ndk_version":cfg["android_ndk_version"],
             "compiler_sha256":build.sha(llvm/"clang++.exe"),"source_sha256":build.sha(ROOT/"android/dxvk/bridge.cpp"),
             "header_sha256":build.sha(ROOT/"android/dxvk/bridge.h"),"generator_sha256":build.sha(Path(__file__)),
             "cpu_header_sha256":build.sha(ROOT/"native/runtime64/cpu64.h"),
             "win64_header_sha256":build.sha(ROOT/"android/host/win64.h"),
             "texture_formats_header_sha256":build.sha(ROOT/"android/host/texture_formats.h"),
             "slots_sha256":build.sha(OUT/"bridge_slots.json"),"archive_sha256":build.sha(OUT/"libwin64_d3d11.a"),
             "synthetic_fixture":False,"phone_executed":False,"game_rendered":False}
    if arguments.probe:
        import build_dxvk_probe
        build.run([sys.executable, ROOT/"tools/build_dxvk_probe.py"])
        probe = build_dxvk_probe.OUT
        system = build_dxvk_probe.SYSTEM
        extra=["-I",ROOT/"android/dxvk","-I",ROOT/"android/gpu","-I",ROOT/"reference/adrenotools/include",
               "-I",probe]
        build.run([llvm/"clang.exe","--target=aarch64-linux-android30","-std=c11","-O2","-fPIC",
                   *(["-DFP_GPU_SYSTEM=1"] if system else []),
                   "-I",ROOT/"android/gpu","-I",ROOT/"reference/adrenotools/include",
                   "-c",ROOT/"android/gpu/driver.c","-o",OUT/"driver.o"])
        build.run([*args,*extra,ROOT/"tests/nativeD3D11_bridge.cpp",OUT/"bridge.o",OUT/"driver.o",
                   *([] if system else ["-L",ROOT/"build/gpu-runtime/runtime","-ladrenotools"]),"-ldl","-landroid","-Wl,--export-dynamic",
                   "-Wl,-z,max-page-size=16384","-o",OUT/"nativeD3D11_bridge"])
        for name in ["libdxvk_d3d11.so","libdxvk_dxgi.so","libc++_shared.so"]:
            shutil.copyfile(probe/name,OUT/name)
        receipt.update({"synthetic_fixture":True,"fixture_source_sha256":build.sha(ROOT/"tests/nativeD3D11_bridge.cpp"),
                        "fixture_binary_sha256":build.sha(OUT/"nativeD3D11_bridge"),
                        "provider_source_sha256":build.sha(ROOT/"android/gpu/driver.c"),
                        "gpu_provider":"system" if system else "turnip",
                        "shader_sha256":{name:build.sha(probe/name)for name in ["native_vs.dxbc","native_ps.dxbc"]},
                        "libraries":{name:build.sha(OUT/name)for name in ["libdxvk_d3d11.so","libdxvk_dxgi.so","libc++_shared.so"]}})
    for name,digest in source_pins.items():
        if build.sha(ROOT/name)!=digest:raise RuntimeError("Bridge input changed during build: "+name)
    receipt["source_inputs_pre_post_sha256"]=source_pins
    receipt["source_inputs_pre_post_equal"]=True
    build.write_changed(OUT/"build-receipt.json",json.dumps(receipt,indent=2)+"\n")
    print("READY", OUT / "libwin64_d3d11.a")


def run_probe():
    cfg=json.loads((ROOT/"port.json").read_text())
    adb=str(Path(cfg["android_sdk"])/"platform-tools/adb.exe"); serial=cfg["device"]
    base="/data/local/tmp/frostpunk/dxvk-bridge"; gpu="/data/local/tmp/frostpunk/gpu-runtime"
    receipt=json.loads((OUT/"build-receipt.json").read_text())
    if build.sha(OUT/"nativeD3D11_bridge")!=receipt["fixture_binary_sha256"]:
        raise RuntimeError("Fixture binary/build receipt mismatch")
    for name,digest in receipt["source_inputs_pre_post_sha256"].items():
        if build.sha(ROOT/name)!=digest:raise RuntimeError("Bridge source differs from built input: "+name)
    def call(args,timeout=30):
        return subprocess.run([adb,"-s",serial,*args],capture_output=True,text=True,timeout=timeout)
    call(["shell","mkdir","-p",base]).check_returncode()
    files=["nativeD3D11_bridge","libdxvk_d3d11.so","libdxvk_dxgi.so","libc++_shared.so"]
    for name in files: call(["push",str(OUT/name),base+"/"+name]).check_returncode()
    call(["shell","chmod","755",base+"/nativeD3D11_bridge"]).check_returncode()
    system=cfg.get("gpu_provider")=="system"
    result=call(["shell","env","LD_LIBRARY_PATH="+base+("" if system else ":"+gpu),base+"/nativeD3D11_bridge",gpu,gpu+"/"],90)
    stamp=datetime.datetime.now(datetime.timezone.utc)
    run_dir=OUT/"runs"/(stamp.strftime("%Y%m%dT%H%M%SZ")+"-"+uuid.uuid4().hex[:8]); run_dir.mkdir(parents=True)
    (run_dir/"stdout.txt").write_text(result.stdout,encoding="utf-8"); (run_dir/"stderr.txt").write_text(result.stderr,encoding="utf-8")
    report={"recorded_at":stamp.isoformat(),"serial":serial,"build":receipt,
            "coverage":json.loads((OUT/"bridge_coverage.json").read_text()),
            "dxvk_build":json.loads((ROOT/"build/dxvk-arm64/build-receipt.json").read_text()),
            "provider":{"gpu_provider":"system"} if system else json.loads((ROOT/"research/gpu-runtime.json").read_text()),
            "returncode":result.returncode,"stdout":result.stdout,"stderr":result.stderr,
            "phone_executed":True,"game_rendered":False,"fps_measured":False}
    try: report["result"]=json.loads(result.stdout)
    except ValueError: report["result"]=None
    paths=[(base+"/"+n,OUT/n)for n in files]
    if not system:
        paths +=[(gpu+"/"+n,ROOT/"build/gpu-runtime/runtime"/n)for n in ["libadrenotools.so","libhook_impl.so","libmain_hook.so","libfile_redirect_hook.so","libgsl_alloc_hook.so","vulkan.ad07xx.so"]]
    check=call(["shell","sha256sum",*[p for p,_ in paths]]); check.check_returncode()
    hashes={line.split()[1]:line.split()[0]for line in check.stdout.splitlines()}
    checks=[{"device_path":remote,"sha256":build.sha(local),"device_sha256":hashes.get(remote),"match":build.sha(local)==hashes.get(remote)}for remote,local in paths]
    report["deployed_sha256_verified"]=checks
    data=report["result"]or{}
    report["checks_passed"]=(result.returncode==0 and data.get("success") is True and data.get("matched_pixels")==16
        and all(data.get(k)is True for k in ["canonical_iunknown","utf16_desc_canary","xmm_float_abi","staging_map_unmap","concurrent_device_passed","native_lifetime_retention_passed","dxgi_device2_latency_passed","dxgi_device12_identity","latency_uint32_canary","factory2_inherited_identity","factory2_null_contract","full_CPU_except_rax_preserved"])
        and all(c["match"]for c in checks))
    text=json.dumps(report,indent=2)+"\n"; build.write_changed(run_dir/"receipt.json",text)
    build.write_changed(ROOT/"research/dxvk-win64-bridge.json",text)
    print(result.stdout); print("stderr tail:",result.stderr[-1500:])
    if not report["checks_passed"]: raise RuntimeError("Win64 D3D11 bridge fixture failed; preserved at "+str(run_dir))


if __name__ == "__main__":
    main()
