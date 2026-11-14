package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.ContextWrapper;
import java.io.File;
import java.util.*;
import org.json.JSONObject;

public final class FrostpunkCompatibilityTest {
    static void need(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    public static void main(String[] args) throws Exception {
        need(args.length == 1 && args[0].startsWith("/data/local/tmp/frostpunk-compatibility-"), "isolated test folder");
        final File root = new File(args[0]); need(root.mkdir(), "fresh test folder");
        android.os.Looper.prepare();
        Class<?> thread = Class.forName("android.app.ActivityThread");
        Object runtime = thread.getMethod("systemMain").invoke(null);
        Context system = (Context) thread.getMethod("getSystemContext").invoke(runtime);
        Context app = system.createPackageContext("com.frostpunk.staticrecomp", Context.CONTEXT_IGNORE_SECURITY);
        Context context = new ContextWrapper(app) { @Override public File getFilesDir() { return root; } };
        JSONObject translations = new JSONObject(Spec.asset(context, "launcher/compatibility.json"));
        Spec spec = Spec.get(context); Spec.Mode turnip = spec.mode("turnip"), stock = spec.mode("dxvk");
        need(FrostpunkDevice.applies(true, "turnip") && !FrostpunkDevice.applies(true, "vulkan"), "Adreno uses only Turnip");
        need(!FrostpunkDevice.applies(false, "turnip") && FrostpunkDevice.applies(false, "vulkan"), "other GPUs use only system Vulkan");
        Gpu gpu = Gpu.get(context);
        List<Renderers.Status> visible = Renderers.evaluate(context, spec, spec.distributions.get(0), gpu, false);
        need(visible.size() == 1 && visible.get(0).mode.id.equals(gpu.qualcomm() ? "turnip" : "dxvk"), "actual device exposes only its renderer");
        List<Renderers.Status> converted = Renderers.evaluate(context, spec, spec.distributions.get(0), gpu, true);
        need(converted.get(0).available == visible.get(0).available && converted.get(0).verified == visible.get(0).verified,
            "native ASTC data does not invalidate the port renderer");
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
        need(Renderers.effective("dxvk", Arrays.asList(accepted), "dxvk", false).equals("turnip"), "default from wrong GPU family replaced");
        report.put("turnip.bcFeature", "0"); need(!Renderers.judge(context, turnip, report).available, "BC feature remains mandatory");
        report.put("stock.status", "missing"); report.put("stock.vendor", "13b5"); report.put("stock.driverId", "9");
        report.put("stock.astc", "1"); report.put("stock.bgra", "1"); report.put("stock.partiallyBound", "1");
        report.put("stock.missing", "bcTextures,fillModeNonSolid,shaderClipDistance,shaderCullDistance,multiViewport,robustness2,robustBufferAccess2,nullDescriptor");
        need(Renderers.judge(context, stock, report).verified, "Mali port fallbacks retained");
        report.put("stock.missing", report.get("stock.missing") + ",geometryShader");
        need(!Renderers.judge(context, stock, report).available, "unhandled missing feature still blocks Mali");
        report.put("stock.status", "crash"); need(!Renderers.judge(context, stock, report).available, "probe crash blocks startup");
        State.put(context, State.CHOICES, "language", "ru"); Locale.setDefault(Locale.JAPANESE);
        need(FrostpunkDevice.language(context).equals("ru"), "explicit launcher language wins");
        System.out.println("PASS: GPU filtering, driver policy, fallback selection, failure details and all " + translations.length() + " languages");
    }
}
