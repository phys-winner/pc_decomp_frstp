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
VERSION = ('0.4-dev', 10)
LANGUAGES = ('en', 'ru', 'de', 'pl', 'fr', 'es', 'it', 'pt-BR', 'tr', 'ja', 'ko', 'zh-CN', 'fa', 'cs', 'hu')


def game_input_manifest(port_ui_variants=()):
    originals = json.loads((ROOT / 'android/game-data-originals.json').read_text())
    legacy = json.loads((ROOT / 'research/game-host-data-port-settings.json').read_text())['files']
    sizes = {name: {row['bytes'], legacy[name]['bytes']} for name, row in originals.items()}
    resources = [json.loads((ROOT / ('android/game-data-' + kind + '.json')).read_text())
                 for kind in ('port-ui', 'switch-ui')]
    for resource in [*resources, *port_ui_variants]:
        for archive in resource['archives']:
            for suffix in ('.dat', '.idx'):
                sizes[archive['stem'] + suffix].add(archive['output'][suffix]['bytes'])
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


def fan_localizations(source, assets, work):
    from package_game_data import port_ui_patch, sha
    from port_settings_localization import FAN_LANGUAGES
    pins = {
        'cs': ('9e9c58be4efdb55e777d843d190a31734c56ebf207886a4375634388c1fae7e9',
               '5e5ac52409e65e2bd9f1345c784e17ab70b75d0b3bced74d9a2548978e4f652b'),
        'hu': ('3bc68c99bd7ab3027b0989e607e5889b95e4a4fc3cefc10f98b6052420a9e13a',
               '58a621ad47d9797fd0516a64b32dd44b0f89c19cd7142056a9da55c7756f0cb4'),
    }
    fan_files = {}
    for language in FAN_LANGUAGES:
        folder = source / language
        if not folder.is_dir():
            continue
        actual = tuple(sha(folder / f'localizations.{suffix}') for suffix in ('dat', 'idx'))
        if actual != pins[language]:
            raise ValueError(f'Unrecognized {language} community localization files')
        fan_files[language] = folder
    if not fan_files:
        raise ValueError('No community localization files found')
    variant, manifest = port_ui_patch(work / 'fan-ui', fan_files)
    shutil.copytree(variant, assets / 'port-ui', dirs_exist_ok=True)
    shutil.copyfile(variant / 'port-ui.json', assets / 'game-data-port-ui.json')
    (assets / 'game-inputs.tsv').write_text(game_input_manifest([manifest]), encoding='utf-8', newline='\n')
    contract = game_data_contract()
    contract['optional_resources']['port-ui']['sha256'] = sha(assets / 'game-data-port-ui.json')
    (assets / 'game-data-contract.json').write_text(json.dumps(contract, indent=2) + '\n', encoding='utf-8')


