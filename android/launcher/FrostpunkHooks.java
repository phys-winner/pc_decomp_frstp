package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.Intent;

/** Migrates the former Vulkan toggle and routes game starts through the crash watcher. */
public final class FrostpunkHooks implements PortHooks {
    public static final String NATIVE_RENDERER = "native_vulkan";
    public static final String NATIVE_TURNIP_RENDERER = "native_vulkan_turnip";
    public static final String NATIVE_PANVK_RENDERER = "native_vulkan_panvk";
    public static final String DXVK_PANVK_RENDERER = "dxvk_panvk";
    static final String ASKED_NATIVE = "asked.nativeVulkan";

    public static boolean panvkRenderer(String renderer) {
        return NATIVE_PANVK_RENDERER.equals(renderer) || DXVK_PANVK_RENDERER.equals(renderer);
    }

    /** The native Vulkan mode on the same driver as a transitional DXVK mode. */
    static String nativeTwin(String renderer) {
        return "turnip".equals(renderer) ? NATIVE_TURNIP_RENDERER : DXVK_PANVK_RENDERER.equals(renderer) ? NATIVE_PANVK_RENDERER : NATIVE_RENDERER;
    }

    public static boolean watchedRenderer(String renderer) {
        return NATIVE_RENDERER.equals(renderer) || NATIVE_TURNIP_RENDERER.equals(renderer) || panvkRenderer(renderer);
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

    @Override public void prepare(Context context, String distribution, Intent intent) {
        String enabled = State.get(context, State.CHOICES, "option.crash_monitor", "1");
        boolean monitor = "1".equals(enabled) || "true".equalsIgnoreCase(enabled);
        if (!monitor && !watchedRenderer(intent.getStringExtra(Launch.EXTRA_RENDERER))) return;
        String game = intent.getComponent() != null ? intent.getComponent().getClassName()
            : "com.frostpunk.staticrecomp.MainActivity";
        intent.setClassName(context.getPackageName(), "com.frostpunk.staticrecomp.GameWatchActivity");
        intent.putExtra("crash.game", game);
        intent.putExtra("crash.language", Texts.language(context));
        intent.putExtra("crash.rendererOnly", !monitor);
    }
}
