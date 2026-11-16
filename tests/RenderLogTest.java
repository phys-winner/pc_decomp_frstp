package com.frostpunk.staticrecomp;

import android.system.Os;
import java.io.File;

public final class RenderLogTest {
    private static void need(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        File log = CrashReport.renderLog();
        long bytes = log.length(), modified = log.lastModified();
        boolean existed = log.exists();
        Os.setenv("FROSTPUNK_RENDER_LOG", "/data/local/tmp/stale-render.log", true);
        CrashReport.configureRenderLog(false);
        need(Os.getenv("FROSTPUNK_RENDER_LOG") == null, "Disabled monitoring clears inherited log path");
        CrashReport.configureRenderLog(true);
        need(log.getAbsolutePath().equals(Os.getenv("FROSTPUNK_RENDER_LOG")), "Enabled monitoring sets public log path");
        need(log.getCanonicalPath().equals("/storage/emulated/0/Frostpunk/render.log"), "Expected shared storage log");
        need(log.getParentFile().isDirectory(), "Log directory exists");
        CrashReport.configureRenderLog(false);
        need(Os.getenv("FROSTPUNK_RENDER_LOG") == null, "Disabling after enabling clears log path");
        need(log.exists() == existed && log.length() == bytes && log.lastModified() == modified, "Configuration preserves existing log");
        System.out.println("RENDER_LOG_TEST PASS: public path, enabled/disabled/inherited environment, existing log preserved");
    }
}