def prepare(work, manifest_text):
    candidates = [Path(os.environ['PORT_LAUNCHER'])] if os.environ.get('PORT_LAUNCHER') else []
    candidates += [ROOT / 'android/launcher-base', ROOT / 'android/launcher', ROOT.parent / 'launcher']
    launcher_root = next((p.resolve() for p in candidates if (p / 'tools/launcher.py').is_file()), None)
    if launcher_root is None:
        raise FileNotFoundError('Set PORT_LAUNCHER, or provide android/launcher or ../launcher')
    module_spec = importlib.util.spec_from_file_location('frostpunk_port_launcher', launcher_root / 'tools/launcher.py')
    launcher = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(launcher)
    launcher.LANGUAGES = LANGUAGES
    work = Path(work).resolve()
    assert work.is_relative_to((ROOT / 'build').resolve())
    work.mkdir(parents=True, exist_ok=True)
    manifest = work / 'game-manifest.xml'
    manifest_text = manifest_text.replace('android:targetSdkVersion="36"', 'android:targetSdkVersion="37"')
    manifest_text = manifest_text.replace('android:versionName="0.1-static-startup"', f'android:versionName="{VERSION[0]}"')
    manifest_text = manifest_text.replace('android:versionCode="1"', f'android:versionCode="{VERSION[1]}"')
    manifest_text = manifest_text.replace('<application ', '<application android:appCategory="game" ', 1) if 'android:appCategory=' not in manifest_text else manifest_text
    manifest_text = manifest_text.replace('<uses-sdk ', '<!-- launcher:manifest -->\n  <uses-sdk ', 1)
    manifest_text = manifest_text.replace('<activity ', '<!-- launcher:application -->\n    <activity ', 1)
    manifest_text = manifest_text.replace('<intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>', '')
    manifest_text = manifest_text.replace('android:launchMode="singleTask"', 'android:launchMode="singleTop"')
    manifest.write_text(manifest_text, encoding='utf-8')
    result = launcher.prepare(ROOT / 'android/port.json', manifest, work / 'prepared')
    from package_game_data import port_ui_patch
    bundled, _ = port_ui_patch(work / 'bundled-ui')
    shutil.copytree(bundled, Path(result['assets']) / 'port-ui', dirs_exist_ok=True)
    for kind in ('originals', 'port-ui', 'switch-ui'):
        shutil.copyfile(ROOT / ('android/game-data-' + kind + '.json'), Path(result['assets']) / ('game-data-' + kind + '.json'))
    (Path(result['assets']) / 'game-inputs.tsv').write_text(game_input_manifest(), encoding='utf-8', newline='\n')
    (Path(result['assets']) / 'game-data-contract.json').write_text(json.dumps(game_data_contract(), indent=2) + '\n', encoding='utf-8')
    shutil.copyfile(ROOT / 'android/launcher-compatibility-localization.json', Path(result['assets']) / 'launcher/compatibility.json')
    shutil.copyfile(ROOT / 'android/launcher-localization.json', Path(result['assets']) / 'launcher/translations.json')
    base_extensions(result)
    renderer_extensions(result)
    activity = activity_extensions(result)
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


def patch(result, name, edits):
    """Applies exact one-occurrence replacements to one generated base class."""
    source = next(Path(p) for p in result['sources'] if Path(p).name == name)
    text = source.read_text(encoding='utf-8')
    for before, after in edits.items():
        assert text.count(before) == 1, name + ': ' + before
        text = text.replace(before, after)
    source.write_text(text, encoding='utf-8', newline='\n')
    return source


