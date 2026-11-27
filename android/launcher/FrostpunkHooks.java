package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.Intent;

/** Migrates the former Vulkan toggle and routes game starts through the crash watcher. */
public final class FrostpunkHooks implements PortHooks {
    public static final String NATIVE_RENDERER = "vulkan";
    public static final String NATIVE_TURNIP_RENDERER = "vulkan-turnip";
    public static final String NATIVE_PANVK_RENDERER = "vulkan-panvk";
    static final String ASKED_NATIVE = "asked.nativeVulkan";

    public static boolean panvkRenderer(String renderer) { return NATIVE_PANVK_RENDERER.equals(renderer); }

    /** Every mode is the native Vulkan renderer: a start that fails opens the crash report. */
    public static boolean watchedRenderer(String renderer) { return true; }

    /** The renderer failure of the distribution's last start (native renderer_failure.h: the driver did not load or
     *  start, the device was lost) that the launcher offers to answer with the next renderer: within the shared
     *  rules and not answered yet. The crash watcher then opens the launcher instead of its crash report. */
    public static boolean rendererFailureOffered(Context context, String distribution) {
        Spec spec = Spec.get(context);
        Spec.Distribution d = distribution != null ? spec.distribution(distribution) : null;
        return RendererFailure.pending(context, spec, d != null ? d : spec.distributions.get(0)) != null;
    }

    /** The reason of a renderer failure record outside those rules (a device loss after the window): null when none. */
    public static String rendererFailureReason(Context context, String distribution) {
        Spec spec = Spec.get(context);
        Spec.Distribution d = distribution != null ? spec.distribution(distribution) : null;
        RendererFailure failure = RendererFailure.last(context, spec, d != null ? d : spec.distributions.get(0));
        return failure == null ? null : failure.summary();
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
