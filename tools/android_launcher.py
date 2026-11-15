"""Shared launcher preparation for the canonical and incremental APK builders."""
import importlib.util
import os
from pathlib import Path
import hashlib
import json
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = 'com.frostpunk.staticrecomp'


def game_input_manifest():
    originals = json.loads((ROOT / 'android/game-data-originals.json').read_text())
    legacy = json.loads((ROOT / 'research/game-host-data-port-settings.json').read_text())['files']
    sizes = {name: {row['bytes'], legacy[name]['bytes']} for name, row in originals.items()}
    for kind in ('port-ui', 'switch-ui'):
        for archive in json.loads((ROOT / ('android/game-data-' + kind + '.json')).read_text())['archives']:
            sizes[archive['stem'] + '.dat'].add(archive['output']['.dat']['bytes'])
    # The validated texture converter retains the entry count but changes the data length.
    return ''.join(('*' if name == 'textures-s3.dat' else ','.join(map(str, sorted(values)))) + '\t' + name + '\n'
                   for name, values in sorted(sizes.items()))


def game_data_contract():
    originals = json.loads((ROOT / 'android/game-data-originals.json').read_text())
    return dict(schema_version=2, kind='frostpunk-game-data', complete=True, package=PACKAGE,
                original_exe_sha256=originals['Frostpunk.exe']['sha256'],
                files={name: dict(row, kind='original') for name, row in originals.items()},
                optional_resources={kind: dict(marker=kind + '.json', sha256=hashlib.sha256((ROOT / ('android/game-data-' + kind + '.json')).read_bytes()).hexdigest())
                                    for kind in ('port-ui', 'switch-ui')},
                storage='Folder or stored ZIP with prefixed resources in the same root',
                texture_conversion='Original DXT on supported GPUs; on-device ASTC conversion to sidecars')