def base_extensions(result):
    """Frostpunk's additions to the shared base. android/launcher-base stays a verbatim copy of ../launcher
    (plus ZipIntegrity.java), so a refresh is a copy; everything the port adds is applied here."""
    patch(result, 'Texts.java', {
        'static final String[] LANGUAGES = {"en", "ru", "de", "pl"};':
            'static final String[] LANGUAGES = {' + ', '.join('"%s"' % code for code in LANGUAGES) + '};\n'
            '    private static JSONObject translations;',
        '        return "uk".equals(language) || "be".equals(language) ? "ru" : language;':
            '        if ("pt".equals(language)) return "pt-BR";\n'
            '        if ("zh".equals(language)) return "zh-CN";\n'
            '        return "uk".equals(language) || "be".equals(language) ? "ru" : language;',
        '    static String t(Context context, int id) { return TEXTS[id][column(context)]; }':
            """    static String t(Context context, int id) {
        String[] row = TEXTS[id];
        int selected = column(context);
        return selected < row.length ? row[selected] : translated(context, row[0]);
    }

    /** Languages beyond the base's four: assets/launcher/translations.json, keyed by the English text. */
    static synchronized String translated(Context context, String english) {
        if (context == null || english == null || english.isEmpty()) return english;
        String language = language(context);
        if ("en".equals(language)) return english;
        if (translations == null) {
            try { translations = new JSONObject(Spec.asset(context, "launcher/translations.json")); }
            catch (java.io.IOException | org.json.JSONException unavailable) { translations = new JSONObject(); }
        }
        JSONObject row = translations.optJSONObject(language);
        return row == null ? english : row.optString(english, english);
    }

    static String english(int id) { return TEXTS[id][0]; }

    /** A row of the base's four languages (HudField titles) in the launcher's language. */
    static String pick(Context context, String[] row) {
        int selected = column(context);
        return selected < row.length ? row[selected] : translated(context, row[0]);
    }""",
        '        if (text.isEmpty()) text = map.optString("en", "");':
            '        if (text.isEmpty()) text = translated(context, map.optString("en", ""));',
    })
    patch(result, 'Launch.java', {
        '    public static String language(Context context) { return Texts.language(context); }':
            '    public static String language(Context context) { return Texts.language(context); }\n\n'
            '    /** The launcher\'s language for the game\'s own messages (GameWatchActivity). */\n'
            '    public static String translated(Context context, String english) { return Texts.translated(context, english); }',
        # The CUBOT linear-gralloc workaround of PanVK: on by device, the player can switch it.
        '        set("PORT_PANVK_DRIVER", panvk != null ? panvk.getAbsolutePath() : "");\n':
            '        set("PORT_PANVK_DRIVER", panvk != null ? panvk.getAbsolutePath() : "");\n'
            '        set("MESA_GRALLOC_LINEAR", FrostpunkDevice.mesaGrallocLinear(activity, Build.DEVICE) ? "1" : "0");\n',
    })
    patch(result, 'Diagnostics.java', {
        '            + "\\nABI: " + String.join(", ", Build.SUPPORTED_ABIS) + "\\nGPU: " + gpu + "\\nRenderer: " + renderer':
            '            + "\\nDevice: " + FrostpunkDevice.deviceName()\n'
            '            + "\\nABI: " + String.join(", ", Build.SUPPORTED_ABIS)\n'
            '            + "\\nRAM: " + FrostpunkDevice.memory(context)\n'
            '            + "\\nGPU: " + gpu + "\\nRenderer: " + renderer\n'
            '            + "\\nInstall: " + FrostpunkDevice.installKind(context)',
    })
    # Frostpunk publishes -dev prereleases, which /releases/latest skips: its Updates.java also reads
    # the release list. The override is the shared class of the pinned revision plus that fallback.
    updates = next(Path(p) for p in result['sources'] if Path(p).name == 'Updates.java')
    shared = (ROOT / 'android/launcher-base/android/src/port/launcher/Updates.java').read_bytes().replace(b'\r\n', b'\n')
    assert hashlib.sha256(shared).hexdigest() == 'c149b21896a806409fd9ff180740af872d796f1afbc04966747ec4c4d71aae84', \
        'The shared Updates.java changed: merge it into android/launcher/base/Updates.java and update this pin'
    override = (ROOT / 'android/launcher/base/Updates.java').read_text(encoding='utf-8')
    assert override.count('package port.launcher;') == 1
    updates.write_text(override.replace('package port.launcher;', 'package ' + result['package'] + ';'),
                       encoding='utf-8', newline='\n')


def renderer_extensions(result):
    """Renderer rows keep the shared rules (one reason per disabled row, measured API version, per-driver
    texture formats). Frostpunk adds what its own renderers need from the driver check."""
    patch(result, 'Renderers.java', {
        # DXVK on Mali/PowerVR runs through the port's overlays without the features forgiven here.
        '            tokens.remove("bcTextures"); // Exact original formats were checked above.\n':
            '            tokens.remove("bcTextures"); // Exact original formats were checked above.\n'
            '            FrostpunkDevice.forgive(tokens, report, driver);\n',
        # After the shared checks (hardware, Android, libraries, driver check): the port's own requirements.
        '        return judge(context, mode, report, originals);\n':
            '        return FrostpunkDevice.judge(context, mode, report, judge(context, mode, report, originals));\n',
        '        return judge(context, mode, report, mode.originalFormats);\n':
            '        return FrostpunkDevice.judge(context, mode, report, judge(context, mode, report, mode.originalFormats));\n',
    })


