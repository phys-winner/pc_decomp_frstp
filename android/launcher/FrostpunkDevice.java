package com.frostpunk.staticrecomp.launcher;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import org.json.JSONObject;

/** What Frostpunk's renderers ask of a device beyond the shared launcher's rules, and the driver
 *  workarounds the player can see and switch. The shared Renderers class decides hardware, Android
 *  version, libraries and the driver check first; judge() then adds the port's own requirements, so a
 *  disabled row carries one reason. */
final class FrostpunkDevice {
    static final String MESA_GRALLOC_LINEAR_OPTION = "option.mesa_gralloc_linear";
    static final int QUALCOMM = 0x5143, ARM = 0x13b5, IMAGINATION = 0x1010;
    /** What the native Vulkan renderer needs besides its Vulkan version: tokens of the driver check
     *  (tools/android_launcher.py build_probe prints each as driver.token=0|1). */
    static final String[] NATIVE_REQUIRES = {
        "androidSurface", "swapchain", "graphicsQueue", "dynamicRendering", "synchronization2",
        "vulkanMemoryModel", "shaderDrawParameters",
    };
    private static JSONObject translations;

    static boolean mesaGrallocLinear(Context context, String device) {
        // The CUBOT display can corrupt PanVK's scaled AFBC window buffers.
        String fallback = "KINGKONG_X_PRO".equals(device) ? "1" : "0";
        return "1".equals(State.get(context, State.CHOICES, MESA_GRALLOC_LINEAR_OPTION, fallback));
    }

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
            // A text missing from the file must not close the launcher: these rows are drawn on every refresh.
            String pattern = words.optString(key, translations.getJSONObject("en").optString(key, ""));
            if (pattern.isEmpty()) {
                android.util.Log.e("FrostpunkLauncher", "compatibility text missing: " + key);
                return key;
            }
            return String.format(Locale.getDefault(), pattern, args);
        } catch (java.io.IOException | org.json.JSONException failure) {
            throw new IllegalStateException("Missing compatibility translations", failure);
        }
    }
    static String tr(Context context, String english, String russian) {
        return "ru".equals(Texts.language(context)) ? russian : Texts.translated(context, english);
    }
    static boolean yes(Map<String, String> report, String driver, String key) {
        return "1".equals(report.get(driver + "." + key));
    }
    static boolean nativeMode(Spec.Mode mode) { return mode.versionProbe(); }

    /** DXVK's missing-feature tokens the port's overlays work without on Mali and PowerVR. */
    static void forgive(Set<String> tokens, Map<String, String> report, String driver) {
        int vendor = Probe.vendor(report, driver);
        if (vendor == ARM || vendor == IMAGINATION)
            for (String feature : new String[]{"fillModeNonSolid", "shaderClipDistance", "shaderCullDistance", "multiViewport"}) tokens.remove(feature);
        if (vendor == ARM && !yes(report, driver, "robustBufferAccess2") && yes(report, driver, "partiallyBound"))
            for (String feature : new String[]{"robustness2", "robustBufferAccess2", "nullDescriptor"}) tokens.remove(feature);
    }

    /** The shared verdict, then Frostpunk's own: the driver family each renderer was written for and the
     *  formats and features it uses. An unavailable row keeps the shared reason. */
    static Renderers.Status judge(Context context, Spec.Mode mode, Map<String, String> report, Renderers.Status shared) {
        if (!shared.available) return shared;
        String driver = mode.driverKey();
        String status = report.getOrDefault(driver + ".status", "");
        if (!status.equals("ok") && !status.equals("missing")) return shared;
        String problem = profileProblem(report, mode);
        if (problem != null) return new Renderers.Status(mode, false, false, text(context, problem));
        if (nativeMode(mode)) {
            Set<String> missing = new LinkedHashSet<>();
            for (String token : NATIVE_REQUIRES) if (!yes(report, driver, token)) missing.add(token);
            if (!missing.isEmpty())
                return new Renderers.Status(mode, false, false, text(context, "features", String.join(", ", missing)));
        }
        return shared;
    }

    /** A key of the compatibility texts, or null. */
    static String profileProblem(Map<String, String> report, Spec.Mode mode) {
        String driver = mode.driverKey();
        if ("4".equals(report.get(driver + ".gpuType"))) return "software";
        int vendor = Probe.vendor(report, driver);
        String id = report.get(driver + ".driverId");
        if ("turnip".equals(driver)) {
            if (id != null && (!"18".equals(id) || vendor != QUALCOMM)) return "driver_turnip";
        } else if ("panvk".equals(driver)) {
            if (vendor != ARM || !"20".equals(id)) return "driver_panvk";
        } else if (vendor == QUALCOMM) {
            // The Qualcomm system driver: the port runs Adreno through Mesa Turnip only.
            return "driver_qualcomm";
        } else if (!nativeMode(mode) && !(vendor == ARM && "9".equals(id)) && !(vendor == IMAGINATION && "7".equals(id))) {
            return "driver";
        }
        if (!yes(report, driver, "bgra")) return "bgra";
        if (!nativeMode(mode) && vendor == ARM && !yes(report, driver, "robustBufferAccess2") && !yes(report, driver, "partiallyBound"))
            return "descriptors";
        Long caps = TextureFormats.vulkan(report, driver);
        if (caps != null && Boolean.FALSE.equals(TextureFormats.supports(caps, TextureFormats.DXT))
                && !yes(report, driver, "astc") && !yes(report, driver, "etc2")) return "no_texture_format";
        return null;
    }

    static boolean dxt(Map<String, String> report, Spec.Mode mode) {
        if (report == null || mode == null) return false;
        Boolean sampled = TextureFormats.supports(TextureFormats.vulkan(report, mode.driverKey()), TextureFormats.DXT);
        return sampled != null ? sampled : "111".equals(report.get(mode.driverKey() + ".bc"));
    }
    static boolean astc(Map<String, String> report, Spec.Mode mode) {
        return report != null && mode != null && yes(report, mode.driverKey(), "astc");
    }
    static boolean etc2(Map<String, String> report, Spec.Mode mode) {
        return report != null && mode != null && yes(report, mode.driverKey(), "etc2");
    }

    /** One driver workaround of the port: where it applies, whether the player may switch it. */
    static final class Workaround {
        final String key, title, hint;
        final boolean toggle, on;
        Workaround(String key, String title, String hint, boolean toggle, boolean on) {
            this.key = key; this.title = title; this.hint = hint; this.toggle = toggle; this.on = on;
        }
    }

    /** The workarounds that apply to the renderer and driver a start would use; empty when none does. */
    static List<Workaround> workarounds(Context context, Spec.Mode mode, Map<String, String> report) {
        List<Workaround> list = new ArrayList<>();
        if (mode == null || report == null || !mode.vulkanKind()) return list;
        String driver = mode.driverKey();
        int vendor = Probe.vendor(report, driver);
        boolean nativeRenderer = nativeMode(mode);
        if (nativeRenderer && vendor == ARM)
            list.add(new Workaround("fix_mali_dynamic_state",
                tr(context, "Mali: set dynamic state again in every pass", "Mali: повторно задавать динамическое состояние в каждом проходе"),
                tr(context, "The Mali-G615 driver lost cull mode and depth state between render passes: buildings and terrain flickered. Costs a few commands per pass.",
                    "Драйвер Mali-G615 терял режим отсечения и состояние глубины между проходами: мерцали здания и рельеф. Стоит нескольких команд на проход."),
                true, !"0".equals(State.get(context, State.CHOICES, "option.fix_mali_dynamic_state", "1"))));
        if (nativeRenderer && vendor == IMAGINATION)
            list.add(new Workaround("fix_powervr_compiler",
                tr(context, "PowerVR: compile shaders one at a time", "PowerVR: компилировать шейдеры по одному"),
                tr(context, "The PowerVR shader compiler crashed when several threads used it at once. Loading takes slightly longer.",
                    "Компилятор шейдеров PowerVR падал при одновременной работе из нескольких потоков. Загрузка идёт немного дольше."),
                true, !"0".equals(State.get(context, State.CHOICES, "option.fix_powervr_compiler", "1"))));
        if (nativeRenderer && !yes(report, driver, "nullDescriptor"))
            list.add(new Workaround("null_descriptor",
                tr(context, "Stand-in textures for empty shader slots", "Заглушки для пустых слотов шейдеров"),
                tr(context, "This driver has no null descriptors (VK_EXT_robustness2): the renderer binds small empty resources instead. Always on here.",
                    "У этого драйвера нет пустых дескрипторов (VK_EXT_robustness2): рендерер подставляет маленькие пустые ресурсы. Здесь включено всегда."),
                false, true));
        if ("turnip".equals(driver))
            list.add(new Workaround("turnip_tessellation",
                tr(context, "Turnip: hardware tessellation off", "Turnip: аппаратная тесселяция выключена"),
                tr(context, "Tessellated snow hung the GPU on Adreno with Turnip during development. Always on with Turnip.",
                    "Тесселяция снега приводила к зависанию GPU на Adreno с Turnip. С Turnip включено всегда."),
                false, true));
        if ("panvk".equals(driver))
            list.add(new Workaround("mesa_gralloc_linear",
                tr(context, "PanVK: linear window buffers", "PanVK: линейные буферы окна"),
                tr(context, "Asks Mesa for linear window buffers (MESA_GRALLOC_LINEAR). Fixes a corrupted picture on the CUBOT KingKong X Pro, where it is on by default.",
                    "Запрашивает у Mesa линейные буферы окна (MESA_GRALLOC_LINEAR). Исправляет испорченную картинку на CUBOT KingKong X Pro, где включено по умолчанию."),
                true, mesaGrallocLinear(context, Build.DEVICE)));
        return list;
    }

    /* ------------------------------------------------------ diagnostics */

    /** Manufacturer and model, for example "Xiaomi 2201117PG"; not a serial number. */
    static String deviceName() {
        String maker = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER.trim();
        String model = Build.MODEL == null ? "" : Build.MODEL.trim();
        if (maker.equalsIgnoreCase("unknown")) maker = "";
        if (model.isEmpty()) return maker.isEmpty() ? "unknown" : maker;
        if (maker.isEmpty() || model.toLowerCase(Locale.ROOT).startsWith(maker.toLowerCase(Locale.ROOT))) return model;
        return maker + " " + model;
    }

    /** ActivityManager availMem and totalMem, in mebibytes. */
    static String memory(Context context) {
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) return "unknown";
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(info);
        return ((info.availMem + (1L << 19)) >> 20) + " MiB free / " + ((info.totalMem + (1L << 19)) >> 20) + " MiB";
    }

    /** "zip" for a game played from its archive, "folder" otherwise. */
    static String installKind(Context context) {
        Spec spec = Spec.get(context);
        Spec.Distribution d = spec.distributions.get(0);
        return Install.mount(context, d, d.parts.get(0)).isEmpty() ? "folder" : "zip";
    }
}
