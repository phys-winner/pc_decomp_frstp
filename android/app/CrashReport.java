package com.frostpunk.staticrecomp.probe;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Environment;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import java.io.ByteArrayInputStream;
import java.io.DataInput;
import java.io.DataInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Pattern;

/** Diagnostics shared by the game process and the crash watcher. */
final class CrashReport {
    private static final Pattern DIAGNOSTIC = Pattern.compile(
        "(?i)^(?:(?:error|fatal|warn(?:ing)?):|STOP\\b|RDVK_CRASH (?:signal=|fault\\b|lr\\b|#0[0-3]\\b)|FP_HANG(?:_STACK)?\\b"
        + "|FROSTPUNK_VULKAN_DRIVER\\b|(?:first present|present|acquire|(?:native vulkan )?swapchain create) result=-)"
        + "|\\b(?:VK_ERROR_[A-Z_]+|SIGSEGV|SIGABRT|SIGBUS|LOW_MEMORY|device lost|out of (?:device |host )?memory)\\b");
    private static final Pattern LOG_TIMESTAMP = Pattern.compile("^\\d+\\s+");

    private CrashReport() { }

    private static File sharedDirectory() {
        return new File(Environment.getExternalStorageDirectory(), "Frostpunk");
    }

    static void prepareDirectory(File directory) throws IOException {
        if (!directory.isDirectory()) directory.mkdirs();
        if (!Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS) || !directory.canWrite())
            throw new IOException("Cannot write to " + directory);
    }

    static File logDirectory(File privateFiles) {
        File directory = sharedDirectory();
        try {
            prepareDirectory(directory);
            return directory;
        } catch (IOException unavailable) {
            return privateFiles;
        }
    }

    static void configureLogs(File privateFiles, boolean monitored) throws ErrnoException {
        Os.unsetenv("FROSTPUNK_NATIVE_LOG_DIR");
        Os.unsetenv("FROSTPUNK_RENDER_LOG");
        File directory = logDirectory(privateFiles);
        if (!directory.equals(privateFiles)) Os.setenv("FROSTPUNK_NATIVE_LOG_DIR", directory.getAbsolutePath(), true);
        if (monitored) Os.setenv("FROSTPUNK_RENDER_LOG", new File(directory, "render.log").getAbsolutePath(), true);
    }

    static String logTail(File privateFiles, String name, int limit) throws IOException {
        File local = new File(privateFiles, name);
        File directory = sharedDirectory();
        File shared = new File(directory, name);
        if (Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS)
                && Files.isRegularFile(shared.toPath(), LinkOption.NOFOLLOW_LINKS)
                && shared.lastModified() >= local.lastModified()) {
            try { return tail(shared, limit); }
            catch (IOException unavailable) { if (!local.isFile()) throw unavailable; }
        }
        return tail(local, limit);
    }

    static String tail(File file, int limit) throws IOException {
        if (file == null || limit <= 0 || !Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) return "";
        try (FileChannel input = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long size = input.size();
            if (size <= 0) return "";
            long start = Math.max(0, size - limit);
            input.position(start);
            ByteBuffer data = ByteBuffer.allocate((int) (size - start));
            while (data.hasRemaining() && input.read(data) > 0) { }
            String text = new String(data.array(), 0, data.position(), StandardCharsets.UTF_8);
            int newline = start > 0 ? text.indexOf('\n') : -1;
            return newline >= 0 ? text.substring(newline + 1) : text;
        }
    }

    private static long varint(DataInput input) throws IOException {
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

    static String nativeStackTrace(File tombstone) throws IOException {
        if (!Files.isRegularFile(tombstone.toPath(), LinkOption.NOFOLLOW_LINKS)) return "";
        try (RandomAccessFile input = new RandomAccessFile(tombstone, "r")) {
            // debuggerd/proto/tombstone.proto: tid (6), threads (16), and their map entries (1, 2).
            long end = input.length(), tid = 0;
            ProtoFields fields = new ProtoFields(input, 0, end);
            for (int tag; (tag = fields.next()) != 0; ) {
                if (tag == (6 << 3)) { tid = fields.number; break; }
            }
            if (tid == 0) return "";
            fields = new ProtoFields(input, 0, end);
            for (int tag; (tag = fields.next()) != 0; ) {
                if (tag != ((16 << 3) | 2)) continue;
                ProtoFields entry = fields.message(), thread = null;
                long key = 0;
                for (int member; (member = entry.next()) != 0; ) {
                    if (member == (1 << 3)) key = entry.number;
                    if (member == ((2 << 3) | 2)) thread = entry.message();
                }
                if (key == tid && thread != null) return nativeThreadTrace(thread, tid);
            }
        }
        return "";
    }

    private static String nativeThreadTrace(ProtoFields thread, long tid) throws IOException {
        String name = "";
        StringBuilder frames = new StringBuilder();
        int count = 0;
        for (int tag; (tag = thread.next()) != 0; ) {
            if (tag == ((2 << 3) | 2)) name = thread.text(80);
            if (tag != ((4 << 3) | 2)) continue;
            if (count < 16) appendNativeFrame(frames, thread.message(), count);
            ++count;
        }
        if (count == 0) return "";
        if (count > 16) frames.append("\n… ").append(count - 16).append(" more frames (see tombstone)");
        return "tid=" + tid + (name.isEmpty() ? "" : " (" + name + ")") + frames;
    }

    private static void appendNativeFrame(StringBuilder text, ProtoFields frame, int index) throws IOException {
        long pc = 0, functionOffset = 0, fileOffset = 0;
        String file = "", function = "", buildId = "";
        for (int tag; (tag = frame.next()) != 0; ) {
            switch (tag) {
                case 1 << 3: pc = frame.number; break;
                case (4 << 3) | 2: function = frame.text(256); break;
                case 5 << 3: functionOffset = frame.number; break;
                case (6 << 3) | 2: file = frame.text(256); break;
                case 7 << 3: fileOffset = frame.number; break;
                case (8 << 3) | 2: buildId = frame.text(64); break;
                default: break;
            }
        }
        text.append(String.format(Locale.ROOT, "\n#%02d pc %016x  %s", index, pc, file.isEmpty() ? "<unknown>" : file));
        if (fileOffset != 0) text.append(" (offset 0x").append(Long.toHexString(fileOffset)).append(')');
        if (!function.isEmpty()) text.append(" (").append(function).append('+').append(functionOffset).append(')');
        if (!buildId.isEmpty()) text.append(" (BuildId: ").append(buildId).append(')');
    }

    private static final class ProtoFields {
        private final RandomAccessFile input;
        private final long end;
        private long position;
        long number;

        ProtoFields(RandomAccessFile input, long start, long end) {
            this.input = input;
            position = start;
            this.end = end;
        }

        int next() throws IOException {
            if (position == end) return 0;
            input.seek(position);
            long tag = varint(input), length;
            switch ((int) tag & 7) {
                case 0: number = varint(input); length = 0; break;
                case 1: length = 8; break;
                case 2: length = varint(input); break;
                case 5: length = 4; break;
                default: throw new IOException("Unsupported tombstone field");
            }
            long start = input.getFilePointer();
            if (tag < 8 || tag > 0xffffffffL || start > end || length < 0 || length > end - start)
                throw new IOException("Invalid tombstone field");
            position = start + length;
            return (int) tag;
        }

        ProtoFields message() throws IOException {
            return new ProtoFields(input, input.getFilePointer(), position);
        }

        String text(int limit) throws IOException {
            long length = position - input.getFilePointer();
            byte[] bytes = new byte[(int) Math.min(limit, length)];
            input.readFully(bytes);
            String value = new String(bytes, StandardCharsets.UTF_8).replace('\n', ' ').replace('\r', ' ');
            return length > limit ? value + "…" : value;
        }
    }

    static void write(File file, String text) throws IOException {
        write(file, new ByteArrayInputStream(text.getBytes(StandardCharsets.UTF_8)));
    }

    private static void write(File file, InputStream contents) throws IOException {
        File parent = file.getParentFile();
        prepareDirectory(parent);
        File temporary = File.createTempFile(".frostpunk-", ".tmp", parent);
        try {
            try (FileOutputStream out = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[16384];
                int count;
                while ((count = contents.read(buffer)) != -1) out.write(buffer, 0, count);
                out.getFD().sync();
            }
            Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(temporary.toPath());
        }
    }

    static File saveTombstone(File report, File privateReport, InputStream trace) throws IOException {
        File local = new File(privateReport.getPath() + ".pb");
        write(local, trace);
        if (report.equals(privateReport)) return local;

        File shared = new File(report.getPath() + ".pb");
        try (InputStream contents = Files.newInputStream(local.toPath())) {
            write(shared, contents);
        } catch (IOException unavailable) {
            return local;
        }
        if (!local.delete()) Log.w("Frostpunk", "Cannot remove private copy of exported tombstone");
        return shared;
    }

    static void appendClipboardLog(StringBuilder report, String name, String log) {
        String[] lines = log.split("\\R");
        Set<String> seen = new HashSet<>();
        StringBuilder excerpt = new StringBuilder();
        // shortcut: match known crash diagnostics, extend the pattern when a new log format matters.
        for (int i = lines.length - 1; i >= 0 && seen.size() < 8; i--) {
            String message = LOG_TIMESTAMP.matcher(lines[i].trim()).replaceFirst("");
            if (DIAGNOSTIC.matcher(message).find() && seen.add(message))
                excerpt.insert(0, shorten(lines[i], 320) + "\n");
        }
        if (excerpt.length() != 0) report.append("\n\n--- ").append(name).append(" ---\n").append(excerpt);
    }

    static String shorten(String text, int limit) {
        return text.length() <= limit ? text : text.substring(0, limit) + "…";
    }

    static boolean copy(Context context, String text) {
        try {
            ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
            if (clipboard == null) return false;
            clipboard.setPrimaryClip(ClipData.newPlainText("Frostpunk crash report", text));
            ClipData copied = clipboard.getPrimaryClip();
            CharSequence contents = copied != null && copied.getItemCount() == 1 ? copied.getItemAt(0).getText() : null;
            return contents != null && text.contentEquals(contents);
        } catch (RuntimeException error) {
            Log.w("Frostpunk", "Cannot copy crash report", error);
            return false;
        }
    }

    /** The line app.c writes from its signal handler: "Native signal 11 (SIGSEGV) code=1 addr=0x… pc=0x… tid=…". */
    static String signalReason(String marker, boolean russian) {
        String line = marker.trim();
        int end = line.indexOf('\n');
        if (end >= 0) line = line.substring(0, end);
        return (russian ? "Сбой в нативном коде игры: " : "Crash in native game code: ")
            + shorten(line.substring("Native signal ".length()), 160);
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
