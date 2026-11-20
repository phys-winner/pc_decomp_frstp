package com.frostpunk.staticrecomp.probe;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
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

    private static long varint(DataInputStream input) throws IOException {
        long value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            int part = input.readUnsignedByte();
            if (shift == 63 && part > 1) break;
            value |= (long) (part & 127) << shift;
            if (part < 128) return value;
        }
        throw new IOException("Invalid tombstone varint");
    }

    static String nativeAbortMessage(byte[] prefix) throws IOException {
        // Android debuggerd's Tombstone protobuf stores abort_message in field 14.
        DataInputStream input = new DataInputStream(new ByteArrayInputStream(prefix));
        while (input.available() != 0) {
            long tag = varint(input);
            long length;
            switch ((int) (tag & 7)) {
                case 0: varint(input); continue;
                case 1: length = 8; break;
                case 2: length = varint(input); break;
                case 5: length = 4; break;
                default: return "";
            }
            if (length < 0 || length > input.available()) return "";
            if (tag == ((14 << 3) | 2)) {
                byte[] message = new byte[(int) Math.min(length, 4096)];
                input.readFully(message);
                return new String(message, StandardCharsets.UTF_8).trim();
            }
            input.skipBytes((int) length);
        }
        return "";
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
