package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.ContextWrapper;
import java.io.File;
import java.util.*;
import org.json.JSONObject;

/** Renderer rows of the launcher on a real device: the three native Vulkan modes stay listed, an unusable
 *  one carries one reason, an old system Vulkan does not block Turnip, earlier mode ids (also the removed
 *  DXVK ones) resolve to the native modes, and the port's own driver rules hold.
 *  Runs with app_process against the installed APK (build/port-sync-20261124/launcher/run_test.py). */
public final class FrostpunkCompatibilityTest {
    static void need(boolean ok, String why) { if (!ok) throw new AssertionError(why); }

    /** A driver report that satisfies the native Vulkan renderer. */
    static String nativeReport(String driver, String vendor, String driverId, String api) {
        StringBuilder text = new StringBuilder();
        for (String line : new String[]{"status=ok", "api=" + api, "vendor=" + vendor, "driverId=" + driverId, "bgra=1", "astc=1",
                "etc2=1", "bcFeature=1", "bc=111", "gpuType=1", "robustBufferAccess2=1", "missing="})
            text.append(driver).append('.').append(line).append('\n');
        for (String token : FrostpunkDevice.NATIVE_REQUIRES) text.append(driver).append('.').append(token).append("=1\n");
        return text.toString();
    }

    public static void main(String[] args) throws Exception {
        need(args.length == 1 && args[0].startsWith("/data/local/tmp/frostpunk-compatibility-"), "isolated test folder");
        final File root = new File(args[0]); need(root.mkdir(), "fresh test folder");
        android.os.Looper.prepareMainLooper();
        Class<?> thread = Class.forName("android.app.ActivityThread");
        Object runtime = thread.getMethod("systemMain").invoke(null);
        Context system = (Context) thread.getMethod("getSystemContext").invoke(runtime);
        Context app = system.createPackageContext("com.frostpunk.staticrecomp", Context.CONTEXT_IGNORE_SECURITY);
        Context context = new ContextWrapper(app) { @Override public File getFilesDir() { return root; } };
        need(FrostpunkDevice.mesaGrallocLinear(context, "KINGKONG_X_PRO"), "CUBOT uses linear gralloc by default");
        need(!FrostpunkDevice.mesaGrallocLinear(context, "TB373FU"), "Lenovo uses the driver default");
        String gralloc = FrostpunkDevice.MESA_GRALLOC_LINEAR_OPTION;
        need(State.put(context, State.CHOICES, gralloc, "0"), "save the explicit off setting");
        need(!FrostpunkDevice.mesaGrallocLinear(context, "KINGKONG_X_PRO"), "explicit off overrides CUBOT's default");
        need(State.update(context, State.CHOICES, values -> values.remove(gralloc)), "restore the default gralloc setting");
        JSONObject translations = new JSONObject(Spec.asset(context, "launcher/compatibility.json"));
        Spec spec = Spec.get(context);
        Spec.Distribution game = spec.distributions.get(0);
        Spec.Mode vulkan = spec.mode("vulkan"), turnip = spec.mode("vulkan-turnip"), panvk = spec.mode("vulkan-panvk");
        need(vulkan != null && turnip != null && panvk != null && spec.modes.size() == 3 && game.renderers.size() == 3,
            "the common ids vulkan, vulkan-turnip and vulkan-panvk are the only modes");
        need(vulkan.id.equals(FrostpunkHooks.NATIVE_RENDERER) && turnip.id.equals(FrostpunkHooks.NATIVE_TURNIP_RENDERER)
            && panvk.id.equals(FrostpunkHooks.NATIVE_PANVK_RENDERER), "the hooks use the common ids");
        for (String[] alias : new String[][]{{"native_vulkan", "vulkan"}, {"dxvk", "vulkan"}, {"native_vulkan_turnip", "vulkan-turnip"},
                {"turnip", "vulkan-turnip"}, {"native_vulkan_panvk", "vulkan-panvk"}, {"dxvk_panvk", "vulkan-panvk"}})
            need(spec.mode(alias[0]) == spec.mode(alias[1]), "the earlier id " + alias[0] + " is " + alias[1]);
        need(spec.performanceHud && "0".equals(spec.option("fps_counter").value), "the FPS overlay is off on a fresh install");
        need(HudField.read(context, "0") == 0, "the first enable shows the FPS number only");
        for (Spec.Mode mode : spec.modes) {
            need(FrostpunkDevice.nativeMode(mode) && mode.readsCache() && !mode.needsBC, mode.id + " is a native mode that reads the cache");
            need(FrostpunkTextures.rendererReads(mode.id, TextureBackend.ETC2) && FrostpunkTextures.rendererReads(mode.id, TextureBackend.ASTC),
                mode.id + " reads ETC2 and ASTC");
            for (String library : mode.libraries) need(!library.contains("dxvk"), mode.id + " needs no DXVK library");
        }
        need(!spec.etc2Formats.isEmpty() && !spec.astcFormats.isEmpty() && spec.textureFormats.contains("DXT5"), "DXT originals, ETC2 and ASTC are declared");

        /* A saved choice of a removed mode starts the matching native mode. */
        State.put(context, State.CHOICES, Renderers.key(game), "turnip");
        need("vulkan-turnip".equals(Renderers.explicitChoice(context, spec, game, new FrostpunkHooks())), "a saved DXVK (Turnip) choice is Vulkan (Turnip)");
        State.put(context, State.CHOICES, Renderers.key(game), "dxvk_panvk");
        need("vulkan-panvk".equals(Renderers.explicitChoice(context, spec, game, new FrostpunkHooks())), "a saved DXVK (PanVK) choice is Vulkan (PanVK)");
        State.update(context, State.CHOICES, values -> values.remove(Renderers.key(game)));

        /* Every row stays listed; a row that cannot run here says why in one reason. */
        Gpu gpu = Gpu.get(context);
        File probe = new File(State.folder(context), Probe.REPORT);
        List<Renderers.Status> visible = Renderers.evaluate(context, spec, game, gpu, false);
        need(visible.size() == 3 && visible.get(0).mode == vulkan && spec.defaultMode.equals("vulkan"), "no row is filtered; system Vulkan is the default");
        for (Renderers.Status status : visible)
            need(status.available || (status.reason != null && !status.reason.trim().isEmpty() && !status.reason.contains("\n")),
                status.mode.id + " names one reason");
        if (gpu.qualcomm()) need(!Renderers.status(visible, panvk.id).available, "the PanVK row is disabled on Adreno");
        else if (gpu.probed) need(!Renderers.status(visible, turnip.id).available, "the Turnip row is disabled off Adreno");

        /* The driver family of each mode, the formats and the native requirements. */
        Map<String, String> panvkReport = Probe.parse(nativeReport("panvk", "13b5", "9", "1.3.300"));
        need(!Renderers.judge(context, panvk, panvkReport).available, "a system driver cannot impersonate PanVK");
        panvkReport.put("panvk.driverId", "20");
        need(Renderers.judge(context, panvk, panvkReport).verified && FrostpunkDevice.dxt(panvkReport, panvk), "Mesa PanVK is accepted and reads BC originals");
        panvkReport.put("panvk.formats", "4");
        need(!FrostpunkDevice.dxt(panvkReport, panvk), "the per-format mask overrides the aggregate BC flag");
        need(Renderers.judge(context, panvk, panvkReport).verified, "ASTC or ETC2 covers a driver without DXT");
        panvkReport.put("panvk.astc", "0"); panvkReport.put("panvk.etc2", "0");
        Renderers.Status noFormat = Renderers.judge(context, panvk, panvkReport);
        need(!noFormat.available && noFormat.reason.equals(FrostpunkDevice.text(context, "no_texture_format")), "no readable texture format disables the row");
        panvkReport.remove("panvk.formats"); panvkReport.put("panvk.astc", "1");
        panvkReport.put("panvk.dynamicRendering", "0");
        Renderers.Status noFeature = Renderers.judge(context, panvk, panvkReport);
        need(!noFeature.available && noFeature.reason.contains("dynamicRendering"), "a missing native requirement is named");

        /* An old system Vulkan never blocks Turnip: the selected driver's own version decides. */
        java.lang.reflect.Constructor<Gpu> gpuConstructor = Gpu.class.getDeclaredConstructor();
        gpuConstructor.setAccessible(true);
        Gpu legacyAdreno = gpuConstructor.newInstance();
        legacyAdreno.adreno = 650; legacyAdreno.renderer = "Adreno (TM) 650"; legacyAdreno.vendor = "Qualcomm"; legacyAdreno.probed = true;
        legacyAdreno.vulkan = (1 << 22) | (1 << 12);
        String legacyStock = Probe.key(context) + "\nstock.status=missing\nstock.vendor=5143\nstock.device=Adreno (TM) 650\n"
            + "stock.api=1.1.128\nstock.missing=vulkan13\n";
        Probe.write(probe, legacyStock + nativeReport("turnip", "5143", "18", "1.3.335"));
        List<Renderers.Status> legacy = Renderers.evaluate(context, spec, game, legacyAdreno, false);
        Renderers.Status overOld = Renderers.status(legacy, turnip.id), old = Renderers.status(legacy, vulkan.id);
        need(overOld.available && overOld.verified, "Turnip is accepted over system Vulkan 1.1");
        need(!old.available && old.reason.contains("1.1.128"), "the system row names the system driver's version");
        need(turnip.id.equals(Renderers.effective(spec.defaultMode, legacy, spec.defaultMode, false)), "the start uses Vulkan (Turnip) on Adreno");
        need("Vulkan 1.3.335 (Turnip)".equals(RendererInfo.title(turnip.kind, turnip.title, "Vulkan (system driver)", 0, legacyAdreno.vulkan, Probe.report(context))),
            "the row title carries the selected driver's measured version");
        Probe.write(probe, legacyStock + nativeReport("turnip", "5143", "18", "1.1.128"));
        Renderers.Status oldTurnip = Renderers.status(Renderers.evaluate(context, spec, game, legacyAdreno, false), turnip.id);
        need(!oldTurnip.available && oldTurnip.reason.contains("1.1.128"), "the Turnip row checks the Turnip API");
        need(probe.delete(), "remove the synthetic driver report");

        /* The real drivers of this device. */
        String libraries = app.getApplicationInfo().nativeLibraryDir;
        System.load(new File(libraries, "libc++_shared.so").getAbsolutePath());
        System.load(new File(libraries, Probe.FILE).getAbsolutePath());
        java.lang.reflect.Method nativeProbe = Probe.class.getDeclaredMethod("nativeProbe", String.class, String.class, boolean.class);
        nativeProbe.setAccessible(true);
        String live = (String) nativeProbe.invoke(null, libraries, null, false);
        if (gpu.qualcomm()) live += (String) nativeProbe.invoke(null, libraries, turnip.driver, false);
        File bundledPanvk = new File(libraries, panvk.driver);
        long pageSize = android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
        if (Probe.mali(context, Probe.parse(live)) && bundledPanvk.isFile()) {
            String result = (String) nativeProbe.invoke(null, libraries, panvk.driver, true);
            live += result;
            Map<String, String> checked = Probe.parse(result);
            String status = checked.get("panvk.status");
            need(Arrays.asList("ok", "missing", "error").contains(status), "the probe reports the PanVK result");
            if (pageSize != 4096)
                need("error".equals(status) && checked.get("panvk.error").contains("4 KiB"), "bundled PanVK requires 4 KiB pages");
            if ("ok".equals(status)) need("20".equals(checked.get("panvk.driverId")), "a passing probe uses Mesa PanVK");
        }
        Map<String, String> real = Probe.parse(live);
        for (String token : FrostpunkDevice.NATIVE_REQUIRES) need(real.containsKey("stock." + token), "the driver check reports " + token);
        for (String token : new String[]{"astc", "etc2", "bgra", "formats", "nullDescriptor", "api", "device"})
            need(real.containsKey("stock." + token), "the driver check reports " + token);
        Probe.write(probe, Probe.key(context) + "\n" + live);
        List<Renderers.Status> actualStatuses = Renderers.evaluate(context, spec, game, gpu, false);
        String effective = Renderers.effective(spec.defaultMode, actualStatuses, spec.defaultMode, false);
        System.out.println("DEVICE: " + gpu.renderer + "; system Vulkan " + Gpu.version(gpu.vulkan) + "; start uses " + effective);
        List<String> formats = new ArrayList<>(spec.textureFormats);
        formats.addAll(spec.etc2Formats); formats.addAll(spec.astcFormats);
        for (Renderers.Status status : actualStatuses) {
            need(status.available || (status.reason != null && !status.reason.trim().isEmpty()), status.mode.id + " has a reason on the real report");
            System.out.println("ROW: " + RendererInfo.title(status.mode.kind, status.mode.title, "Vulkan (system driver)", gpu.gles, gpu.vulkan, Probe.report(context))
                + " | " + (status.available ? status.verified ? "available" : "available, not confirmed" : "disabled: " + status.reason)
                + (status.available ? " | " + RendererInfo.textures(context, TextureFormats.vulkan(Probe.report(context), status.mode.driverKey()), formats).replace('\n', ' ') : ""));
            for (FrostpunkDevice.Workaround item : FrostpunkDevice.workarounds(context, status.mode, Probe.report(context)))
                if (status.available) System.out.println("  WORKAROUND: " + item.key + (item.toggle ? " (switch, " + (item.on ? "on" : "off") + ")" : " (always)"));
        }
        System.out.println(live);

        /* Fifteen languages of the port's own reasons and of the shared rows. */
        JSONObject words15 = new JSONObject(Spec.asset(context, "launcher/translations.json"));
        Set<String> rowTexts = new HashSet<>();
        for (Iterator<String> languages = translations.keys(); languages.hasNext();) {
            String language = languages.next(); Locale.setDefault(Locale.forLanguageTag(language));
            State.put(context, State.CHOICES, "language", language); Texts.reset();
            need(Texts.language(context).equals(language), "the launcher speaks " + language);
            JSONObject words = translations.getJSONObject(language);
            for (Iterator<String> keys = words.keys(); keys.hasNext();) {
                String key = keys.next(), value = words.getString(key);
                int placeholders = value.split("%s", -1).length - 1;
                Object[] values = new Object[placeholders]; Arrays.fill(values, "TEST");
                need(FrostpunkDevice.text(context, key, values).equals(String.format(value, values)), language + ": " + key);
            }
            for (int id = 0; id <= Texts.LAST; ++id) need(!Texts.t(context, id).isEmpty() || Texts.english(id).isEmpty(), language + ": text " + id);
            for (HudField field : HudField.values()) need(!Texts.pick(context, field.title).isEmpty(), language + ": " + field);
            String sample = Texts.t(context, Texts.TEX_COMPARE) + Texts.pick(context, HudField.GRAPH.title) + Texts.t(context, Texts.HYPEROS_NEVER)
                + Texts.translated(context, HyperOs.advice(HyperOs.XIAOMI, Texts.column(context)));
            need(rowTexts.add(sample), language + " has its own texts for the texture note, the FPS dialog and the game-mode tip");
            if (!Arrays.asList("en", "ru", "de", "pl").contains(language))
                need(words15.getJSONObject(language).has(Texts.english(Texts.TEX_COMPARE)), language + ": translations.json covers the shared base");
        }
        State.update(context, State.CHOICES, values -> values.remove("language")); Texts.reset();
        Locale.setDefault(Locale.ENGLISH);

        Map<String, String> report = new HashMap<>(Probe.parse(nativeReport("turnip", "5143", "9", "1.3.335")));
        need(!Renderers.judge(context, turnip, report).available, "a driver that is not Turnip is rejected");
        report.put("turnip.driverId", "18");
        need(Renderers.judge(context, turnip, report).verified, "verified Turnip accepted");
        /* The Qualcomm system driver is judged like any other (owner's decision of 24 November 2026). */
        report.putAll(Probe.parse(nativeReport("stock", "5143", "8", "1.3.128")));
        Renderers.Status qualcomm = Renderers.judge(context, vulkan, report);
        need(qualcomm.available && qualcomm.verified && FrostpunkDevice.dxt(report, vulkan), "a Qualcomm system driver with Vulkan 1.3 and BC runs the originals");
        report.put("stock.driverVersion", "802fa028");
        String qcom = "fix_qualcomm_shader_interface";
        need(FrostpunkDevice.workarounds(context, vulkan, report).stream().noneMatch(item -> item.key.equals(qcom)),
            "an untested Qualcomm driver version has no shader interface switch");
        report.put("stock.driverVersion", "802fa029");
        need(FrostpunkDevice.workarounds(context, vulkan, report).stream().anyMatch(item -> item.key.equals(qcom) && item.toggle && item.on),
            "the affected driver version enables the shader interface workaround");
        need(FrostpunkDevice.workarounds(context, turnip, report).stream().noneMatch(item -> item.key.equals(qcom)),
            "the system driver workaround is absent from the Turnip row");
        need(State.put(context, State.CHOICES, "option." + qcom, "0"), "save the explicit shader interface workaround setting");
        need(FrostpunkDevice.workarounds(context, vulkan, report).stream().anyMatch(item -> item.key.equals(qcom) && !item.on),
            "the launcher can turn off the shader interface workaround");
        State.update(context, State.CHOICES, values -> values.remove("option." + qcom));
        need(spec.option(qcom) != null && "1".equals(spec.option(qcom).value), "the workaround is exported enabled by default");
        report.put("stock.bcFeature", "0"); report.put("stock.bc", "000"); report.put("stock.formats", "0");
        qualcomm = Renderers.judge(context, vulkan, report);
        need(qualcomm.available && !FrostpunkDevice.dxt(report, vulkan) && FrostpunkDevice.astc(report, vulkan), "a Qualcomm system driver without BC reads the converted cache");
        report.put("stock.synchronization2", "0");
        qualcomm = Renderers.judge(context, vulkan, report);
        need(!qualcomm.available && qualcomm.reason.contains("synchronization2") && !qualcomm.reason.contains("\n"), "a Qualcomm system driver without a native requirement names it");
        report.put("stock.synchronization2", "1"); report.put("stock.bcFeature", "1"); report.put("stock.bc", "111"); report.remove("stock.formats");
        report.put("turnip.bcFeature", "0"); report.put("turnip.bc", "000"); report.put("turnip.formats", "0");
        need(Renderers.judge(context, turnip, report).available, "Turnip without BC reads the converted cache");
        report.put("stock.vendor", "13b5"); report.put("stock.driverId", "9"); report.put("stock.nullDescriptor", "0");
        need(Renderers.judge(context, vulkan, report).verified, "native Vulkan runs on the Mali system driver");
        boolean maliFix = false, standIn = false;
        for (FrostpunkDevice.Workaround item : FrostpunkDevice.workarounds(context, vulkan, report)) {
            maliFix |= item.key.equals("fix_mali_dynamic_state") && item.toggle && item.on;
            standIn |= item.key.equals("null_descriptor") && !item.toggle;
        }
        need(maliFix && standIn, "the Mali workarounds are listed for the native renderer");
        need(spec.option("fix_mali_dynamic_state") != null && spec.option("fix_powervr_compiler") != null, "the switches reach the game as PORT_OPTION_*");
        report.put("stock.status", "crash"); need(!Renderers.judge(context, vulkan, report).available, "a probe crash blocks startup");
        State.put(context, State.CHOICES, "language", "ru"); Locale.setDefault(Locale.JAPANESE);
        need(FrostpunkDevice.language(context).equals("ru"), "explicit launcher language wins");
        State.put(context, State.CHOICES, "option.native_vulkan", "1");
        FrostpunkHooks hooks = new FrostpunkHooks();
        hooks.migrate(context);
        need("vulkan".equals(Renderers.chosen(context, spec, game, hooks)), "the earlier toggle migrates to the renderer list");
        System.out.println("PASS: common mode ids with aliases, renderer rows with reasons, Turnip over old system Vulkan, workarounds and all "
            + translations.length() + " languages");
    }
}
