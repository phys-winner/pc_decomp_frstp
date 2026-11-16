package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.content.Intent;

/** Points Play at the crash watcher unless the player turned the monitor off. */
public final class FrostpunkHooks implements PortHooks {
    @Override public void prepare(Context context, String distribution, Intent intent) {
        String enabled = State.get(context, State.CHOICES, "option.crash_monitor", "1");
        if (!"1".equals(enabled) && !"true".equalsIgnoreCase(enabled)) return;
        String game = intent.getComponent() != null ? intent.getComponent().getClassName()
            : "com.frostpunk.staticrecomp.MainActivity";
        intent.setClassName(context.getPackageName(), "com.frostpunk.staticrecomp.GameWatchActivity");
        intent.putExtra("crash.game", game);
        intent.putExtra("crash.language", java.util.Locale.getDefault().getLanguage());
    }
}
