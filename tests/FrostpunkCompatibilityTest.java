package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.ContextWrapper;
import java.io.File;
import java.util.*;
import org.json.JSONObject;

/** Renderer rows of the launcher on a real device: every mode of port.json stays listed, an unusable one
 *  carries one reason, an old system Vulkan does not block Turnip, and the port's own driver rules hold.
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
        need(!FrostpunkDevice.mesaGrallocLinear(context, ""), "unknown devices use the driver default");
        String gralloc = FrostpunkDevice.MESA_GRALLOC_LINEAR_OPTION;
        need(State.put(context, State.CHOICES, gralloc, "0"), "save the explicit off setting");
        need(!FrostpunkDevice.mesaGrallocLinear(context, "KINGKONG_X_PRO"), "explicit off overrides CUBOT's default");
        need(State.put(context, State.CHOICES, gralloc, "1"), "save the explicit on setting");
        need(FrostpunkDevice.mesaGrallocLinear(context, "TB373FU"), "explicit on overrides Lenovo's default");
        need(State.update(context, State.CHOICES, values -> values.remove(gralloc)), "restore the default gralloc setting");
        JSONObject translations = new JSONObject(Spec.asset(context, "launcher/compatibility.json"));
        Spec spec = Spec.get(context); Spec.Mode turnip = spec.mode("turnip"), stock = spec.mode("dxvk");
        Spec.Distribution game = spec.distributions.get(0);
        Spec.Mode nativeVulkan = spec.mode(FrostpunkHooks.NATIVE_RENDERER);
        Spec.Mode nativeTurnip = spec.mode(FrostpunkHooks.NATIVE_TURNIP_RENDERER);
        Spec.Mode panvk = spec.mode(FrostpunkHooks.NATIVE_PANVK_RENDERER);
        Spec.Mode dxvkPanvk = spec.mode(FrostpunkHooks.DXVK_PANVK_RENDERER);
        need(panvk.minSdk == 30 && dxvkPanvk.minSdk == 30, "PanVK can be checked starting at Frostpunk's minimum Android version");
        need(spec.performanceHud && "0".equals(spec.option("fps_counter").value), "the FPS overlay is off on a fresh install");
        need(HudField.read(context, "0") == 0, "the first enable shows the FPS number only");
        for (Spec.Mode mode : Arrays.asList(nativeVulkan, nativeTurnip, panvk))
            need(FrostpunkDevice.nativeMode(mode) && mode.readsCache() && !mode.needsBC, mode.id + " is a native mode that reads the cache");
        for (Spec.Mode mode : Arrays.asList(stock, turnip, dxvkPanvk))
            need(!FrostpunkDevice.nativeMode(mode) && mode.readsCache(), mode.id + " is a DXVK mode that reads the cache");
        need(Probe.turnipMode(spec) == nativeTurnip && Probe.panvkMode(spec) == panvk, "the driver check does not depend on DXVK libraries");

        /* Every row stays listed; a row that cannot run here says why in one reason. */
        Gpu gpu = Gpu.get(context);
        File probe = new File(State.folder(context), Probe.REPORT);
        List<Renderers.Status> visible = Renderers.evaluate(context, spec, game, gpu, false);
        need(visible.size() == game.renderers.size(), "no renderer row is filtered out");
        need(visible.get(0).mode == nativeVulkan && spec.defaultMode.equals(nativeVulkan.id), "native system Vulkan is the default");
        for (Renderers.Status status : visible)
            need(status.available || (status.reason != null && !status.reason.trim().isEmpty() && !status.reason.contains("\n")),
                status.mode.id + " names one reason");
        if (gpu.qualcomm()) {
            need(!Renderers.status(visible, panvk.id).available && !Renderers.status(visible, dxvkPanvk.id).available, "PanVK rows are disabled on Adreno");
        } else if (gpu.probed) {
            need(!Renderers.status(visible, nativeTurnip.id).available && !Renderers.status(visible, turnip.id).available, "Turnip rows are disabled off Adreno");
        }

        /* The driver family of each mode. */
        Map<String, String> panvkReport = Probe.parse(nativeReport("panvk", "13b5", "9", "1.3.300"));
        need(!Renderers.judge(context, panvk, panvkReport).available, "a system driver cannot impersonate PanVK");
        need(!Renderers.judge(context, dxvkPanvk, panvkReport).available, "DXVK PanVK also verifies the driver");
        panvkReport.put("panvk.driverId", "20");
        need(Renderers.judge(context, panvk, panvkReport).verified, "Mesa PanVK is accepted");
        need(Renderers.judge(context, dxvkPanvk, panvkReport).verified && FrostpunkDevice.dxt(panvkReport, dxvkPanvk),
            "DXVK PanVK can use sampled BC originals");
        need(FrostpunkDevice.dxt(panvkReport, panvk), "native PanVK can read reported BC originals");
        panvkReport.put("panvk.formats", "4");
        need(!FrostpunkDevice.dxt(panvkReport, panvk), "the per-format mask overrides the aggregate BC flag");
        need(Renderers.judge(context, panvk, panvkReport).verified, "ASTC or ETC2 covers a driver without DXT");
        panvkReport.put("panvk.astc", "0"); panvkReport.put("panvk.etc2", "0");
        Renderers.Status noFormat = Renderers.judge(context, panvk, panvkReport);
        need(!noFormat.available && noFormat.reason.equals(FrostpunkDevice.text(context, "no_texture_format")), "no readable texture format disables the row");
        panvkReport.remove("panvk.formats"); panvkReport.put("panvk.astc", "1"); panvkReport.put("panvk.etc2", "1");
        panvkReport.put("panvk.bc", "101");
        need(!FrostpunkDevice.dxt(panvkReport, panvk), "incomplete PanVK BC formats need a conversion");
        panvkReport.put("panvk.dynamicRendering", "0");
        Renderers.Status noFeature = Renderers.judge(context, panvk, panvkReport);
        need(!noFeature.available && noFeature.reason.contains("dynamicRendering"), "a missing native requirement is named");
        need(Renderers.judge(context, dxvkPanvk, panvkReport).available, "the native requirements do not apply to DXVK");

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
        for (Spec.Mode mode : Arrays.asList(turnip, nativeTurnip)) {
            Renderers.Status status = Renderers.status(legacy, mode.id);
            need(status != null && status.available && status.verified, mode.id + " accepts Turnip over system Vulkan 1.1");
        }
        for (Spec.Mode mode : Arrays.asList(stock, nativeVulkan)) {
            Renderers.Status status = Renderers.status(legacy, mode.id);
            need(status != null && !status.available && status.reason.contains("1.1.128"), mode.id + " names the system driver's version");
        }
        need(nativeTurnip.id.equals(Renderers.effective(spec.defaultMode, legacy, spec.defaultMode, false)), "the start uses native Turnip on Adreno");
        need("Vulkan 1.3.335 (Turnip)".equals(RendererInfo.title(nativeTurnip.kind, nativeTurnip.title, "Vulkan (system driver)", 0, legacyAdreno.vulkan, Probe.report(context))),
            "the row title carries the selected driver's measured version");
        Probe.write(probe, legacyStock + nativeReport("turnip", "5143", "18", "1.1.128").replace("turnip.status=ok", "turnip.status=missing")
            .replace("turnip.missing=", "turnip.missing=vulkan13"));
        legacy = Renderers.evaluate(context, spec, game, legacyAdreno, false);
        for (Spec.Mode mode : Arrays.asList(turnip, nativeTurnip)) {
            Renderers.Status status = Renderers.status(legacy, mode.id);
            need(status != null && !status.available && status.reason.contains("1.1.128"), mode.id + " checks the Turnip API");
        }
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
        for (String token : FrostpunkDevice.NATIVE_REQUIRES)
            need(real.containsKey("stock." + token), "the driver check reports " + token);
        for (String token : new String[]{"astc", "etc2", "bgra", "formats", "nullDescriptor", "api", "device"})
            need(real.containsKey("stock." + token), "the driver check reports " + token);
        Probe.write(probe, Probe.key(context) + "\n" + live);
        if (bundledPanvk.isFile()) {
            String bundledKey = Probe.key(context);
            VulkanDriver.PANVK.install(context, null, bundledPanvk);
            File selectedPanvk = VulkanDriver.PANVK.chosen(context);
            need(selectedPanvk != null && selectedPanvk.getParentFile().equals(VulkanDriver.PANVK.folder(context))
                && VulkanDriver.TURNIP.chosen(context) == null, "PanVK has independent storage");
            need(Probe.report(context) == null, "the imported library invalidates the bundled probe report");
            try {
                VulkanDriver.PANVK.install(context, null, probe);
                throw new AssertionError("the driver picker accepted a text file");
            } catch (java.io.IOException expected) {
                need(selectedPanvk.equals(VulkanDriver.PANVK.chosen(context)), "a rejected import preserves the selection");
            }
            VulkanDriver.PANVK.reset(context);
            need(Probe.key(context).equals(bundledKey) && Probe.report(context) != null,
                "reset returns to the bundled PanVK and its probe identity");
        }
        List<Renderers.Status> actualStatuses = Renderers.evaluate(context, spec, game, gpu, false);
        need(actualStatuses.size() == game.renderers.size(), "the real report keeps every row");
        String effective = Renderers.effective(spec.defaultMode, actualStatuses, spec.defaultMode, false);
        System.out.println("DEVICE: " + gpu.renderer + "; system Vulkan " + Gpu.version(gpu.vulkan) + "; start uses " + effective);
        for (Renderers.Status status : actualStatuses) {
            need(status.available || (status.reason != null && !status.reason.trim().isEmpty()), status.mode.id + " has a reason on the real report");
            List<String> formats = new ArrayList<>(spec.textureFormats);
            formats.addAll(spec.etc2Formats); formats.addAll(spec.astcFormats);
            System.out.println("ROW: " + RendererInfo.title(status.mode.kind, status.mode.title, "Vulkan (system driver)", gpu.gles, gpu.vulkan, Probe.report(context))
                + " | " + (status.available ? status.verified ? "available" : "available, not confirmed" : "disabled: " + status.reason)
                + (status.available ? " | " + RendererInfo.textures(context, TextureFormats.vulkan(Probe.report(context), status.mode.driverKey()), formats).replace('\n', ' ') : ""));
            for (FrostpunkDevice.Workaround item : FrostpunkDevice.workarounds(context, status.mode, Probe.report(context)))
                if (status.available) System.out.println("  WORKAROUND: " + item.key + (item.toggle ? " (switch, " + (item.on ? "on" : "off") + ")" : " (always)"));
        }
        System.out.println(live);

        /* Fifteen languages of the port's own reasons and of the shared rows. */
        Map<String, String> report = new HashMap<>();
        report.put("turnip.status", "missing"); report.put("turnip.vendor", "5143");
        report.put("turnip.api", "1.0.335"); report.put("turnip.bcFeature", "1"); report.put("turnip.bc", "111");
        report.put("turnip.missing", "vulkan13,robustBufferAccess2,nullDescriptor,multiViewport");
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
            Renderers.Status rejected = Renderers.judge(context, turnip, report);
            need(!rejected.available && !rejected.verified, "old Turnip rejected");
            need(rejected.reason.contains("1.0.335") && rejected.reason.contains("multiViewport"), "API and feature details");
            need(!rejected.reason.contains(words.getString("driver_turnip")), "a missing driverId does not hide the API failure");
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
        report.put("turnip.status", "ok"); report.put("turnip.missing", ""); report.put("turnip.api", "1.3.335");
        report.put("turnip.bgra", "1"); report.put("turnip.astc", "1");
        report.put("turnip.driverId", "9");
        need(!Renderers.judge(context, turnip, report).available, "a driver that is not Turnip is rejected");
        report.put("turnip.driverId", "18");
        Renderers.Status accepted = Renderers.judge(context, turnip, report);
        need(accepted.available && accepted.verified, "verified Turnip accepted");
        report.putAll(Probe.parse(nativeReport("turnip", "5143", "18", "1.3.335")));
        report.putAll(Probe.parse(nativeReport("stock", "5143", "8", "1.3.300")));
        Renderers.Status qualcomm = Renderers.judge(context, nativeVulkan, report);
        need(Renderers.judge(context, nativeTurnip, report).verified && !qualcomm.available
            && qualcomm.reason.equals(FrostpunkDevice.text(context, "driver_qualcomm")), "the Qualcomm system driver is a disabled row with its reason");
        report.put("turnip.bcFeature", "0"); report.put("turnip.bc", "000"); report.put("turnip.formats", "0");
        need(Renderers.judge(context, turnip, report).available, "Turnip without BC reads the converted cache");
        report.put("stock.status", "missing"); report.put("stock.vendor", "13b5"); report.put("stock.driverId", "9");
        report.put("stock.partiallyBound", "1"); report.put("stock.robustBufferAccess2", "0");
        report.put("stock.missing", "bcTextures,fillModeNonSolid,shaderClipDistance,shaderCullDistance,multiViewport,robustness2,robustBufferAccess2,nullDescriptor");
        need(Renderers.judge(context, stock, report).verified, "Mali port fallbacks retained");
        need(Renderers.judge(context, nativeVulkan, report).verified, "native Vulkan shares the system driver on Mali");
        boolean maliFix = false, standIn = false;
        report.put("stock.nullDescriptor", "0");
        for (FrostpunkDevice.Workaround item : FrostpunkDevice.workarounds(context, nativeVulkan, report)) {
            maliFix |= item.key.equals("fix_mali_dynamic_state") && item.toggle && item.on;
            standIn |= item.key.equals("null_descriptor") && !item.toggle;
        }
        need(maliFix && standIn, "the Mali workarounds are listed for the native renderer");
        need(spec.option("fix_mali_dynamic_state") != null && spec.option("fix_powervr_compiler") != null, "the switches reach the game as PORT_OPTION_*");
        report.put("stock.missing", report.get("stock.missing") + ",geometryShader");
        need(!Renderers.judge(context, stock, report).available, "unhandled missing feature still blocks Mali");
        report.put("stock.status", "crash"); need(!Renderers.judge(context, stock, report).available, "probe crash blocks startup");
        State.put(context, State.CHOICES, "language", "ru"); Locale.setDefault(Locale.JAPANESE);
        need(FrostpunkDevice.language(context).equals("ru"), "explicit launcher language wins");
        State.put(context, State.CHOICES, "option.native_vulkan", "1");
        FrostpunkHooks hooks = new FrostpunkHooks();
        hooks.migrate(context);
        need(FrostpunkHooks.NATIVE_RENDERER.equals(Renderers.chosen(context, spec, game, hooks)), "existing toggle migrates to the renderer list");
        for (Spec.Mode mode : Arrays.asList(nativeVulkan, nativeTurnip, panvk, dxvkPanvk))
            need(FrostpunkHooks.watchedRenderer(mode.id), "startup failure reporting covers " + mode.id);
        need(FrostpunkHooks.nativeTwin("turnip").equals(nativeTurnip.id) && FrostpunkHooks.nativeTwin("dxvk").equals(nativeVulkan.id)
            && FrostpunkHooks.nativeTwin(dxvkPanvk.id).equals(panvk.id), "each DXVK mode has its native twin");
        System.out.println("PASS: renderer rows with reasons, driver policy, Turnip over old system Vulkan, workarounds and all "
            + translations.length() + " languages");
    }
}
