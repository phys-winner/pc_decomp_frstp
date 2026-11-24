package com.frostpunk.staticrecomp.probe;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.widget.Toast;
import com.frostpunk.staticrecomp.launcher.FrostpunkHooks;
import com.frostpunk.staticrecomp.launcher.Launch;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;

/** Separate process that outlives a native crash and shows why the game stopped. */
public final class GameWatchActivity extends Activity {
    private long started;
    private boolean returned, shown, rendererOnly;
    private String renderer;
    private int exitCode;
    private String startupError;

    private File crashFile() { return new File(getFilesDir(), "frostpunk-crash.txt"); }
    private File session() { return new File(getFilesDir(), "frostpunk-crash-session.txt"); }
    private boolean russian() {
        String language = getIntent().getStringExtra("crash.language");
        if (language == null) language = java.util.Locale.getDefault().getLanguage();
        return language.startsWith("ru");
    }
    private String say(String english, String russian) { return russian() ? russian : Launch.translated(this, english); }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) {
            started = state.getLong("started");
            returned = state.getBoolean("returned");
            exitCode = state.getInt("exitCode");
            startupError = state.getString("startupError");
            renderer = state.getString("renderer");
            rendererOnly = state.getBoolean("rendererOnly");
            return;
        }
        try {
            String[] previous = CrashReport.tail(session(), 128).trim().split("\n", 3);
            if (!previous[0].isEmpty()) {
                started = Long.parseLong(previous[0]);
                renderer = previous.length > 1 ? previous[1] : null;
                rendererOnly = previous.length == 3 && previous[2].equals("1");
                if (report()) return;
            }
        } catch (IOException | NumberFormatException error) {
            android.util.Log.e("Frostpunk", "crash monitor storage", error);
        }
        started = System.currentTimeMillis();
        renderer = getIntent().getStringExtra(Launch.EXTRA_RENDERER);
        rendererOnly = getIntent().getBooleanExtra("crash.rendererOnly", false);
        try { CrashReport.write(session(), started + "\n" + (renderer == null ? "" : renderer) + "\n" + (rendererOnly ? "1" : "0")); }
        catch (IOException error) { android.util.Log.e("Frostpunk", "crash monitor storage", error); }
        String gameClass = getIntent().getStringExtra("crash.game");
        if (gameClass == null || gameClass.isEmpty()) gameClass = "com.frostpunk.staticrecomp.MainActivity";
        Intent game = new Intent().setClassName(this, gameClass);
        if (getIntent().getExtras() != null) game.putExtras(getIntent().getExtras());
        game.putExtra("crash.watched", true);
        startActivityForResult(game, 1);
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putLong("started", started);
        state.putBoolean("returned", returned);
        state.putInt("exitCode", exitCode);
        state.putString("startupError", startupError);
        state.putString("renderer", renderer);
        state.putBoolean("rendererOnly", rendererOnly);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 1) return;
        returned = true;
        exitCode = data != null ? data.getIntExtra("crash.exitCode", 0) : 0;
        startupError = data != null ? data.getStringExtra("crash.startupError") : null;
        if (result == RESULT_OK && exitCode == 0) { close(); return; }
        new Handler().postDelayed(() -> {
            if (!isFinishing() && !report()) close();
        }, 600);
    }

    @Override protected void onResume() {
        super.onResume();
        if (returned && !shown) new Handler().postDelayed(() -> {
            if (!isFinishing() && !shown && !report()) close();
        }, 650);
    }

    /** Android records exit reasons from API 30. Below it the native journal of app.c
     *  ("Native signal …" in frostpunk-crash.txt) and the Java handler's text are the only sources. */
    private ApplicationExitInfo exitInfo() {
        if (Build.VERSION.SDK_INT < 30) return null;
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (manager == null) return null;
        for (ApplicationExitInfo info : manager.getHistoricalProcessExitReasons(getPackageName(), 0, 16))
            if (getPackageName().equals(info.getProcessName()) && info.getTimestamp() >= started) return info;
        return null;
    }

    private boolean quiet(ApplicationExitInfo info, String nativeReason) {
        if (nativeReason != null && !nativeReason.isEmpty()) return false;
        if (exitCode != 0 || info == null) return false;
        int reason = info.getReason();
        return reason == ApplicationExitInfo.REASON_USER_REQUESTED
            || reason == ApplicationExitInfo.REASON_USER_STOPPED
            || reason == ApplicationExitInfo.REASON_PACKAGE_UPDATED
            || reason == ApplicationExitInfo.REASON_PACKAGE_STATE_CHANGE
            || (reason == ApplicationExitInfo.REASON_EXIT_SELF && info.getStatus() == 0);
    }

    private boolean report() {
        if (shown) return true;
        try {
            File marker = crashFile();
            String markerText = "";
            if (marker.lastModified() >= started && Files.isRegularFile(marker.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                try (InputStream input = Files.newInputStream(marker.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                    markerText = new String(readTracePrefix(input, 8192), StandardCharsets.UTF_8).trim();
                } catch (IOException error) { android.util.Log.w("Frostpunk", "Cannot read crash marker", error); }
            }
            ApplicationExitInfo info = exitInfo();
            if (quiet(info, markerText)) return false;
            if (markerText.isEmpty() && info == null && exitCode == 0) return false;
            boolean rendererWatched = FrostpunkHooks.watchedRenderer(renderer);
            String title = say("The game crashed", "Игра завершилась с ошибкой");
            String fallback = "";
            if (rendererWatched) {
                title = FrostpunkHooks.panvkRenderer(renderer)
                    ? say("PanVK stopped", "Сбой PanVK") : say("Native Vulkan stopped", "Сбой нативного Vulkan");
                fallback = say("Open Renderer in the launcher to select a driver or retry this one.",
                    "В разделе рендеринга лаунчера можно выбрать драйвер или повторить попытку с текущим.");
                if (rendererOnly) {
                    shown = true;
                    new AlertDialog.Builder(this).setTitle(title)
                        .setMessage(startupError != null ? startupError + "\n\n" + fallback : fallback).setCancelable(false)
                        .setPositiveButton(android.R.string.ok, (dialog, which) -> close()).show();
                    return true;
                }
            }
            boolean ru = russian();
            // The signal line of the native journal is the last resort: Android's own reason says more when it exists.
            boolean signalMarker = markerText.startsWith("Native signal ");
            String reason = startupError != null ? startupError : !markerText.isEmpty() && !signalMarker ? markerText
                : exitCode != 0 ? (ru ? "Игра завершилась с кодом " : "Game exited with code ") + exitCode
                : info != null ? CrashReport.exitReason(info.getReason(), info.getStatus(), info.getDescription(), ru)
                : signalMarker ? CrashReport.signalReason(markerText, ru)
                : say("Game process stopped unexpectedly", "Процесс игры неожиданно завершился");
            String stackTrace = markerText.startsWith("Java ") ? CrashReport.shorten(markerText, 4000) : "";
            String reasonSummary = reason;
            if (reason.startsWith("Java ")) {
                int header = reason.indexOf('\n');
                int exception = header < 0 ? -1 : reason.indexOf('\n', header + 1);
                if (exception >= 0) reasonSummary = reason.substring(0, exception);
            }
            String exitDetails = info == null ? "" : "\n" + say("Android exit reason: ", "Причина выхода Android: ")
                + info.getReason() + ", status: " + info.getStatus();
            StringBuilder text = new StringBuilder("Frostpunk crash report\nTime: ").append(java.time.Instant.now());
            try {
                text.append("\nVersion: ").append(getPackageManager().getPackageInfo(getPackageName(), 0).versionName);
            } catch (Exception ignored) { }
            text.append("\nDevice: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append("\nAndroid: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")")
                .append("\nBuild fingerprint: ").append(Build.FINGERPRINT)
                .append("\nRenderer: ").append(String.valueOf(renderer)).append("\n\n");
            String visibleSummary = CrashReport.shorten(reasonSummary, 1200) + exitDetails;
            StringBuilder clipboard = new StringBuilder(text.toString()).append(visibleSummary);
            text.append(reason).append(exitDetails);
            if (signalMarker && info != null) text.append("\nNative journal: ").append(markerText);
            if (info != null) {
                String memory = "\nLast PSS (KiB): " + info.getPss() + "\nLast RSS (KiB): " + info.getRss();
                text.append(memory);
                clipboard.append(memory);
            }
            String reportName = "crashes/frostpunk-" + System.currentTimeMillis() + ".txt";
            File privateReport = new File(getFilesDir(), reportName);
            File saved = new File(CrashReport.logDirectory(getFilesDir()), reportName);
            if (info != null) try (InputStream trace = info.getTraceInputStream()) {
                if (trace != null) {
                    boolean nativeCrash = info.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                        && Build.VERSION.SDK_INT >= 31;
                    if (nativeCrash) {
                        File tombstone = CrashReport.saveTombstone(saved, privateReport, trace);
                        String location = "\nAndroid tombstone: " + tombstone.getAbsolutePath();
                        text.append(location);
                        clipboard.append(location);
                        stackTrace = CrashReport.nativeStackTrace(tombstone);
                        String abort;
                        try (InputStream prefix = Files.newInputStream(tombstone.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                            abort = CrashReport.nativeAbortMessage(readTracePrefix(prefix, 65536));
                        }
                        if (!abort.isEmpty()) {
                            text.append("\nAbort message: ").append(abort);
                            clipboard.append("\nAbort message: ").append(CrashReport.shorten(abort, 800));
                        }
                    } else {
                        String traceText = new String(readTracePrefix(trace, 65536), StandardCharsets.UTF_8);
                        text.append("\n\n--- Android trace ---\n").append(traceText);
                        if (stackTrace.isEmpty()) stackTrace = CrashReport.shorten(traceText.trim(), 4000);
                    }
                }
            } catch (IOException error) {
                String traceError = "\nAndroid trace: " + error;
                text.append(traceError);
                clipboard.append(CrashReport.shorten(traceError, 320));
            }
            for (String name : new String[] {"render.log", "native.log"}) {
                text.append("\n\n--- ").append(name).append(" ---\n");
                try {
                    String log = CrashReport.logTail(getFilesDir(), name, 32768);
                    text.append(log);
                    CrashReport.appendClipboardLog(clipboard, name, log);
                } catch (IOException error) { text.append(error.toString()); }
            }
            String full = text.toString();
            try { CrashReport.write(saved, full); }
            catch (IOException unavailable) {
                if (saved.equals(privateReport)) throw unavailable;
                CrashReport.write(privateReport, full);
                saved = privateReport;
            }
            if (session().exists() && !session().delete()) android.util.Log.w("Frostpunk", "Cannot retire crash session");
            shown = true;
            String savedLocation = say("Report saved: ", "Отчёт сохранён: ") + saved.getAbsolutePath();
            String visible = visibleSummary + "\n\n" + savedLocation;
            clipboard.append("\n\n").append(savedLocation);
            if (!stackTrace.isEmpty()) clipboard.append("\n\n--- Stack trace ---\n").append(stackTrace);
            String copied = clipboard.toString();
            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(rendererWatched ? fallback + "\n\n" + visible : visible)
                .setPositiveButton(say("Copy to clipboard", "Скопировать в буфер обмена"), null)
                .setNegativeButton(rendererWatched ? say("Launcher", "В лаунчер") : say("Exit", "Выйти"), (d, which) -> {
                    if (rendererWatched) close(); else finishAndRemoveTask();
                })
                .setCancelable(false).create();
            dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                String message = CrashReport.copy(this, copied) ? say("Report copied", "Отчёт скопирован")
                    : say("Could not copy the report", "Не удалось скопировать отчёт");
                Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
            }));
            dialog.show();
            return true;
        } catch (Exception error) {
            android.util.Log.e("Frostpunk", "crash report", error);
            return false;
        }
    }

    private static byte[] readTracePrefix(InputStream trace, int limit) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        while (bytes.size() < limit) {
            int count = trace.read(buffer, 0, Math.min(buffer.length, limit - bytes.size()));
            if (count < 0) break;
            bytes.write(buffer, 0, count);
        }
        return bytes.toByteArray();
    }

    private void close() {
        if (session().exists() && !session().delete()) android.util.Log.w("Frostpunk", "Cannot retire crash session");
        Launch.openLauncher(this, getIntent().getStringExtra(Launch.EXTRA_DISTRIBUTION));
        finish();
    }
}
