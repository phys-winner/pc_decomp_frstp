"""Execute the production mobile FX policy with controlled Android properties/settings."""
import hashlib
import json
from pathlib import Path
import subprocess

from leaf_probe import ROOT, windows_toolchain
from dxvk_mobile_overlay import CONTEXT_CODE


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    out = ROOT / 'build/mobile-20261108/input-photos/ui-depth-policy-check'
    out.mkdir(parents=True, exist_ok=True)
    policy = CONTEXT_CODE[:CONTEXT_CODE.index('  static void fpFrame(')]
    source = out / 'mobile-policy.cpp'
    source.write_text(r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#define __ANDROID__ 1
#define PROP_VALUE_MAX 92
static bool effects = true, highlight = true;
static const char *override_value;
static int frostpunk_dxvk_mobile_effects(){return effects;}
static int frostpunk_dxvk_object_highlight(){return highlight;}
static int __system_property_get(const char *name,char *value){
  if(std::strcmp(name,"debug.fp.fx")||!override_value){value[0]=0;return 0;}
  std::strcpy(value,override_value);return int(std::strlen(value));
}
''' + policy + r'''
#endif
static unsigned checks;
static void check(unsigned expected){g_fpPolls=0;assert(fpPolicy()==expected);++checks;}
int main(){
  check(30); // Mobile effects retain low-resolution tonemapping, without transient stencil.
  effects=false;check(14);
  highlight=false;check(15);
  effects=true;check(31);
  highlight=true;
  const char *values[]={"0","14","30","46","62"};
  for(const char *value: values){
    override_value=value;check(unsigned(std::strtoul(value,nullptr,0)));
  }
  override_value=nullptr;check(30);
  // Properties are polled every thirty policy calls; normal settings are read each frame.
  override_value="46";g_fpPolls=1;assert(fpPolicy()==30);++checks;
  g_fpPolls=30;assert(fpPolicy()==46);++checks;
  override_value=nullptr;check(30);
  std::printf("{\"success\":true,\"checks\":%u,\"default_mobile_mask\":30,"
              "\"default_transient_stencil_disabled\":true,\"lowres_tonemap_retained\":true,"
              "\"settings_and_highlight_preserved\":true,\"diagnostic_override_preserved\":true}\n",checks);
}
''')
    # The fixture uses the policy verbatim; only Android property/settings providers are mocked.
    bins, env = windows_toolchain()
    exe = out / 'mobile-policy.exe'
    run = subprocess.run([str(bins / 'cl.exe'), '/nologo', '/std:c++17', '/EHsc', '/O2',
                          str(source), '/Fo' + str(out / 'mobile-policy.obj'), '/Fe' + str(exe)],
                         env=env, capture_output=True, text=True)
    if run.returncode:
        raise RuntimeError(run.stdout + run.stderr)
    run = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    result = json.loads(run.stdout)
    result.update(source_sha256=sha(Path(__file__)), overlay_sha256=sha(ROOT / 'tools/dxvk_mobile_overlay.py'),
                  compiled_fixture_sha256=sha(source), binary_sha256=sha(exe), phone_executed=False)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
