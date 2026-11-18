package com.frostpunk.staticrecomp.probe;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.ClipboardManager;
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
    private String say(String english, String russian) { return russian() ? russian : english; }

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

    private ApplicationExitInfo exitInfo() {
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
            String nativeReason = marker.lastModified() >= started ? CrashReport.tail(marker, 8192).trim() : "";
            ApplicationExitInfo info = exitInfo();
            if (quiet(info, nativeReason)) return false;
            if (nativeReason.isEmpty() && info == null && exitCode == 0) return false;
            boolean rendererFallback = FrostpunkHooks.fallbackRenderer(renderer);
            String title = say("The game crashed", "Игра завершилась с ошибкой");
            String fallback = "";
            if (rendererFallback) {
                title = FrostpunkHooks.panvkRenderer(renderer)
                    ? say("PanVK stopped", "Сбой PanVK") : say("Native Vulkan stopped", "Сбой нативного Vulkan");
                fallback = FrostpunkHooks.fallbackToDxvk(this, getIntent().getStringExtra(Launch.EXTRA_DISTRIBUTION))
                    ? say("The next game start will use DXVK.", "При следующем запуске игра будет использовать DXVK.")
                    : say("Choose DXVK in the launcher's Renderer section before restarting.",
                        "Перед запуском выберите DXVK в разделе рендерера лаунчера.");
                if (rendererOnly) {
                    shown = true;
                    new AlertDialog.Builder(this).setTitle(title)
                        .setMessage(startupError != null ? startupError + "\n\n" + fallback : fallback).setCancelable(false)
                        .setPositiveButton(android.R.string.ok, (dialog, which) -> close()).show();
                    return true;
                }
            }
            boolean ru = russian();
            String reason = startupError != null ? startupError : !nativeReason.isEmpty() ? nativeReason
                : exitCode != 0 ? (ru ? "Игра завершилась с кодом " : "Game exited with code ") + exitCode
                : info != null ? CrashReport.exitReason(info.getReason(), info.getStatus(), info.getDescription(), ru)
                : say("Game process stopped unexpectedly", "Процесс игры неожиданно завершился");
            StringBuilder summary = new StringBuilder(reason);
            if (info != null) summary.append("\n").append(say("Android exit reason: ", "Причина выхода Android: "))
                .append(info.getReason()).append(", status: ").append(info.getStatus());
            StringBuilder text = new StringBuilder("Frostpunk crash report\nTime: ").append(java.time.Instant.now());
            try {
                text.append("\nVersion: ").append(getPackageManager().getPackageInfo(getPackageName(), 0).versionName);
            } catch (Exception ignored) { }
            text.append("\nDevice: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append("\nAndroid: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")")
                .append("\nRenderer: ").append(String.valueOf(renderer))
                .append("\n\n").append(summary);
            if (info != null) text.append("\nLast PSS (KiB): ").append(info.getPss()).append("\nLast RSS (KiB): ").append(info.getRss());
            for (String name : new String[] {"render.log", "native.log"}) {
                text.append("\n\n--- ").append(name).append(" ---\n");
                try { text.append(CrashReport.tail(name.equals("render.log") ? CrashReport.renderLog() : new File(getFilesDir(), name), 32768)); }
                catch (IOException error) { text.append(error.toString()); }
            }
            if (info != null) try (InputStream trace = info.getTraceInputStream()) {
                if (trace != null) {
                    ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                    byte[] buffer = new byte[4096];
                    while (bytes.size() < 65536) {
                        int count = trace.read(buffer, 0, Math.min(buffer.length, 65536 - bytes.size()));
                        if (count < 0) break;
                        bytes.write(buffer, 0, count);
                    }
                    text.append("\n\n--- Android trace ---\n").append(new String(bytes.toByteArray(), StandardCharsets.UTF_8));
                }
            } catch (IOException error) { text.append("\nAndroid trace: ").append(error); }
            String full = text.toString();
            File saved = new File(getFilesDir(), "crashes/frostpunk-" + System.currentTimeMillis() + ".txt");
            CrashReport.write(saved, full);
            if (session().exists() && !session().delete()) android.util.Log.w("Frostpunk", "Cannot retire crash session");
            shown = true;
            String visible = summary.length() > 1200 ? summary.substring(0, 1200) : summary.toString();
            visible += "\n\n" + say("Report saved: ", "Отчёт сохранён: ") + saved.getAbsolutePath();
            AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(title)
                .setMessage(rendererFallback ? fallback + "\n\n" + visible : visible)
                .setPositiveButton(say("Copy to clipboard", "Скопировать в буфер обмена"), null)
                .setNegativeButton(rendererFallback ? say("Launcher", "В лаунчер") : say("Exit", "Выйти"), (d, which) -> {
                    if (rendererFallback) close(); else finishAndRemoveTask();
                })
                .setCancelable(false).create();
            final String copied = full;
            dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                if (clipboard != null) clipboard.setPrimaryClip(ClipData.newPlainText("Frostpunk crash report", copied));
                Toast.makeText(this, say("Report copied", "Отчёт скопирован"), Toast.LENGTH_SHORT).show();
            }));
            dialog.show();
            return true;
        } catch (Exception error) {
            android.util.Log.e("Frostpunk", "crash report", error);
            return false;
        }
    }

    private void close() {
        if (session().exists() && !session().delete()) android.util.Log.w("Frostpunk", "Cannot retire crash session");
        Launch.openLauncher(this, getIntent().getStringExtra(Launch.EXTRA_DISTRIBUTION));
        finish();
    }
}