def prepare(work, manifest_text):
    candidates = [Path(os.environ['PORT_LAUNCHER'])] if os.environ.get('PORT_LAUNCHER') else []
    candidates += [ROOT / 'android/launcher-base', ROOT / 'android/launcher', ROOT.parent / 'launcher']
    launcher_root = next((p.resolve() for p in candidates if (p / 'tools/launcher.py').is_file()), None)
    if launcher_root is None:
        raise FileNotFoundError('Set PORT_LAUNCHER, or provide android/launcher or ../launcher')
    module_spec = importlib.util.spec_from_file_location('frostpunk_port_launcher', launcher_root / 'tools/launcher.py')
    launcher = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(launcher)
    work = Path(work).resolve()
    assert work.is_relative_to((ROOT / 'build').resolve())
    work.mkdir(parents=True, exist_ok=True)
    manifest = work / 'game-manifest.xml'
    manifest_text = manifest_text.replace('android:targetSdkVersion="36"', 'android:targetSdkVersion="37"')
    manifest_text = manifest_text.replace('android:versionName="0.1-static-startup"', 'android:versionName="0.1.1-dev"')
    manifest_text = manifest_text.replace('android:versionCode="1"', 'android:versionCode="3"')
    manifest_text = manifest_text.replace('<application ', '<application android:appCategory="game" ', 1) if 'android:appCategory=' not in manifest_text else manifest_text
    manifest_text = manifest_text.replace('<uses-sdk ', '<!-- launcher:manifest -->\n  <uses-sdk ', 1)
    manifest_text = manifest_text.replace('<activity ', '<!-- launcher:application -->\n    <activity ', 1)
    manifest_text = manifest_text.replace('<intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>', '')
    manifest_text = manifest_text.replace('android:launchMode="singleTask"', 'android:launchMode="singleTop"')
    manifest.write_text(manifest_text, encoding='utf-8')
    result = launcher.prepare(ROOT / 'android/port.json', manifest, work / 'prepared')
    for kind in ('originals', 'port-ui', 'switch-ui'):
        shutil.copyfile(ROOT / ('android/game-data-' + kind + '.json'), Path(result['assets']) / ('game-data-' + kind + '.json'))
    (Path(result['assets']) / 'game-inputs.tsv').write_text(game_input_manifest(), encoding='utf-8', newline='\n')
    (Path(result['assets']) / 'game-data-contract.json').write_text(json.dumps(game_data_contract(), indent=2) + '\n', encoding='utf-8')
    shutil.copyfile(ROOT / 'android/launcher-compatibility-localization.json', Path(result['assets']) / 'launcher/compatibility.json')
    renderer_source = next(Path(p) for p in result['sources'] if Path(p).name == 'Renderers.java')
    renderer_text = renderer_source.read_text(encoding='utf-8')
    start = renderer_text.index('    static Status judge(')
    end = renderer_text.index("    /** The check's missing-requirement", start)
    renderer_text = renderer_text[:start] + '''    static Status judge(Context context, Spec.Mode mode, Map<String, String> report) {
        return FrostpunkDevice.judge(context, mode, report);
    }

''' + renderer_text[end:]
    edits = {
        '        if (converted) return new Status(mode, false, false, Texts.t(context, Texts.R_CONVERTED));\n': '',
        'if (mode != null) result.add(': 'if (mode != null && FrostpunkDevice.applies(gpu.qualcomm(), mode.kind)) result.add(',
        '        return chosen;': '        return status(statuses, chosen) != null || statuses.isEmpty() ? chosen : statuses.get(0).mode.id;',
        'Texts.fmt(context, Texts.R_ANDROID, version(mode.minSdk), Build.VERSION.SDK_INT)': 'FrostpunkDevice.text(context, "android", version(mode.minSdk), Build.VERSION.SDK_INT)',
        'Texts.fmt(context, Texts.R_LIBRARIES, String.join(", ", missing))': 'FrostpunkDevice.text(context, "libraries", String.join(", ", missing))',
        'Texts.t(context, Texts.R_NO_VULKAN)': 'FrostpunkDevice.text(context, "no_vulkan")',
        'Texts.fmt(context, Texts.R_VULKAN, Gpu.version(gpu.vulkan), Gpu.version(mode.vulkan))': 'FrostpunkDevice.text(context, "api", Gpu.version(gpu.vulkan))',
    }
    for before, after in edits.items():
        assert renderer_text.count(before) == 1, before
        renderer_text = renderer_text.replace(before, after)
    renderer_source.write_text(renderer_text, encoding='utf-8')
    activity = next(Path(p) for p in result['sources'] if Path(p).name == 'LauncherActivity.java')
    activity_text = activity.read_text(encoding='utf-8')
    edits = {
        '        String chosen = explicit != null ? explicit : spec.defaultMode;':
            '        String chosen = Renderers.effective(spec.defaultMode, statuses, explicit != null ? explicit : spec.defaultMode, explicit != null);',
        'current != null && !current.verified && fallback != null && !fallback.id.equals(chosen)':
            'current != null && !current.verified && fallback != null && Renderers.status(statuses, fallback.id) != null && !fallback.id.equals(chosen)',
        'Texts.fmt(this, Texts.GPU, gpu.renderer.isEmpty() ? "?" : gpu.renderer, Gpu.version(gpu.vulkan))':
            'FrostpunkDevice.text(this, "gpu", gpu.renderer.isEmpty() ? "?" : gpu.renderer)',
    }
    for before, after in edits.items():
        assert activity_text.count(before) == 1, before
        activity_text = activity_text.replace(before, after)
    play_button = '            card.addView(ui.buttons(play));'
    assert activity_text.count(play_button) == 1
    activity_text = activity_text.replace(play_button, '''            String problem = running ? null : fpDeviceProblem(d);
            if (problem != null) card.addView(ui.label(problem, 13, ui.error));
''' + play_button + '''
            if (Work.current != null && !Work.current.finished && Work.current.section == Texts.PLAY) {
                attach(card, work);
                work.setVisibility(View.VISIBLE);
            }''')
    cancel_texture = 'if (textures != null && job.section == Texts.TEXTURES) textures.cancel();'
    assert activity_text.count(cancel_texture) == 1
    activity_text = activity_text.replace(cancel_texture, 'if (textures != null && (job.section == Texts.TEXTURES || job.section == Texts.PLAY && fpTexturePlay != null)) textures.cancel();')
    etc2_button = '        buttons.add(ui.button(Texts.fmt(this, Texts.TEX_CONVERT, "ETC2"), Ui.SECONDARY, () -> startTextures(TextureBackend.ETC2)));'
    assert activity_text.count(etc2_button) == 1
    activity_text = activity_text.replace(etc2_button, '')
    group_loop = '        for (Spec.Option o : list) {'
    assert activity_text.count(group_loop) == 1
    activity_text = activity_text.replace(group_loop, group_loop + '\n            if (fpGraphicsGroup(o.key)) continue;')
    options_hint = '        String hint = Texts.of(this, spec.optionsHint);'
    assert activity_text.count(options_hint) == 1
    activity_text = activity_text.replace(options_hint, '''        TextView advanced = ui.button(FrostpunkDevice.tr(this, "Advanced graphics settings", "Дополнительные настройки"), Ui.SECONDARY, this::fpGraphicsAdvanced);
        Ui.enable(advanced, idle());
        card.addView(ui.buttons(advanced));
''' + options_hint)
    files_refresh = '    private void refreshFiles(boolean idle) {\n        files.removeAllViews();'
    assert activity_text.count(files_refresh) == 1
    activity_text = activity_text.replace(files_refresh, files_refresh + '\n        fpResourceWarnings();')
    native_dxt = '        if (!gpu.probed || Boolean.TRUE.equals(dxtSupport())) return;'
    assert activity_text.count(native_dxt) == 1
    activity_text = activity_text.replace(native_dxt, '''        if (!gpu.probed) return;
        boolean nativeDxt = Boolean.TRUE.equals(dxtSupport());
        if (nativeDxt) card.addView(ui.label(FrostpunkDevice.tr(this,
            "This renderer supports original DXT textures. Conversion is optional.",
            "Этот рендерер поддерживает исходные DXT-текстуры. Конвертация необязательна."), 13, ui.good));''')
    texture_reason = '        card.addView(ui.label(Texts.t(this, Texts.TEX_WHY), 13, ui.muted));'
    assert activity_text.count(texture_reason) == 1
    activity_text = activity_text.replace(texture_reason,
        '        if (!nativeDxt) card.addView(ui.label(FrostpunkDevice.tr(this,\n'
        '            "This GPU needs a one-time DXT to ASTC conversion. Original files are kept.",\n'
        '            "Для этого GPU нужна разовая конвертация DXT в ASTC. Исходные файлы сохранятся."), 13, ui.muted));')
    texture_card = '        if (!Install.ready(this, spec, selected)) return;\n        if (textures == null) {'
    assert activity_text.count(texture_card) == 1
    activity_text = activity_text.replace(texture_card, texture_card.replace('        if (textures == null)',
        '        fpTextureQualityCard(card, idle);\n        if (textures == null)'))
    texture_source = '        String source = textureSourceKey();'
    assert activity_text.count(texture_source) == 1
    activity_text = activity_text.replace(texture_source,
        '        if (idle && textures instanceof FrostpunkTextures) ((FrostpunkTextures)textures).quality(fpReducedTextureQuality());\n'
        '        String source = textureSourceKey();')
    texture_key = '            + Install.mount(this, selected, selected.parts.get(0));'
    assert activity_text.count(texture_key) == 1
    activity_text = activity_text.replace(texture_key, texture_key[:-1] + ' + "\\n" + fpReducedTextureQuality();')
    dxt_status = '            return bc == null ? null : "111".equals(bc);'
    assert activity_text.count(dxt_status) == 1
    activity_text = activity_text.replace(dxt_status,
        '            return bc == null ? null : FrostpunkDevice.dxt(report, effectiveRenderer(selected));')
    master_toggle = '                    on -> State.put(this, State.CHOICES, key, on ? "1" : "0")));'
    assert activity_text.count(master_toggle) == 1
    activity_text = activity_text.replace(master_toggle,
        '                    on -> { State.put(this, State.CHOICES, key, on ? "1" : "0");\n'
        '                        if (o.key.equals("switch_graphics")) { textureScan = null; refresh(Texts.TEXTURES); } }));')
    converted_card = '        if (converted) {\n            card.addView(ui.label(Texts.fmt(this, Texts.TEX_DONE,'
    assert activity_text.count(converted_card) == 1
    activity_text = activity_text.replace(converted_card, converted_card.replace('            card.addView',
        '            try {\n'
        '                boolean ready = FrostpunkTextures.coversQuality(textureRoot(), fpReducedTextureQuality());\n'
        '                card.addView(ui.label(FrostpunkDevice.tr(this, "Converted quality: ", "Качество готового набора: ")\n'
        '                    + fpTextureQualityTitle(FrostpunkTextures.convertedQuality(textureRoot()).equals("switch")), 14, ui.text));\n'
        '                if (!ready) card.addView(ui.label(FrostpunkDevice.tr(this,\n'
        '                    "Selected quality is missing. Play will offer to prepare it.",\n'
        '                    "Текстур выбранного качества нет. При нажатии Играть будет предложена подготовка."), 13, ui.error));\n'
        '            } catch (java.io.IOException failure) { note(Texts.TEXTURES, failure.getMessage(), true); return; }\n'
        '            card.addView'))
    planned_space = '        List<TextView> buttons = new ArrayList<>();\n        if (gpu.astc) buttons.add'
    assert activity_text.count(planned_space) == 1
    activity_text = activity_text.replace(planned_space,
        '        if (textures instanceof FrostpunkTextures) card.addView(ui.label(FrostpunkDevice.tr(this,\n'
        '            "Additional space: ", "Дополнительное место: ") + Texts.size(this, ((FrostpunkTextures)textures).outputBytes()), 13, ui.muted));\n'
        + planned_space)
    texture_backend = '        TextureBackend backend = textures;'
    assert activity_text.count(texture_backend) == 1
    activity_text = activity_text.replace(texture_backend, texture_backend + '\n'
        '        if (backend instanceof FrostpunkTextures) ((FrostpunkTextures)backend).quality(fpReducedTextureQuality());\n'
        '        final boolean rebuild = fpTextureRebuild; fpTextureRebuild = false;')
    texture_result = '                int result = format == 0 ? backend.undo(root)'
    assert activity_text.count(texture_result) == 1
    activity_text = activity_text.replace(texture_result,
        '                if (rebuild) {\n'
        '                    if (backend.undo(root) != 0) throw new Failure(Failure.IO, backend.message());\n'
        '                    check();\n'
        '                }\n' + texture_result)
    storage_check = '''    private boolean needsAllFiles() {
        if (!spec.allFilesAccess) return false;
        for (Spec.Distribution d : spec.distributions)
            for (Spec.Part p : d.parts) if (!Storage.own(this, Install.folder(this, spec, d, p))) return true;
        return false;
    }'''
    assert activity_text.count(storage_check) == 1
    activity_text = activity_text.replace(storage_check, '''    private boolean needsAllFiles() {
        // Persistent profiles use /sdcard/Frostpunk even with app-owned game files.
        return spec.allFilesAccess;
    }''')
    # The shared launcher moved the same launch failure from its banner to the Play card.
    launch_error = '            error(Texts.t(this, Texts.NOT_IN_BUILD));'
    launch_note = '            note(Texts.PLAY, Texts.t(this, Texts.NOT_IN_BUILD), true);'
    assert activity_text.count(launch_error) + activity_text.count(launch_note) == 1
    launch_failure = launch_note if launch_note in activity_text else launch_error
    texture_job = '        confirmJob(new Work.Job(Texts.t(this, Texts.JOB_TEXTURES), root) {'
    texture_section_job = '        confirmJob(new Work.Job(Texts.TEXTURES, Texts.t(this, Texts.JOB_TEXTURES), root) {'
    assert activity_text.count(texture_job) + activity_text.count(texture_section_job) == 1
    texture_job = texture_section_job if texture_section_job in activity_text else texture_job
    finished_refresh = '        refresh(Texts.PERM_FILES, Texts.PLAY, Texts.GAME_FILES, Texts.RENDERER, Texts.TEXTURES, Texts.SPACE);\n    }'
    finished_all = '        textureScan = null;\n        refresh();\n    }'
    assert activity_text.count(finished_refresh) + activity_text.count(finished_all) == 1
    finished_anchor = finished_refresh if finished_refresh in activity_text else finished_all
    item_progress = 'if (p != null && p.length >= 4) { done = p[2]; total = p[3]; itemsDone = p[0]; itemsTotal = p[1]; }'
    byte_progress = 'if (p != null && p.length >= 4) { done = p[2]; total = p[3]; }'
    assert activity_text.count(item_progress) + activity_text.count(byte_progress) == 1
    progress_anchor = item_progress if item_progress in activity_text else byte_progress
    launch_block = '''        try {
            Launch.start(this, d, effectiveRenderer(d), hooks);
            finish();
        } catch (ActivityNotFoundException e) {
            error(Texts.t(this, Texts.NOT_IN_BUILD));
        }'''.replace(launch_error, launch_failure)
    deferred_launch = launch_block.replace('            finish();', '            finishAfterLaunch = true;')
    assert activity_text.count(launch_block) + activity_text.count(deferred_launch) == 1
    launch_anchor = deferred_launch if deferred_launch in activity_text else launch_block
    edits = {
        launch_anchor: '        fpPlay(d);',
        finished_anchor: finished_anchor[:-5] + '        fpTexturesFinished(job);\n    }',
        texture_job: texture_job.replace('confirmJob(', 'startJob(', 1).replace('new Work.Job(Texts.TEXTURES,', 'new Work.Job(fpTexturePlay != null ? Texts.PLAY : Texts.TEXTURES,')
            + '\n            @Override String progressText(android.content.Context context) { return meter.describe(Texts.language(context), true, done, total, itemsDone, itemsTotal); }',
        progress_anchor: progress_anchor[:-1] + 'file = backend.message(); }',
    }
    for before, after in edits.items():
        assert activity_text.count(before) == 1, before
        activity_text = activity_text.replace(before, after)
    activity_text = activity_text.rstrip().removesuffix('}') + '\n' + (ROOT / 'android/launcher/launch_checks.java.inc').read_text(encoding='utf-8') + '\n}\n'
    activity.write_text(activity_text, encoding='utf-8')
    ui = next(Path(p) for p in result['sources'] if Path(p).name == 'Ui.java')
    ui.write_text(snow_java(ui.read_text(encoding='utf-8')), encoding='utf-8')
    for source in sorted((ROOT / 'android/launcher').glob('*.java')):
        target = activity.parent / source.name
        shutil.copyfile(source, target)
        result['sources'].append(str(target))
    result['probe_library'] = build_probe(launcher, work, result['package'])
    result['texture_library'] = build_textures()
    licenses = Path(result['assets']) / 'licenses'
    licenses.mkdir(exist_ok=True)
    for source, name in [('reference/astcenc/LICENSE.txt', 'ASTC-encoder.txt'),
                         ('reference/bcdec/bcdec.h', 'bcdec.txt'), ('reference/texture-codecs.json', 'Texture-codecs.json'),
                         ('reference/pcrecomp/LICENSE', 'pcrecomp.txt'),
                         ('reference/protobuf-21.12/source/protobuf-3.21.12/LICENSE', 'protobuf.txt'),
                         ('reference/dxvk/subprojects/libdisplay-info/LICENSE', 'libdisplay-info.txt'),
                         ('reference/dxvk/include/spirv/LICENSE', 'SPIRV-Headers.txt'),
                         ('reference/dxvk/include/vulkan/LICENSE.md', 'Vulkan-Headers.txt'),
                         ('reference/dxvk/include/vulkan/LICENSES/Apache-2.0.txt', 'Vulkan-Headers-Apache-2.0.txt'),
                         ('reference/dxvk/include/vulkan/LICENSES/MIT.txt', 'Vulkan-Headers-MIT.txt')]:
        shutil.copyfile(ROOT / source, licenses / name)
    for source, name in [('reference/goldberg/json/json.hpp', 'nlohmann-json.txt'),
                         ('reference/goldberg/sha/sha1.hpp', 'Goldberg-SHA1-public-domain.txt')]:
        notice = (ROOT / source).read_text(encoding='utf-8')
        assert notice.startswith('/*') and '*/' in notice
        (licenses / name).write_text(notice[:notice.index('*/') + 2] + '\n', encoding='utf-8')
    return result