def activity_extensions(result):
    """Frostpunk's screens and checks inside the shared LauncherActivity (android/launcher/launch_checks.java.inc)."""
    edits = {
        '        build();\n        if (Build.VERSION.SDK_INT >= 33) {':
            '        build();\n        fpWarnLowMemory();\n        if (Build.VERSION.SDK_INT >= 33) {',
        # Renderer card: the bundled Turnip by name, and the driver workarounds of this GPU.
        'title.isEmpty() ? Texts.t(this, Texts.TURNIP_BUILTIN) : title), 14, ui.text));':
            'title.isEmpty() ? (panvk ? Texts.t(this, Texts.TURNIP_BUILTIN) : "purple-turnip T30 (Mesa)") : title), 14, ui.text));',
        '        if (panvkDrivers) driverControls(card, VulkanDriver.PANVK, ASK_PANVK_DRIVER);\n':
            '        if (panvkDrivers) driverControls(card, VulkanDriver.PANVK, ASK_PANVK_DRIVER);\n'
            '        fpWorkarounds(card, spec.mode(effective), probe);\n',
        # Play card: why no renderer of this build can run here; conversion started by Play reports there.
        '            card.addView(ui.buttons(play));':
            '            String problem = running || d != selected ? null : fpDeviceProblem(d);\n'
            '            if (problem != null) card.addView(ui.label(problem, 13, ui.error));\n'
            '            card.addView(ui.buttons(play));\n'
            '            if (Work.current != null && !Work.current.finished && Work.current.section == Texts.PLAY) {\n'
            '                attach(card, work);\n'
            '                work.setVisibility(View.VISIBLE);\n'
            '            }',
        # Options: the graphics groups live in their own dialog, memory use is a compact choice.
        '        for (Spec.Option o : list) {':
            '        for (Spec.Option o : list) {\n'
            '            if (fpGraphicsGroup(o.key)) continue;\n'
            '            if (o.key.equals("memory")) { fpMemoryOption(card, o); continue; }',
        '        String hint = Texts.of(this, spec.optionsHint);':
            '        TextView advanced = ui.button(FrostpunkDevice.tr(this, "Advanced graphics settings", "Дополнительные настройки"), Ui.SECONDARY, this::fpGraphicsAdvanced);\n'
            '        Ui.enable(advanced, idle());\n'
            '        card.addView(ui.buttons(advanced));\n'
            '        String hint = Texts.of(this, spec.optionsHint);',
        '                        if (spec.modesNeed(o.key)) refresh(Texts.RENDERER, Texts.TEXTURES, Texts.PLAY);':
            '                        if (spec.modesNeed(o.key)) refresh(Texts.RENDERER, Texts.TEXTURES, Texts.PLAY);\n'
            '                        if (o.key.equals("switch_graphics")) { textureScan = null; refresh(Texts.TEXTURES); }',
        # Game files: resource-pack warnings from a background snapshot, never a file walk on the UI thread.
        '    private void refreshFiles(boolean idle) {\n        files.removeAllViews();':
            '    private void refreshFiles(boolean idle) {\n        files.removeAllViews();\n        fpResourceWarnings();',
        # Textures: the Switch/PC mip quality belongs to the cache.
        '        if (textureBackend() == null) return;\n':
            '        if (textureBackend() == null) return;\n        fpTextureQualityCard(card, idle);\n',
        '        String source = textureSourceKey();':
            '        if (idle && textures instanceof FrostpunkTextures) ((FrostpunkTextures)textures).quality(fpReducedTextureQuality());\n'
            '        String source = textureSourceKey();',
        '            + Install.mount(this, selected, selected.parts.get(0));':
            '            + Install.mount(this, selected, selected.parts.get(0)) + "\\n" + fpReducedTextureQuality();',
        '        if (converted) {\n            card.addView(ui.label(Texts.fmt(this, Texts.TEX_DONE,':
            '        if (converted) {\n'
            '            try {\n'
            '                boolean ready = FrostpunkTextures.coversQuality(textureRoot(), fpReducedTextureQuality());\n'
            '                card.addView(ui.label(FrostpunkDevice.tr(this, "Converted quality: ", "Качество готового набора: ")\n'
            '                    + fpTextureQualityTitle(FrostpunkTextures.convertedQuality(textureRoot()).equals("switch")), 14, ui.text));\n'
            '                if (!ready) card.addView(ui.label(FrostpunkDevice.tr(this,\n'
            '                    "Selected quality is missing. Play will offer to prepare it.",\n'
            '                    "Текстур выбранного качества нет. При нажатии Играть будет предложена подготовка."), 13, ui.error));\n'
            '            } catch (java.io.IOException failure) { note(Texts.TEXTURES, failure.getMessage(), true); return; }\n'
            '            card.addView(ui.label(Texts.fmt(this, Texts.TEX_DONE,',
        '        List<TextView> buttons = new ArrayList<>();\n        Long caps = Launch.textureCaps(this, mode, gpu);\n        if (caps == null) return;':
            '        if (textures instanceof FrostpunkTextures) card.addView(ui.label(FrostpunkDevice.tr(this,\n'
            '            "Additional space: ", "Дополнительное место: ") + Texts.size(this, ((FrostpunkTextures)textures).outputBytes()), 13, ui.muted));\n'
            '        List<TextView> buttons = new ArrayList<>();\n        Long caps = Launch.textureCaps(this, mode, gpu);\n        if (caps == null) return;',
        '        boolean selective = backend.selective();\n'
        '        return new Work.Job(Texts.TEXTURES, Texts.t(this, mods == null ? Texts.JOB_TEXTURES : Texts.MOD_TEXTURES), format > 0 ? cache : root) {':
            '        boolean selective = backend.selective();\n'
            '        if (backend instanceof FrostpunkTextures) ((FrostpunkTextures)backend).quality(fpReducedTextureQuality());\n'
            '        final boolean rebuild = fpTextureRebuild; fpTextureRebuild = false;\n'
            '        return new Work.Job(fpTexturePlay != null ? Texts.PLAY : Texts.TEXTURES, Texts.t(this, mods == null ? Texts.JOB_TEXTURES : Texts.MOD_TEXTURES), format > 0 ? cache : root) {\n'
            '            @Override String progressText(android.content.Context context) { return meter.describe(Texts.language(context), true, done, total, itemsDone, itemsTotal); }',
        '                        result = format == 0 ? backend.undo(root, cache)':
            '                        if (rebuild) {\n'
            '                            if (backend.undo(root, cache) != 0) throw new Failure(Failure.IO, backend.message());\n'
            '                            check();\n'
            '                        }\n'
            '                        result = format == 0 ? backend.undo(root, cache)',
        'if (p != null && p.length >= 4) { done = p[2]; total = p[3]; itemsDone = p[0]; itemsTotal = p[1]; }':
            'if (p != null && p.length >= 4) { done = p[2]; total = p[3]; itemsDone = p[0]; itemsTotal = p[1]; file = backend.message(); }',
        # The first-start offer of the shared base goes on to the game when the conversion is done.
        '        if (etc2) dialog.setPositiveButton("ETC2", (dlg, w) -> startTextures(TextureBackend.ETC2, sources));\n'
        '        if (astc) dialog.setNeutralButton("ASTC", (dlg, w) -> startTextures(TextureBackend.ASTC, sources));':
            '        if (etc2) dialog.setPositiveButton("ETC2", (dlg, w) -> { fpTexturePlay = d; startTextures(TextureBackend.ETC2, sources); });\n'
            '        if (astc) dialog.setNeutralButton("ASTC", (dlg, w) -> { fpTexturePlay = d; startTextures(TextureBackend.ASTC, sources); });',
        '        if (!idle() || mode == null || !d.readsCache(mode) || textureBackend() == null) return false;\n        Long caps = Launch.textureCaps(this, mode, gpu);':
            '        if (fpOwnTextureOffer() || !idle() || mode == null || !d.readsCache(mode) || textureBackend() == null) return false;\n        Long caps = Launch.textureCaps(this, mode, gpu);',
        # ETC2 is read by the native Vulkan modes only (FrostpunkTextures.rendererReads): the DXVK rows
        # neither list it nor offer it.
        '                    for (String format : spec.etc2Formats) if (!formats.contains(format)) formats.add(format);':
            '                    if (FrostpunkTextures.rendererReads(s.mode.id, TextureBackend.ETC2))\n'
            '                        for (String format : spec.etc2Formats) if (!formats.contains(format)) formats.add(format);',
        '        if (Boolean.TRUE.equals(TextureFormats.supports(caps, spec.etc2Formats)))\n'
        '            buttons.add(ui.button(Texts.fmt(this, Texts.TEX_CONVERT, "ETC2")':
            '        if (Boolean.TRUE.equals(TextureFormats.supports(caps, spec.etc2Formats)) && FrostpunkTextures.rendererReads(mode.id, TextureBackend.ETC2))\n'
            '            buttons.add(ui.button(Texts.fmt(this, Texts.TEX_CONVERT, "ETC2")',
        # The workaround switches are shown by fpWorkarounds for the driver they belong to.
        '            if (!option.rendererSetting || (option.arm64Only && !android.os.Process.is64Bit())) continue;':
            '            if (!option.rendererSetting || option.key.startsWith("fix_") || (option.arm64Only && !android.os.Process.is64Bit())) continue;',
        # Persistent profiles use /sdcard/Frostpunk even with app-owned game files.
        '''    private boolean needsAllFiles() {
        if (!spec.allFilesAccess) return false;
        for (Spec.Distribution d : spec.distributions)
            for (Spec.Part p : d.parts) if (!Storage.own(this, Install.folder(this, spec, d, p))) return true;
        return false;
    }''': '''    private boolean needsAllFiles() {
        return spec.allFilesAccess;
    }''',
        # A start goes through the data check, the texture quality check and the renderer question.
        '''        try {
            Launch.start(this, d, renderer, hooks);
            finishAfterLaunch = true;
            return true;
        } catch (ActivityNotFoundException e) {
            note(Texts.PLAY, Texts.t(this, Texts.NOT_IN_BUILD), true);
            return false;
        }''': '        return fpPlay(d, renderer);',
        '        refresh(Texts.PERM_FILES, Texts.PLAY, Texts.GAME_FILES, Texts.RENDERER, Texts.TEXTURES, Texts.SPACE);\n'
        '        if (job instanceof Install.UnpackJob && job.failure == null) refresh(Texts.OPTIONS);':
            '        refresh(Texts.PERM_FILES, Texts.PLAY, Texts.GAME_FILES, Texts.RENDERER, Texts.TEXTURES, Texts.SPACE);\n'
            '        fpFinished(job);\n'
            '        if (job instanceof Install.UnpackJob && job.failure == null) refresh(Texts.OPTIONS);',
        # Fifteen languages: rows of the base's four fall back to translations.json.
        'names[field.ordinal()] = field.title[Texts.column(this)];': 'names[field.ordinal()] = Texts.pick(this, field.title);',
        'HyperOs.advice(vendor, column)': 'Texts.translated(this, HyperOs.advice(vendor, column))',
        # The tool's name (realme/OPPO "Game Assistant") in the same words as the translated advice.
        'String system = HyperOs.system(vendor, Build.MANUFACTURER), tool = HyperOs.tool(vendor, column);':
            'String system = HyperOs.system(vendor, Build.MANUFACTURER), tool = Texts.translated(this, HyperOs.tool(vendor, column));',
        '.setMessage(Texts.fmt(this, Texts.HYPEROS_FAILED, HyperOs.tool(vendor, Texts.column(this))))':
            '.setMessage(Texts.fmt(this, Texts.HYPEROS_FAILED, Texts.translated(this, HyperOs.tool(vendor, Texts.column(this)))))',
        'names.add(new java.util.Locale(code).getDisplayLanguage(new java.util.Locale(code)));':
            'names.add(java.util.Locale.forLanguageTag(code).getDisplayName(java.util.Locale.forLanguageTag(code)));',
    }
    activity = patch(result, 'LauncherActivity.java', edits)
    text = activity.read_text(encoding='utf-8')
    text = text.rstrip().removesuffix('}') + '\n' + (ROOT / 'android/launcher/launch_checks.java.inc').read_text(encoding='utf-8') + '\n}\n'
    activity.write_text(text, encoding='utf-8', newline='\n')
    return activity


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
    anchor = '    caps.null_descriptor = r2.nullDescriptor;'
    assert text.count(anchor) == 1
    # What Frostpunk's renderers ask beyond the shared report: FrostpunkDevice reads these lines.
    text = text.replace(anchor, anchor + '''
    int astc = f2.features.textureCompressionASTC_LDR;
    const VkFormat astc_formats[] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK,
        VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    for (unsigned i = 0; i < 4; ++i) {
        VkFormatProperties fp; formats(chosen, astc_formats[i], &fp);
        astc = astc && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    }
    snprintf(text, sizeof text, "%d", astc); port_line(r, prefix, "astc", text);
    int etc2 = f2.features.textureCompressionETC2;
    const VkFormat etc2_formats[] = {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK,
        VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK};
    for (unsigned i = 0; i < 4; ++i) {
        VkFormatProperties fp; formats(chosen, etc2_formats[i], &fp);
        etc2 = etc2 && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    }
    snprintf(text, sizeof text, "%d", etc2); port_line(r, prefix, "etc2", text);
    snprintf(text, sizeof text, "%u", f2.features.textureCompressionBC); port_line(r, prefix, "bcFeature", text);
    snprintf(text, sizeof text, "%u", p.deviceType); port_line(r, prefix, "gpuType", text);
    snprintf(text, sizeof text, "%08x", p.driverVersion); port_line(r, prefix, "driverVersion", text);
    snprintf(text, sizeof text, "%u", r2.robustBufferAccess2); port_line(r, prefix, "robustBufferAccess2", text);
    snprintf(text, sizeof text, "%u", r2.nullDescriptor); port_line(r, prefix, "nullDescriptor", text);
    snprintf(text, sizeof text, "%u", v12.descriptorBindingPartiallyBound); port_line(r, prefix, "partiallyBound", text);
    VkFormatProperties bgra; formats(chosen, VK_FORMAT_B8G8R8A8_UNORM, &bgra);
    snprintf(text, sizeof text, "%d", !!(bgra.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT));
    port_line(r, prefix, "bgra", text);
    /* The native Vulkan renderer (android/vulkan/lib/device.cpp): core 1.3 features and the capabilities
     * its SPIR-V 1.6 shader library declares. */
    VkPhysicalDeviceShaderDrawParametersFeatures draw;
    memset(&draw, 0, sizeof draw);
    draw.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    if (features2 && p.apiVersion >= VK_API_VERSION_1_1) {
        VkPhysicalDeviceFeatures2 query;
        memset(&query, 0, sizeof query);
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        query.pNext = &draw;
        features2(chosen, &query);
    }
    port_line(r, prefix, "dynamicRendering", v13.dynamicRendering ? "1" : "0");
    port_line(r, prefix, "synchronization2", v13.synchronization2 ? "1" : "0");
    port_line(r, prefix, "vulkanMemoryModel", v12.vulkanMemoryModel ? "1" : "0");
    port_line(r, prefix, "shaderDrawParameters", draw.shaderDrawParameters ? "1" : "0");
''')
    path = work / 'probe/vulkan_probe.c'
    shutil.rmtree(path.parent, ignore_errors=True)
    shutil.copytree(source.parent, path.parent)
    path.write_text(text, encoding='utf-8')
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
    # The launcher's FPS overlay (port.json app.performanceHud) above the game surface and the touch layer,
    # inside the touch subwindow (Vulkan owns NativeActivity's own window surface; addContentView is not drawn):
    # null while the player leaves the counter off, so nothing is created or sampled then.
    edits = {
        'private TextView status;': 'private ' + PACKAGE + '.launcher.FpsOverlay fpsOverlay;\n    private TextView status;',
        '            controls.attach(this);\n':
            '            controls.attach(this);\n'
            '            fpsOverlay=' + PACKAGE + '.launcher.FpsOverlay.create(this, TouchControls::frameTimes);\n'
            '            if(fpsOverlay!=null){controls.addOverlay(fpsOverlay);fpsOverlay.start();}\n',
        '    @Override protected void onPause() {\n':
            '    @Override protected void onResume() {\n        super.onResume();\n        if(fpsOverlay!=null)fpsOverlay.start();\n    }\n'
            '    @Override protected void onPause() {\n        if(fpsOverlay!=null)fpsOverlay.stop();\n',
    }
    for before, after in edits.items():
        assert text.count(before) == 1, before
        text = text.replace(before, after)
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
