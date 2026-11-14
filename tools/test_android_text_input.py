"""Run the production Android IME adapter and touch editor detection on the JVM; never ADB."""
import json
from pathlib import Path
import shutil
import subprocess

from test_android_touch_controls import extract

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/mobile-20261108/input-photos/host-checks'


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    source=(ROOT/'android/app/TouchControls.java').read_text(encoding='utf-8')
    java=Path(shutil.which('javac')).parent
    def run(args):
        result=subprocess.run(list(map(str,args)),capture_output=True,text=True,encoding='utf-8',timeout=60)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        return result.stdout.strip()
    production=extract(source,'private final class GameInputConnection')
    fixture=(ROOT/'tests/GameInputConnectionCheck.java').read_text(encoding='utf-8')
    activity=(ROOT/'android/app/MainActivity.java').read_text(encoding='utf-8')
    fixture=fixture.replace('/* PRODUCTION_FRAME_RATES */',extract(activity,'private static int[] frameRates(').replace('android.view.Display.Mode','Mode'))
    fixture=fixture.replace('/* PRODUCTION_IME_FRACTION */',extract(source,'private static float imeVisibleFraction('))
    hide=extract(source,'boolean hideKeyboard()').replace('android.view.WindowInsets','Insets').replace('android.view.inputmethod.InputMethodManager','InputManager')
    fixture=fixture.replace('/* PRODUCTION_HIDE_KEYBOARD */',hide)
    show=extract(source,'private void showKeyboard()').replace('android.view.inputmethod.InputMethodManager','InputManager')
    fixture=fixture.replace('/* PRODUCTION_SHOW_KEYBOARD */',show)
    (OUT/'GameInputConnectionCheck.java').write_text(fixture.replace('/* PRODUCTION_INPUT_CONNECTION */',production.replace('android.view.inputmethod.BaseInputConnection','BaseInputConnection')),encoding='utf-8')
    methods=''.join(extract(source,signature) for signature in (
        'private void camera(', 'private void zoomWheel(', 'private void releasePointer()', 'private void click(', 'void pulse(', 'void cancel()',
        'private void timeStart(', 'private void timeMove(', 'private void timeEnd()', 'private final class Gesture'))
    methods=methods.replace('android.os.SystemClock.uptimeMillis()','now')
    gesture=(ROOT/'tests/TouchControlsCheck.java').read_text(encoding='utf-8')
    (OUT/'TouchControls.java').write_text(gesture.replace('/* PRODUCTION_METHODS */',methods),encoding='utf-8')
    run([java/'javac.exe','-encoding','UTF-8','-d',OUT,OUT/'GameInputConnectionCheck.java',OUT/'TouchControls.java'])
    ime=json.loads(run([java/'java.exe','-ea','-cp',OUT,'GameInputConnectionCheck']))
    gestures=json.loads(run([java/'java.exe','-ea','-cp',OUT,'TouchControls']))
    assert ime['success'] and gestures['success']
    (OUT/'window_scans.h').write_text('static const struct {unsigned android_key,vk,scan;} window_key_scans[]={{0,0,0}};\n')
    (OUT/'window_title_returns.h').write_text('static const struct {uint64_t lresult,result;uint32_t error;int native_errno;} window_title_returns[]={{0,0,0,0}};\n')
    wrappers=(ROOT/'tools/build_mobile_controls.py').read_text(encoding='utf-8')
    (OUT/'touch_update_wrappers.inc').write_text(''.join(extract(wrappers,signature) for signature in (
        'void L_000140EC7240(', 'void L_000140E74B30(')),encoding='utf-8')
    cfg=json.loads((ROOT/'port.json').read_text())
    compiler=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    binary=OUT/'android-text-window'
    run([compiler,'--target=aarch64-linux-android30','-O2','-std=c11','-ffunction-sections','-fdata-sections',
         *['-I'+str(ROOT/p) for p in ('native/runtime64','generated/frostpunk-x64','android/host','android/app','tests')],'-I'+str(OUT),
         ROOT/'tests/android_text_window_fixture.c',ROOT/'android/host/win64_window.c',ROOT/'android/host/win64_kernel.c',
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-pthread','-lm','-o',binary])
    report=dict(success=True,ime=ime,gestures=gestures,native_binary=binary.relative_to(ROOT).as_posix(),native_executed=False,device_actions=False)
    (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__=='__main__':main()
