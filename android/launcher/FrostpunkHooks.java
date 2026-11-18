package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.Intent;

/** Migrates the former Vulkan toggle and routes game starts through the crash watcher. */
public final class FrostpunkHooks implements PortHooks {
    public static final String NATIVE_RENDERER = "native_vulkan";
    public static final String NATIVE_PANVK_RENDERER = "native_vulkan_panvk";
    public static final String DXVK_PANVK_RENDERER = "dxvk_panvk";
    static final String ASKED_NATIVE = "asked.nativeVulkan";

    public static boolean panvkRenderer(String renderer) {
        return NATIVE_PANVK_RENDERER.equals(renderer) || DXVK_PANVK_RENDERER.equals(renderer);
    }

    public static boolean fallbackRenderer(String renderer) {
        return NATIVE_RENDERER.equals(renderer) || panvkRenderer(renderer);
    }

    @Override public void migrate(Context context) {
        if (State.get(context, State.CHOICES, "option.native_vulkan", null) == null) return;
        State.update(context, State.CHOICES, values -> {
            if ("1".equals(values.remove("option.native_vulkan"))) {
                values.setProperty("renderer.frostpunk", NATIVE_RENDERER);
                values.setProperty(ASKED_NATIVE, "1");
            }
        });
    }

    public static boolean fallbackToDxvk(Context context, String distribution) {
        if (distribution == null || distribution.isEmpty()) distribution = "frostpunk";
        final String key = "renderer." + distribution;
        java.util.Map<String, String> report = Probe.report(context);
        final String dxvk = report != null && Probe.vendor(report, "stock") == Probe.QUALCOMM ? "turnip" : "dxvk";
        return State.update(context, State.CHOICES, values -> {
            values.remove("option.native_vulkan");
            values.setProperty(ASKED_NATIVE, "1");
            if (fallbackRenderer(values.getProperty(key, NATIVE_RENDERER))) values.setProperty(key, dxvk);
        });
    }

    @Override public void prepare(Context context, String distribution, Intent intent) {
        String enabled = State.get(context, State.CHOICES, "option.crash_monitor", "1");
        boolean monitor = "1".equals(enabled) || "true".equalsIgnoreCase(enabled);
        if (!monitor && !fallbackRenderer(intent.getStringExtra(Launch.EXTRA_RENDERER))) return;
        String game = intent.getComponent() != null ? intent.getComponent().getClassName()
            : "com.frostpunk.staticrecomp.MainActivity";
        intent.setClassName(context.getPackageName(), "com.frostpunk.staticrecomp.GameWatchActivity");
        intent.putExtra("crash.game", game);
        intent.putExtra("crash.language", Texts.language(context));
        intent.putExtra("crash.rendererOnly", !monitor);
    }
}
