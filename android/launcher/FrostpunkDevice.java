package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import java.util.Map;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.Locale;
import org.json.JSONObject;

final class FrostpunkDevice {
    private static JSONObject translations;
    static boolean lowMemory(long totalBytes) {
        return totalBytes > 0 && totalBytes <= 15L * 1024 * 1024 * 1024 / 2;
    }
    static String language(Context context) {
        String chosen = State.get(context, State.CHOICES, "language", "");
        String code = chosen.isEmpty() ? Locale.getDefault().toLanguageTag() : chosen;
        if (code.startsWith("pt")) return "pt-BR";
        if (code.startsWith("zh")) return "zh-CN";
        code = code.split("[-_]")[0];
        return code.equals("uk") || code.equals("be") ? "ru" : code;
    }
    static synchronized String text(Context context, String key, Object... args) {
        try {
            if (translations == null) translations = new JSONObject(Spec.asset(context, "launcher/compatibility.json"));
            JSONObject words = translations.optJSONObject(language(context));
            if (words == null) words = translations.getJSONObject("en");
            return String.format(Locale.getDefault(), words.getString(key), args);
        } catch (java.io.IOException | org.json.JSONException failure) {
            throw new IllegalStateException("Missing compatibility translations", failure);
        }
    }
    static boolean applies(boolean qualcomm, String kind) {
        return qualcomm == "turnip".equals(kind);
    }
    static Set<String> missing(Map<String, String> report, String driver, boolean needsBC) {
        Set<String> tokens = new LinkedHashSet<>();
        for (String token : report.getOrDefault(driver + ".missing", "").split(","))
            if (!token.trim().isEmpty()) tokens.add(token.trim());
        if (!needsBC) tokens.remove("bcTextures");
        int vendor = Probe.vendor(report, driver);
        if (vendor == 0x13b5 || vendor == 0x1010)
            for (String feature : new String[]{"bcTextures", "fillModeNonSolid", "shaderClipDistance", "shaderCullDistance", "multiViewport"}) tokens.remove(feature);
        if (vendor == 0x13b5 && !yes(report, driver, "robustBufferAccess2") && yes(report, driver, "partiallyBound"))
            for (String feature : new String[]{"robustness2", "robustBufferAccess2", "nullDescriptor"}) tokens.remove(feature);
        return tokens;
    }
    static Renderers.Status judge(Context context, Spec.Mode mode, Map<String, String> report) {
        String driver = "turnip".equals(mode.kind) ? "turnip" : "stock";
        String status = report.getOrDefault(driver + ".status", "");
        if (!status.equals("ok") && !status.equals("missing")) {
            String reason = status.equals("crash") ? text(context, "crash")
                : status.equals("error") ? text(context, "error", report.getOrDefault(driver + ".error", "?"))
                : text(context, "unchecked");
            return new Renderers.Status(mode, false, false, reason);
        }
        Set<String> tokens = missing(report, driver, mode.needsBC);
        StringBuilder reason = new StringBuilder();
        if (tokens.remove("vulkan13")) reason.append(text(context, "api", report.getOrDefault(driver + ".api", "?")));
        if (!tokens.isEmpty()) {
            if (reason.length() != 0) reason.append("\n");
            reason.append(text(context, "features", String.join(", ", tokens)));
        }
        String problem = profileProblem(report, driver);
        // Probe omits driverId below Vulkan 1.2; report the concrete missing requirements.
        if (problem != null && !(problem.equals("driver") && reason.length() != 0 && !report.containsKey(driver + ".driverId"))) {
            if (reason.length() != 0) reason.append("\n");
            reason.append(text(context, problem.equals("driver") && driver.equals("turnip") ? "driver_turnip" : problem));
        }
        boolean accepted = reason.length() == 0;
        return new Renderers.Status(mode, accepted, accepted, accepted ? null : reason.toString());
    }
    static String tr(Context context, String english, String russian) {
        return "ru".equals(Texts.language(context)) ? russian : english;
    }
    static boolean yes(Map<String, String> report, String driver, String key) {
        return "1".equals(report.get(driver + "." + key));
    }
    static String profileProblem(Map<String, String> report, String driver) {
        if ("4".equals(report.get(driver + ".gpuType"))) return "software";
        int vendor = Probe.vendor(report, driver);
        String id = report.get(driver + ".driverId");
        if ("turnip".equals(driver)) {
            if (!"18".equals(id) || vendor != 0x5143) return "driver";
            if (!yes(report, driver, "bcFeature") || !"111".equals(report.get(driver + ".bc"))) return "bc";
        } else {
            if (!(vendor == 0x13b5 && "9".equals(id)) && !(vendor == 0x1010 && "7".equals(id))) return "driver";
            if (!yes(report, driver, "astc")) return "astc";
            if (!yes(report, driver, "bgra")) return "bgra";
            if (vendor == 0x13b5 && !yes(report, driver, "robustBufferAccess2") && !yes(report, driver, "partiallyBound"))
                return "descriptors";
        }
        return null;
    }
    static String explain(Context context, String problem) {
        return text(context, problem);
    }
    static boolean dxt(Map<String, String> report, String renderer) {
        String driver = "turnip".equals(renderer) ? "turnip" : "stock";
        return yes(report, driver, "bcFeature") && "111".equals(report.get(driver + ".bc"));
    }
    static boolean astc(Map<String, String> report, String renderer) {
        return yes(report, "turnip".equals(renderer) ? "turnip" : "stock", "astc");
    }
}