def snow_java(text):
    edits = {
        '        private final Path water = new Path();':
            '        private final Path water = new Path();\n        private FrostpunkSnow snow;',
        '                if (effect == 2 && particleStyle != 3) {':
            '                if (effect == 2 && particleStyle == 1) {\n'
            '                    if (snow == null) snow = new FrostpunkSnow();\n'
            '                    snow.draw(canvas, paint, getWidth(), getHeight(),\n'
            '                        activity.getResources().getDisplayMetrics().density,\n'
            '                        motion && hasWindowFocus() && getWindowVisibility() == View.VISIBLE && isShown(), particles);\n'
            '                }\n'
            '                if (effect == 2 && particleStyle != 3 && particleStyle != 1) {',
        '                if (motion && hasWindowFocus() && getWindowVisibility() == View.VISIBLE && isShown()) postInvalidateDelayed(33);':
            '                if (motion && hasWindowFocus() && getWindowVisibility() == View.VISIBLE && isShown()) {\n'
            '                    if (effect == 2 && particleStyle == 1) postInvalidateOnAnimation();\n'
            '                    else postInvalidateDelayed(33);\n'
            '                }',
    }
    for before, after in edits.items():
        assert text.count(before) == 1, before
        text = text.replace(before, after)
    return text


def build_probe(launcher, work, package):
    source = Path(launcher.PROBE_SOURCE)
    text = source.read_text(encoding='utf-8')
    anchor = '    caps.core = f2.features;'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '''
    int astc = f2.features.textureCompressionASTC_LDR;
    const VkFormat astc_formats[] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
        VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    for (unsigned i = 0; i < 4; ++i) {
        VkFormatProperties fp; formats(chosen, astc_formats[i], &fp);
        astc = astc && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    }
    snprintf(text, sizeof text, "%d", astc); port_line(r, prefix, "astc", text);
    snprintf(text, sizeof text, "%u", f2.features.textureCompressionBC); port_line(r, prefix, "bcFeature", text);
    snprintf(text, sizeof text, "%u", p.deviceType); port_line(r, prefix, "gpuType", text);
    snprintf(text, sizeof text, "%u", r2.robustBufferAccess2); port_line(r, prefix, "robustBufferAccess2", text);
    snprintf(text, sizeof text, "%u", v12.descriptorBindingPartiallyBound); port_line(r, prefix, "partiallyBound", text);
    VkFormatProperties bgra; formats(chosen, VK_FORMAT_B8G8R8A8_UNORM, &bgra);
    snprintf(text, sizeof text, "%d", !!(bgra.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
    port_line(r, prefix, "bgra", text);
''')
    path = work / 'probe/vulkan_probe.c'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
    shutil.copyfile(source.parent / 'vulkan_requirements.h', path.parent / 'vulkan_requirements.h')
    launcher.PROBE_SOURCE = path
    try:
        return launcher.build_probe(path.parent, package, ndk=ndk_root())
    finally:
        launcher.PROBE_SOURCE = source


