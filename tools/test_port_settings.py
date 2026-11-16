"""Check settings defaults and persistence on an explicitly selected device."""
import argparse
import json,subprocess
from fastbuild import CLANG,ROOT
from test_android_render_scale import function
from test_android_touch_controls import extract
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial')
parser.add_argument('--compile-only',action='store_true')
args=parser.parse_args()
if not args.serial and not args.compile_only:parser.error('--serial is required unless --compile-only is used')
out=ROOT/'build/mobile-20261103/port-settings-test'
out.parent.mkdir(parents=True,exist_ok=True)
source=ROOT/'build/fast/mobile-objects/recomp_funcs_0807.c'
(out.parent/'render_defaults_production.inc').write_text(function(source.read_text(),'140E5C1A0'))
options_source=ROOT/'build/fast/port-objects/recomp_funcs_0959.c'
(out.parent/'frame_options_production.inc').write_text(extract(options_source.read_text(),'static void fp_frame_options(')+extract(options_source.read_text(),'static int fp_label_matches(')+extract(options_source.read_text(),'static void fp_output_labels('))
subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-std=c11','-Wl,-z,max-page-size=16384','-I'+str(ROOT/'android/host'),
 '-I'+str(ROOT/'native/runtime64'),'-I'+str(out.parent),
 str(ROOT/'tests/port_settings_fixture.c'),str(ROOT/'android/host/port_settings.c'),'-o',str(out)],check=True)
if args.compile_only:
 print(json.dumps(dict(success=True,binary=out.relative_to(ROOT).as_posix(),executed=False)))
 raise SystemExit(0)
adb=['adb','-s',args.serial];remote='/data/local/tmp/fp-port-settings-test'
subprocess.run(adb+['push',str(out),remote],check=True,capture_output=True)
try:
 subprocess.run(adb+['shell','chmod','700',remote],check=True)
 result=json.loads(subprocess.check_output(adb+['shell',remote]));assert result['success']
 out.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
finally:subprocess.run(adb+['shell','rm','-f',remote],check=True)
