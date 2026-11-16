package com.frostpunk.staticrecomp.probe;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;

/** Tail and replace the short crash files shared by the game process and the watcher. */
final class CrashReport {
    private CrashReport() { }

    static File renderLog() {
        return new File(android.os.Environment.getExternalStorageDirectory(), "Frostpunk/render.log");
    }

    static void configureRenderLog(boolean monitored) throws IOException, android.system.ErrnoException {
        android.system.Os.unsetenv("FROSTPUNK_RENDER_LOG");
        if (!monitored) return;
        File file = renderLog(), parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create " + parent);
        android.system.Os.setenv("FROSTPUNK_RENDER_LOG", file.getAbsolutePath(), true);
    }

    static String tail(File file, int limit) throws IOException {
        if (file == null || !file.isFile() || limit <= 0) return "";
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long size = input.length();
            if (size <= 0) return "";
            long start = Math.max(0, size - limit);
            input.seek(start);
            byte[] data = new byte[(int) Math.min(limit, size - start)];
            input.readFully(data);
            String text = new String(data, StandardCharsets.UTF_8);
            int newline = start > 0 ? text.indexOf('\n') : -1;
            return newline >= 0 ? text.substring(newline + 1) : text;
        }
    }

    static void write(File file, String text) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create " + parent);
        File temporary = new File(parent, file.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        if (file.exists() && !file.delete()) throw new IOException("Cannot replace " + file.getName());
        if (!temporary.renameTo(file)) throw new IOException("Cannot store " + file.getName());
    }

    static String exitReason(int reason, int status, String description, boolean russian) {
        if (description != null && !description.trim().isEmpty()) return description.trim();
        switch (reason) {
        case 2:
            return (russian ? "Процесс игры завершён сигналом " : "Game process terminated by signal ")
                + (status == 11 ? "SIGSEGV" : status == 6 ? "SIGABRT" : status == 7 ? "SIGBUS" : status == 9 ? "SIGKILL" : Integer.toString(status));
        case 3:
            return russian ? "Android завершил игру из-за нехватки памяти" : "Android stopped the game because memory was low";
        case 4:
            return russian ? "Необработанная ошибка Java" : "Unhandled Java exception";
        case 5:
            return russian ? "Сбой в нативном коде игры" : "Crash in native game code";
        case 6:
            return russian ? "Игра перестала отвечать (ANR)" : "The game stopped responding (ANR)";
        default:
            return russian ? "Процесс игры неожиданно завершился" : "Game process stopped unexpectedly";
        }
    }
}
