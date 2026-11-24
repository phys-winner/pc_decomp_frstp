package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.util.Log;
import java.io.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.CRC32;

/** The same sidecar-first, positional ZMI1 reads used by the shared native zipmount. */
public final class FrostpunkFiles {
    private static final class Entry {
        String name; long offset, size, crc; int flags;
    }
    private static final class Mount {
        String game, identity;
        File index;
        FileInputStream input;
        FileChannel channel;
        final Map<String, Entry> entries = new TreeMap<>();
    }
    /** Progress and cancellation for one stored-entry CRC pass. */
    interface Watch { void update(long done, long total, String name) throws IOException; }
    private static volatile Mount mounted;
    private static String selected = "";
    private static final Object verifyLock = new Object();
    private FrostpunkFiles() { }
    private static void directory(File game) throws IOException {
        if (Files.isSymbolicLink(game.toPath()) || !game.isDirectory() && !game.mkdirs() && !game.isDirectory()) throw new IOException("Cannot create the archive sidecar folder");
    }

    private static File safe(File game, String name) throws IOException {
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.equals(".") || name.equals("..")) throw new IOException("Invalid game file name");
        File file = new File(game, name);
        if (Files.isSymbolicLink(file.toPath()) || !file.getCanonicalFile().getParentFile().equals(game.getCanonicalFile())) throw new IOException("Game file leaves its folder");
        return file;
    }
    private static synchronized void mount(File game, File index, ParcelFileDescriptor descriptor, String identity) throws IOException {
        FileInputStream input = new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
        try {
            byte[] raw = Files.readAllBytes(index.toPath()); ByteBuffer data = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
            if (raw.length < 48 || data.getInt() != 0x31494d5a) throw new IOException("Invalid stored ZIP index");
            long count = data.getInt() & 0xffffffffL, size = data.getLong();
            if (size < 0 || size != input.getChannel().size() || count > (raw.length - 48) / 34) throw new IOException("The ZIP changed after it was selected");
            data.position(48); Mount next = new Mount(); next.game = game.getCanonicalPath(); next.index = index; next.input = input; next.channel = input.getChannel();
            next.identity = hex(MessageDigest.getInstance("SHA-256").digest(raw));
            for (long i = 0; i < count; ++i) {
                if (data.remaining() < 34) throw new IOException("Truncated stored ZIP index");
                Entry entry = new Entry(); entry.offset = data.getLong(); entry.size = data.getLong(); entry.crc = data.getInt() & 0xffffffffL;
                entry.flags = data.getInt(); data.getLong(); int length = data.getShort() & 65535;
                if (length > data.remaining() || entry.size < 0 || (entry.flags & ~1) != 0
                    || entry.flags == 0 && (entry.offset < 0 || entry.offset > size || entry.size > size - entry.offset)) throw new IOException("Invalid stored ZIP entry span");
                byte[] name = new byte[length]; data.get(name); String path = new String(name, "UTF-8"); entry.name = path;
                safe(game, path);
                if (next.entries.put(path.toLowerCase(Locale.ROOT), entry) != null) throw new IOException("Duplicate stored ZIP entry");
            }
            if (data.hasRemaining()) throw new IOException("Unexpected stored ZIP index tail");
            Mount old = mounted; mounted = next;
            if (old != null) old.input.close();
        } catch (Exception failure) { input.close(); if (failure instanceof IOException) throw (IOException)failure; throw new IOException(failure); }
    }
    static synchronized void launcher(Context context, File game, Spec.Distribution distribution) throws IOException {
        Spec.Part part = distribution.part("game"); String uri = Install.mount(context, distribution, part);
        File index = new File(State.folder(context), "mount-" + distribution.id + "-game.zmi");
        String identity = game.getCanonicalPath() + ":" + uri + ":" + index.length() + ":" + index.lastModified();
        if (identity.equals(selected)) return;
        if (uri.isEmpty()) { Mount old = mounted; mounted = null; if (old != null) old.input.close(); }
        else {
            directory(game);
            ParcelFileDescriptor descriptor = uri.startsWith("content:")
                ? context.getContentResolver().openFileDescriptor(android.net.Uri.parse(uri), "r")
                : ParcelFileDescriptor.open(new File(uri), ParcelFileDescriptor.MODE_READ_ONLY);
            if (descriptor == null) throw new IOException("Cannot open the selected game ZIP");
            mount(game, index, descriptor, identity);
        }
        selected = identity;
    }

    /** CRC-32 of every stored ZIP entry, once per index and archive file snapshot.
     *  An unpacked folder, or a mount of another folder, returns without reading. */
    static void verifyOnce(File game) throws IOException { verifyOnce(game, null); }
    static void verifyOnce(File game, Watch watch) throws IOException {
        synchronized (verifyLock) {
            Mount mount = mounted;
            if (mount == null || mount.index == null || !mount.game.equals(game.getCanonicalPath())) return;
            String stamp = stampOf(mount);
            if (ZipIntegrity.matches(mount.index, stamp)) { Log.i("FrostpunkFiles", "ZIP integrity cached"); return; }
            long total = 0;
            for (Entry entry : mount.entries.values()) if (entry.flags == 0) total += entry.size;
            byte[] buffer = new byte[1 << 20];
            long checked = 0;
            Log.i("FrostpunkFiles", "ZIP integrity check " + (total >> 20) + " MiB");
            for (Entry entry : mount.entries.values()) {
                if (entry.flags != 0) continue;
                if (watch != null) watch.update(checked, total, entry.name);
                CRC32 crc = new CRC32();
                long left = entry.size, at = entry.offset;
                while (left > 0) {
                    int n = mount.channel.read(ByteBuffer.wrap(buffer, 0, (int)Math.min(buffer.length, left)), at);
                    if (n <= 0) throw new IOException("ZIP archive is damaged: " + entry.name);
                    crc.update(buffer, 0, n);
                    at += n; left -= n; checked += n;
                    if (watch != null && (checked & ~((64L << 20) - 1)) != ((checked - n) & ~((64L << 20) - 1)))
                        watch.update(checked, total, entry.name);
                }
                if (crc.getValue() != entry.crc) throw new IOException("ZIP archive is damaged (CRC): " + entry.name);
            }
            if (watch != null) watch.update(checked, total, "");
            if (mounted != mount) throw new IOException("The ZIP changed during the integrity check");
            if (!stamp.equals(stampOf(mount))) throw new IOException("The ZIP changed during the integrity check");
            ZipIntegrity.write(mount.index, stamp);
            Log.i("FrostpunkFiles", "ZIP integrity ok " + (checked >> 20) + " MiB");
        }
    }
    private static String stampOf(Mount mount) throws IOException {
        try {
            android.system.StructStat stat = Os.fstat(mount.input.getFD());
            return ZipIntegrity.stamp(mount.identity, stat);
        } catch (android.system.ErrnoException failure) { throw new IOException(failure); }
    }
    public static void attach(Context context) throws IOException {
        String index = Os.getenv("PORT_MOUNT_GAME"), fd = Os.getenv("PORT_MOUNT_FD_GAME");
        if (index != null && !index.isEmpty() && fd != null && !fd.isEmpty()) {
            directory(game(context));
            try { mount(game(context), new File(index), ParcelFileDescriptor.fromFd(Integer.parseInt(fd)), index); }
            catch (NumberFormatException failure) { throw new IOException("Invalid stored ZIP descriptor", failure); }
        }
    }
    public static File game(Context context) {
        String path = Os.getenv("PORT_PART_GAME");
        return path != null && !path.isEmpty() ? new File(path) : new File(context.getExternalFilesDir(null), "game");
    }
    private static Entry entry(File game, String name) throws IOException {
        Mount mount = mounted;
        if (mount == null || !mount.game.equals(game.getCanonicalPath())) return null;
        Entry result = mount.entries.get(name.toLowerCase(Locale.ROOT));
        return result != null && result.flags == 0 ? result : null;
    }
    static boolean isFile(File game, String name) throws IOException { return safe(game, name).isFile() || entry(game, name) != null; }
    static boolean archived(File game, String name) throws IOException { return entry(game, name) != null; }
    static long length(File game, String name) throws IOException {
        File file = safe(game, name); if (file.isFile()) return file.length();
        Entry entry = entry(game, name); if (entry == null) throw new FileNotFoundException(name); return entry.size;
    }
    static String stamp(File game, String name) throws IOException {
        File file = safe(game, name); if (file.isFile()) return file.length() + ":" + file.lastModified();
        Entry entry = entry(game, name); Mount mount = mounted;
        if (entry == null || mount == null) throw new FileNotFoundException(name);
        try { android.system.StructStat stat = Os.fstat(mount.input.getFD());
            return mount.identity + ":" + stat.st_size + ":" + stat.st_mtime + ":" + entry.offset + ":" + entry.size + ":" + entry.crc;
        } catch (android.system.ErrnoException failure) { throw new IOException(failure); }
    }
    static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(); for (byte value : bytes) text.append(String.format(Locale.ROOT, "%02x", value & 255)); return text.toString();
    }
    static byte[] read(File game, String name) throws IOException {
        long size = length(game, name); if (size > (1 << 20)) throw new IOException("Game metadata is too large");
        byte[] result = new byte[(int)size];
        try (Reader input = reader(game, name)) {
            int at = 0; while (at < result.length) { int n = input.read(result, at, result.length - at); if (n <= 0) throw new EOFException(name); at += n; }
        }
        return result;
    }
    static Reader reader(File game, String name) throws IOException { return new Reader(game, name); }
    public static InputStream open(Context context, String name) throws IOException { return reader(game(context), name); }

    static final class Reader extends InputStream {
        private final FileChannel channel;
        private final FileInputStream owned;
        private final long offset, size;
        private long position;
        Reader(File game, String name) throws IOException {
            File file = safe(game, name);
            if (file.isFile()) { owned = new FileInputStream(file); channel = owned.getChannel(); offset = 0; size = channel.size(); }
            else {
                Entry entry = entry(game, name); Mount mount = mounted;
                if (entry == null || mount == null) throw new FileNotFoundException(name);
                channel = mount.channel; owned = null; offset = entry.offset; size = entry.size;
            }
        }
        long length() { return size; }
        void seek(long at) throws IOException { if (at < 0) throw new IOException("Negative game file offset"); position = at; }
        @Override public int read(byte[] buffer, int at, int count) throws IOException {
            if (at < 0 || count < 0 || at > buffer.length - count) throw new IndexOutOfBoundsException();
            if (count == 0) return 0; if (position >= size) return -1;
            int n = channel.read(ByteBuffer.wrap(buffer, at, (int)Math.min(count, size - position)), offset + position);
            if (n < 0) throw new EOFException("The ZIP ends inside a game file");
            position += n; return n;
        }
        @Override public int read() throws IOException { byte[] value = new byte[1]; return read(value, 0, 1) < 0 ? -1 : value[0] & 255; }
        @Override public void close() throws IOException { if (owned != null) owned.close(); }
    }
}
