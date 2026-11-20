package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.ContextWrapper;
import java.io.File;
import java.util.*;
import org.json.JSONObject;

public final class FrostpunkCompatibilityTest {
    static void need(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    static String launchProblem(Context context, Spec spec, Gpu gpu) throws Exception {
        LauncherActivity activity = new LauncherActivity();
        java.lang.reflect.Method attach = ContextWrapper.class.getDeclaredMethod("attachBaseContext", Context.class);
        attach.setAccessible(true); attach.invoke(activity, context);
        for (String name : new String[]{"spec", "gpu"}) {
            java.lang.reflect.Field field = LauncherActivity.class.getDeclaredField(name); field.setAccessible(true);
            field.set(activity, name.equals("spec") ? spec : gpu);
        }
        java.lang.reflect.Method problem = LauncherActivity.class.getDeclaredMethod("fpDeviceProblem", Spec.Distribution.class);
        problem.setAccessible(true);
        return (String) problem.invoke(activity, spec.distributions.get(0));
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
        JSONObject translations = new JSONObject(Spec.asset(context, "launcher/compatibility.json"));
        Spec spec = Spec.get(context); Spec.Mode turnip = spec.mode("turnip"), stock = spec.mode("dxvk");
        Spec.Mode nativeVulkan = spec.mode(FrostpunkHooks.NATIVE_RENDERER);
        Spec.Mode panvk = spec.mode(FrostpunkHooks.NATIVE_PANVK_RENDERER);
        Spec.Mode dxvkPanvk = spec.mode(FrostpunkHooks.DXVK_PANVK_RENDERER);
        need(panvk.minSdk == 30 && dxvkPanvk.minSdk == 30, "PanVK can be checked starting at Frostpunk's minimum Android version");
        need(FrostpunkDevice.applies(true, "turnip") && !FrostpunkDevice.applies(true, "vulkan"), "Adreno uses only Turnip");
        need(!FrostpunkDevice.applies(false, "turnip") && FrostpunkDevice.applies(false, "vulkan"), "other GPUs use only system Vulkan");
        Gpu gpu = Gpu.get(context);
        for (String name : Arrays.asList("Mali-G52", "mali-g76", "Mali-G615", "Mali-G715", "Immortalis-G925"))
            need(Gpu.maliRenderer(name), "PanVK can be checked on " + name);
        need(!Gpu.maliRenderer("Adreno 750") && !Gpu.maliRenderer(null), "other GPUs do not match Mali renderer names");
        need(FrostpunkDevice.applies(gpu, panvk, Probe.parse("stock.vendor=13b5\n")), "ARM Vulkan devices can check PanVK");
        List<Renderers.Status> visible = Renderers.evaluate(context, spec, spec.distributions.get(0), gpu, false);
        boolean supportsPanvk = gpu.mali();
        need(visible.size() == (supportsPanvk ? 4 : 2) && visible.get(0).mode.id.equals(gpu.qualcomm() ? "turnip" : "dxvk")
            && visible.get(1).mode == nativeVulkan, "native Vulkan is listed alongside the device's DXVK mode");
        if (supportsPanvk) need(visible.get(2).mode == panvk && visible.get(3).mode == dxvkPanvk, "both PanVK renderers are listed");
        Map<String, String> panvkReport = Probe.parse("panvk.status=ok\npanvk.vendor=13b5\npanvk.driverId=9\n"
            + "panvk.astc=1\npanvk.bgra=1\npanvk.robustBufferAccess2=1\npanvk.bcFeature=1\npanvk.bc=111\n");
        need(!Renderers.judge(context, panvk, panvkReport).available, "a system driver cannot impersonate PanVK");
        need(!Renderers.judge(context, dxvkPanvk, panvkReport).available, "DXVK PanVK also verifies the driver");
        panvkReport.put("panvk.driverId", "20");
        need(Renderers.judge(context, panvk, panvkReport).verified, "Mesa PanVK with ASTC is accepted");
        need(Renderers.judge(context, dxvkPanvk, panvkReport).verified && !FrostpunkDevice.dxt(panvkReport, dxvkPanvk.id),
            "DXVK PanVK uses the verified driver and existing ASTC data");
        need(!FrostpunkDevice.dxt(panvkReport, panvk.id), "PanVK keeps the Mali ASTC texture data");
        List<Renderers.Status> converted = Renderers.evaluate(context, spec, spec.distributions.get(0), gpu, true);
        need(converted.get(0).available == visible.get(0).available && converted.get(0).verified == visible.get(0).verified,
            "native ASTC data does not invalidate the port renderer");
        need(launchProblem(context, spec, gpu) != null, "Play card reports a pending driver check before any report exists");
        String driver = gpu.qualcomm() ? "turnip" : "stock";
        File probe = new File(State.folder(context), Probe.REPORT);
        Probe.write(probe, Probe.key(context) + "\n" + driver + ".status=missing\n" + driver + ".api=1.0.335\n"
            + driver + ".missing=vulkan13,multiViewport\n");
        String oldApi = !gpu.qualcomm() && gpu.vulkan != 0 && Integer.compareUnsigned(gpu.vulkan, stock.vulkan) < 0
            ? Gpu.version(gpu.vulkan) : "1.0.335";
        need(launchProblem(context, spec, gpu).contains(oldApi), "Play card explains insufficient Vulkan without launching the game");
        need(probe.delete(), "remove synthetic driver report");

        String libraries = app.getApplicationInfo().nativeLibraryDir;
        System.load(new File(libraries, "libc++_shared.so").getAbsolutePath());
        System.load(new File(libraries, Probe.FILE).getAbsolutePath());
        java.lang.reflect.Method nativeProbe = Probe.class.getDeclaredMethod("nativeProbe", String.class, String.class, boolean.class);
        nativeProbe.setAccessible(true);
        String live = (String) nativeProbe.invoke(null, libraries, null, false);
        if (gpu.qualcomm()) live += (String) nativeProbe.invoke(null, libraries, turnip.driver, false);
        File bundledPanvk = new File(libraries, panvk.driver);
        if (FrostpunkDevice.applies(gpu, panvk, Probe.parse(live)) && bundledPanvk.isFile()) {
            String result = (String) nativeProbe.invoke(null, libraries, panvk.driver, true);
            live += result;
            Map<String, String> checked = Probe.parse(result);
            String status = checked.get("panvk.status");
            need(Arrays.asList("ok", "missing", "error").contains(status), "the probe reports the PanVK result");
            if (Gpu.pageSize() != 4096)
                need("error".equals(status) && checked.get("panvk.error").contains("4 KiB"), "bundled PanVK requires 4 KiB pages");
            if ("ok".equals(status)) need("20".equals(checked.get("panvk.driverId")), "a passing probe uses Mesa PanVK");
        }
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
        List<Renderers.Status> actualStatuses = Renderers.evaluate(context, spec, spec.distributions.get(0), gpu, false);
        Renderers.Status actual = actualStatuses.get(0);
        String problem = launchProblem(context, spec, gpu);
        need((problem == null) == actualStatuses.stream().anyMatch(s -> s.available && s.verified), "Play card agrees with the real driver check");
        if (problem != null) need(problem.contains(actual.reason), "Play card retains the actual rejection details");
        System.out.println("DEVICE: " + gpu.renderer + "; system Vulkan " + Gpu.version(gpu.vulkan)
            + "; selected " + actual.mode.id + "; available=" + actual.available + "; verified=" + actual.verified);
        System.out.println(live);
        if (problem != null) System.out.println("PLAY: " + problem);
        Map<String, String> report = new HashMap<>();
        report.put("turnip.status", "missing"); report.put("turnip.vendor", "5143");
        report.put("turnip.api", "1.0.335"); report.put("turnip.bcFeature", "1"); report.put("turnip.bc", "111");
        report.put("turnip.missing", "vulkan13,robustBufferAccess2,nullDescriptor,multiViewport");
        for (Iterator<String> languages = translations.keys(); languages.hasNext();) {
            String language = languages.next(); Locale.setDefault(Locale.forLanguageTag(language));
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
            need(!rejected.reason.contains("Mali") && !rejected.reason.contains("PowerVR"), "no unrelated driver message");
            need(!rejected.reason.contains(words.getString("driver_turnip")), "missing driverId does not hide API failure");
        }
        Locale.setDefault(Locale.ENGLISH);
        report.put("turnip.status", "ok"); report.put("turnip.missing", ""); report.put("turnip.api", "1.3.335");
        need(!Renderers.judge(context, turnip, report).available, "unverified driver still rejected");
        report.put("turnip.driverId", "18");
        Renderers.Status accepted = Renderers.judge(context, turnip, report);
        need(accepted.available && accepted.verified, "verified Turnip accepted");
        report.put("stock.vendor", "5143");
        need(Renderers.judge(context, nativeVulkan, report).verified, "native Vulkan shares the Adreno Turnip driver");
        need(Renderers.effective("dxvk", Arrays.asList(accepted), "dxvk", false).equals("turnip"), "default from wrong GPU family replaced");
        report.put("turnip.bcFeature", "0"); need(!Renderers.judge(context, turnip, report).available, "BC feature remains mandatory");
        report.put("stock.status", "missing"); report.put("stock.vendor", "13b5"); report.put("stock.driverId", "9");
        report.put("stock.astc", "1"); report.put("stock.bgra", "1"); report.put("stock.partiallyBound", "1");
        report.put("stock.missing", "bcTextures,fillModeNonSolid,shaderClipDistance,shaderCullDistance,multiViewport,robustness2,robustBufferAccess2,nullDescriptor");
        need(Renderers.judge(context, stock, report).verified, "Mali port fallbacks retained");
        need(Renderers.judge(context, nativeVulkan, report).verified, "native Vulkan shares the system driver on Mali");
        report.put("stock.missing", report.get("stock.missing") + ",geometryShader");
        need(!Renderers.judge(context, stock, report).available, "unhandled missing feature still blocks Mali");
        report.put("stock.status", "crash"); need(!Renderers.judge(context, stock, report).available, "probe crash blocks startup");
        State.put(context, State.CHOICES, "language", "ru"); Locale.setDefault(Locale.JAPANESE);
        need(FrostpunkDevice.language(context).equals("ru"), "explicit launcher language wins");
        State.put(context, State.CHOICES, "option.native_vulkan", "1");
        FrostpunkHooks hooks = new FrostpunkHooks();
        hooks.migrate(context);
        need(FrostpunkHooks.NATIVE_RENDERER.equals(Renderers.chosen(context, spec, spec.distributions.get(0), hooks)), "existing toggle migrates to the renderer list");
        need(FrostpunkHooks.fallbackToDxvk(context, "frostpunk") && !FrostpunkHooks.NATIVE_RENDERER.equals(
            Renderers.chosen(context, spec, spec.distributions.get(0), hooks)), "crash fallback stores DXVK");
        for (Spec.Mode panvkMode : Arrays.asList(panvk, dxvkPanvk)) {
            State.put(context, State.CHOICES, "renderer.frostpunk", panvkMode.id);
            need(FrostpunkHooks.fallbackToDxvk(context, "frostpunk") && "dxvk".equals(
                Renderers.chosen(context, spec, spec.distributions.get(0), hooks)), "PanVK crash switches to system DXVK");
        }
        System.out.println("PASS: GPU filtering, driver policy, fallback selection, failure details and all " + translations.length() + " languages");
    }
}