def build_textures():
    for name, expected in json.loads((ROOT / 'reference/texture-codecs.json').read_text())['sources'].items():
        with (ROOT / name).open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
                raise ValueError('Texture codec source changed: ' + name)
    ndk = ndk_root()
    cmake = ndk.parents[1] / 'cmake/3.22.1/bin/cmake.exe'
    out = ROOT / 'build/texture-converter'
    commands = [[cmake, '-S', ROOT / 'android/texconv', '-B', out, '-G', 'Ninja',
                 '-DCMAKE_MAKE_PROGRAM=' + str(cmake.parent / 'ninja.exe'),
                 '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'),
                 '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30', '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release'],
                [cmake, '--build', out, '--target', 'frostpunk_texconv', '--parallel', '6']]
    for command in commands:
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError('Texture converter build failed:\n' + result.stdout[-4000:] + result.stderr[-4000:])
    return out / 'libfrostpunk_texconv.so'


def ndk_root():
    import json
    config = json.loads((ROOT / 'port.json').read_text())
    return Path(config['android_sdk']) / 'ndk' / config['android_ndk_version']


def main_java(text):
    assert text.count('super.onCreate(state);') == 1
    text = text.replace('import android.app.NativeActivity;',
                        'import android.app.NativeActivity;\nimport '+PACKAGE+'.launcher.Launch;')
    text = text.replace('private TextView status;', 'private Launch launch;\n    private volatile boolean presented;\n    private TextView status;')
    text = text.replace('super.onCreate(state);', 'launch=Launch.attach(this);\n        launch.starting();\n'
                        '        try { '+PACKAGE+'.launcher.FrostpunkData.attach(this); }\n'
                        '        catch(Exception failure) { throw new IllegalStateException("Cannot validate game resource packages", failure); }\n'
                        '        super.onCreate(state);')
    text = text.replace('public void showNativeStatus(final String text) {',
                        'public String prepareNativeRuntime() throws java.io.IOException {\n'
                        '        String game=android.system.Os.getenv("PORT_PART_GAME");\n'
                        '        '+PACKAGE+'.launcher.FrostpunkTextures.recover(game!=null?new java.io.File(game):new java.io.File(getExternalFilesDir(null),"game"));\n'
                        '        return NativeLibrary.prepare(this, this::showNativeStatus); }\n'
                        '    public void gamePresented() { presented=true; showNativeStatus(""); runOnUiThread(() -> { if(controls!=null)controls.presented(); launch.firstFrame(null); }); }\n'
                        '    public void onNativeExiting(int code) { if(launch!=null)launch.exiting(code,null); }\n'
                        '    public void showNativeStatus(final String text) {\n'
                        '        if(text.equals("\\u0001")) { gamePresented(); return; }\n'
                        '        if(text.isEmpty() && !presented) { showNativeStatus("Loading game resources…"); return; }\n'
                        '        if(text.startsWith("\\u0005")) launch.failed(text.substring(1));\n'
                        '        else if(text.startsWith("Native game runtime returned") || text.startsWith("Missing ") || text.startsWith("Cannot ")) launch.failed(text);')
    return text
