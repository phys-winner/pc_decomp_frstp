package com.frostpunk.staticrecomp.launcher;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileLock;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.*;
import java.util.zip.GZIPInputStream;
import java.security.MessageDigest;
import org.json.JSONObject;

/** LiquidEngine texture archives, with the same private ASTC6 ABI as the game loader. */
public final class FrostpunkTextures implements TextureBackend {
    private static final String[] NAMES = {"textures-s3.dat", "textures-s3.idx"};
    private static final String WORK = ".frostpunk-astc-work", BACKUP = ".frostpunk-dxt";
    private static final String OWNER = "FROSTPUNK_ASTC_WORK_1\n";
    private static final int HEADER = 144, MAX_TEXTURE = 256 << 20;
    private volatile boolean cancelled;
    private volatile long done, total, bytesDone, bytesTotal;
    private volatile String message = "";
    private boolean switchMips;
    private volatile long plannedBytes;
    private static native byte[] encode(byte[] texture, int threads) throws IOException;
    private static native int mipSkip(int key, int width, int height, int count);

    public void quality(boolean reduced) { switchMips = reduced; }
    public long outputBytes() { return plannedBytes; }
    public static String convertedQuality(File game) throws IOException {
        File receipt = safe(safe(game, BACKUP), "conversion.json");
        need(receipt.exists(), "Texture conversion metadata is missing; select the original game data");
        need(receipt.isFile() && receipt.length() <= 8192, "Invalid texture conversion receipt");
        try {
            JSONObject value = new JSONObject(new String(Files.readAllBytes(receipt.toPath()), "UTF-8"));
            String quality = value.optString("quality", "pc");
            need(quality.equals("pc") || quality.equals("switch"), "Unknown converted texture quality");
            need(value.getString("index").equals(FrostpunkFiles.hex(MessageDigest.getInstance("SHA-256").digest(
                FrostpunkFiles.read(game, NAMES[1])))), "Converted texture index changed");
            return quality;
        } catch (IOException failure) { throw failure; }
        catch (Exception failure) { throw new IOException("Invalid texture conversion receipt", failure); }
    }
    public static boolean coversQuality(File game, boolean reduced) throws IOException {
        return convertedQuality(game).equals("pc") || reduced;
    }

    static final class Row {
        long stored, decoded, offset, target, source;
        int flag, format, key, skip;
    }

    private static void need(boolean ok, String why) throws IOException { if (!ok) throw new IOException(why); }
    private static File safe(File root, String name) throws IOException {
        File file = new File(root, name);
        need(!Files.isSymbolicLink(file.toPath()), "Symbolic link: " + file.getName());
        return file;
    }
    private static void move(File from, File to, boolean replace) throws IOException {
        if (replace) Files.move(from.toPath(), to.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        else Files.move(from.toPath(), to.toPath(), StandardCopyOption.ATOMIC_MOVE);
    }
    private static void syncWrite(File path, byte[] bytes) throws IOException {
        try (FileOutputStream out = new FileOutputStream(path)) { out.write(bytes); out.getFD().sync(); }
    }
    private static void cleanWork(File game) throws IOException {
        File work = safe(game, WORK);
        if (!work.exists()) return;
        ownedWork(work);
        Set<String> allowed = new HashSet<>(Arrays.asList(NAMES[0], NAMES[1], "commit", "owner"));
        for (String name : Objects.requireNonNull(work.list())) need(allowed.contains(name) && safe(work, name).isFile(), "Unexpected file in the texture conversion workspace");
        for (String name : new String[]{NAMES[0], NAMES[1], "commit", "owner"}) Files.deleteIfExists(safe(work, name).toPath());
        need(work.delete(), "Unexpected files in the texture conversion workspace");
    }
    private static void ownedWork(File work) throws IOException {
        File owner = safe(work, "owner");
        need(owner.isFile() && owner.length() == OWNER.length()
            && OWNER.equals(new String(Files.readAllBytes(owner.toPath()), "US-ASCII")), "Unrecognized texture conversion workspace");
    }
    private static void recoverLocked(File game) throws IOException {
        File work = safe(game, WORK), backup = safe(game, BACKUP);
        if (!work.exists()) return;
        ownedWork(work);
        if (safe(work, "commit").exists()) {
            File journal = safe(work, "commit");
            String transaction = new String(Files.readAllBytes(journal.toPath()), "US-ASCII");
            boolean zip = transaction.equals("FP_DXT_ZIP_0001\n");
            need(journal.isFile() && journal.length() == 16 && (zip || transaction.equals("FP_DXT_BACKUP_1\n")), "Invalid texture conversion journal");
            for (String name : NAMES) {
                File original = safe(backup, name);
                if (zip) Files.deleteIfExists(safe(game, name).toPath());
                else if (original.exists()) move(original, safe(game, name), true);
            }
            Files.deleteIfExists(safe(backup, "conversion.json").toPath());
            Files.deleteIfExists(safe(work, "commit").toPath());
            if (backup.isDirectory() && Objects.requireNonNull(backup.list()).length == 0) need(backup.delete(), "Cannot remove empty backup directory");
        }
        if (backup.isDirectory() && Objects.requireNonNull(backup.list()).length == 0) need(backup.delete(), "Cannot remove empty backup directory");
        cleanWork(game);
    }
    /** A killed conversion rolls back before either the launcher or the game reads the pair. */
    public static void recover(File game) throws IOException {
        if (!safe(game, WORK).exists()) return;
        try (RandomAccessFile lock = new RandomAccessFile(safe(game, ".frostpunk-textures.lock"), "rw");
             FileLock held = lock.getChannel().lock()) { recoverLocked(game); }
    }

    private static final class Archive implements AutoCloseable {
        final File index, data;
        final byte[] raw;
        final List<Row> rows = new ArrayList<>();
        final FrostpunkFiles.Reader stream;
        final long size;
        final String dataStamp, indexStamp;
        final boolean fromZip;
        long dxt, astc, decodedBytes, outputBytes;
        Archive(File game) throws IOException { this(game, false); }
        Archive(File game, boolean reduced) throws IOException {
            if (reduced) System.loadLibrary("frostpunk_texconv");
            index = safe(game, NAMES[1]); data = safe(game, NAMES[0]);
            need(FrostpunkFiles.isFile(game, NAMES[1]) && FrostpunkFiles.isFile(game, NAMES[0]), "Missing textures-s3.idx or textures-s3.dat");
            need(data.isFile() == index.isFile(), "Incomplete texture sidecar pair"); fromZip = !data.isFile();
            long indexLength = FrostpunkFiles.length(game, NAMES[1]);
            need(indexLength >= 11 && indexLength <= 29_000_011, "Invalid texture index length");
            raw = FrostpunkFiles.read(game, NAMES[1]);
            ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
            need(b.get() == 0 && b.get() == 2 && b.get() == 1, "Unknown texture archive header");
            long count = b.getLong();
            need(count >= 0 && count <= 1_000_000 && 11 + 29 * count == raw.length, "Invalid texture archive entry count");
            size = FrostpunkFiles.length(game, NAMES[0]); dataStamp = FrostpunkFiles.stamp(game, NAMES[0]); indexStamp = FrostpunkFiles.stamp(game, NAMES[1]);
            stream = FrostpunkFiles.reader(game, NAMES[0]);
            try {
                for (long i = 0; i < count; ++i) {
                    Row row = new Row(); row.key = b.getInt();
                    row.stored = b.getLong(); row.decoded = b.getLong(); row.offset = b.getLong(); row.flag = b.get() & 255;
                    need(row.stored >= 0 && row.decoded >= 0 && row.offset >= 0 && row.offset <= size && row.stored <= size - row.offset
                        && (row.flag == 0 || row.flag == 1) && (row.flag != 0 || row.stored == row.decoded), "Invalid texture archive entry span");
                    byte[] header = new byte[(int)Math.min(HEADER, row.decoded)];
                    try (InputStream in = entry(row)) { readFully(in, header); }
                    row.target = inspect(header, row, reduced);
                    if (dxt(row.format)) ++dxt;
                    if (row.format == 0x34545341 || row.format == 0x36545341) ++astc;
                    decodedBytes = Math.addExact(decodedBytes, row.source);
                    outputBytes = Math.addExact(outputBytes, row.target);
                    rows.add(row);
                }
            } catch (IOException | RuntimeException error) { stream.close(); throw error; }
        }
        InputStream entry(Row row) throws IOException {
            return entry(row, true);
        }
        InputStream entry(Row row, boolean inflate) throws IOException {
            stream.seek(row.offset);
            InputStream bounded = new InputStream() {
                long left = row.stored;
                @Override public int read() throws IOException { if (left == 0) return -1; int v = stream.read(); if (v >= 0) --left; return v; }
                @Override public int read(byte[] b, int off, int len) throws IOException {
                    if (left == 0) return -1;
                    int n = stream.read(b, off, (int)Math.min(left, len)); if (n > 0) left -= n; return n;
                }
            };
            return !inflate || row.flag == 0 ? bounded : new GZIPInputStream(bounded);
        }
        boolean unchanged() throws IOException {
            return FrostpunkFiles.stamp(data.getParentFile(), NAMES[0]).equals(dataStamp) && FrostpunkFiles.stamp(index.getParentFile(), NAMES[1]).equals(indexStamp)
                && Arrays.equals(raw, FrostpunkFiles.read(index.getParentFile(), NAMES[1]))
                && !Files.isSymbolicLink(index.toPath()) && !Files.isSymbolicLink(data.toPath());
        }
        @Override public void close() throws IOException { stream.close(); }
    }

    private static void readFully(InputStream in, byte[] bytes) throws IOException {
        int at = 0;
        while (at < bytes.length) { int n = in.read(bytes, at, bytes.length - at); if (n < 0) throw new EOFException("Truncated archive entry"); at += n; }
    }
    private static boolean dxt(int format) { return format == 0x31545844 || format == 0x33545844 || format == 0x35545844; }
    private static long inspect(byte[] header, Row row, boolean reduced) throws IOException {
        row.source = row.stored;
        if (header.length < 16) return row.stored;
        ByteBuffer b = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        long w = b.getInt() & 0xffffffffL, height = b.getInt() & 0xffffffffL;
        row.format = b.getInt(); long count = b.getInt() & 0xffffffffL;
        if (!dxt(row.format) && row.format != 0x34545341 && row.format != 0x36545341) return row.stored;
        int faces = height == 0xffffffffL ? 6 : 1;
        long h = faces == 6 ? w : height;
        need(header.length == HEADER && w > 0 && h > 0 && w <= 16384 && h <= 16384 && count > 0 && count <= 16
            && (Math.max(w, h) >> (count - 1)) != 0, "Invalid texture header");
        int block = dxt(row.format) ? 4 : row.format == 0x34545341 ? 4 : 6;
        int blockBytes = row.format == 0x31545844 ? 8 : 16;
        row.skip = reduced && dxt(row.format) ? mipSkip(row.key, (int)w, (int)height, (int)count) : 0;
        long source = HEADER, target = HEADER, kept = HEADER;
        for (int n = 0; n < count; ++n) {
            long mw = b.getShort() & 65535, mh = b.getShort() & 65535, size = b.getInt() & 0xffffffffL;
            int shift = (int)count - 1 - n;
            need(mw == Math.max(1, w >> shift) && mh == Math.max(1, h >> shift)
                && size == ((mw + block - 1) / block) * ((mh + block - 1) / block) * blockBytes, "Invalid texture mip layout");
            source += size * faces;
            if (n < count - row.skip) {
                if (dxt(row.format)) need(mw * mh * 4 <= (512L << 20), "Texture exceeds the conversion memory limit");
                kept += size * faces;
                target += ((mw + 5) / 6) * ((mh + 5) / 6) * 16 * faces;
            }
        }
        need(source == row.decoded, "Texture entry length does not match its mip chain");
        if (dxt(row.format)) row.source = kept;
        return dxt(row.format) ? target : row.stored;
    }

    @Override public Scan scan(File game) {
        Scan result = new Scan();
        try {
            recover(game);
            try (Archive archive = new Archive(game, switchMips)) {
                plannedBytes = archive.outputBytes;
                result.textures = archive.dxt; result.bytes = archive.decodedBytes;
                result.converted = archive.astc > 0 ? ASTC : 0;
                result.convertedTextures = archive.astc;
                result.convertedBytes = archive.astc > 0 ? archive.size : 0;
            }
        } catch (IOException | RuntimeException | LinkageError failure) { result.failure = failure.getMessage(); }
        return result;
    }
    private void check() throws IOException { if (cancelled) throw new IOException("Conversion cancelled"); }

    @Override public int convert(File game, int format, int threads) {
        cancelled = false; done = bytesDone = 0; message = "DXT → ASTC";
        if (format != ASTC) { message = "This port requires ASTC textures"; return -1; }
        try (RandomAccessFile lock = new RandomAccessFile(safe(game, ".frostpunk-textures.lock"), "rw");
             FileLock held = lock.getChannel().lock()) {
            recoverLocked(game);
            File backup = safe(game, BACKUP), work = safe(game, WORK);
            // shortcut: retain one original pair, restore it before converting another source installation in this folder.
            need(!backup.exists(), "Original DXT backup already exists in " + BACKUP + "; use another game folder for another source archive");
            boolean fromZip; byte[] originalIndex;
            MessageDigest outputHash = MessageDigest.getInstance("SHA-256");
            try (Archive archive = new Archive(game, switchMips)) {
                fromZip = archive.fromZip; originalIndex = archive.raw.clone();
                total = archive.dxt; bytesTotal = archive.decodedBytes;
                if (total == 0) return 0;
                need(game.getUsableSpace() >= archive.outputBytes + archive.raw.length + (8 << 20), "Not enough free space for the ASTC archive");
                System.loadLibrary("frostpunk_texconv");
                need(work.mkdir(), "Cannot create texture conversion workspace");
                syncWrite(safe(work, "owner"), OWNER.getBytes("US-ASCII"));
                byte[] index = archive.raw.clone(); ByteBuffer rows = ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN);
                try (FileOutputStream out = new FileOutputStream(safe(work, NAMES[0]))) {
                    byte[] buffer = new byte[65536];
                    for (int i = 0; i < archive.rows.size(); ++i) {
                        check(); Row row = archive.rows.get(i); long offset = out.getChannel().position();
                        try (InputStream in = archive.entry(row, dxt(row.format))) {
                            if (dxt(row.format)) {
                                need(row.source <= MAX_TEXTURE, "Texture exceeds the conversion memory limit");
                                byte[] source = new byte[(int)row.source]; readFully(in, source); check();
                                if (row.skip > 0) {
                                    ByteBuffer header = ByteBuffer.wrap(source).order(ByteOrder.LITTLE_ENDIAN);
                                    header.putInt(0, Math.max(1, header.getInt(0) >>> row.skip));
                                    if (header.getInt(4) != -1) header.putInt(4, Math.max(1, header.getInt(4) >>> row.skip));
                                    int count = header.getInt(12) - row.skip; header.putInt(12, count);
                                    Arrays.fill(source, 16 + 8 * count, HEADER, (byte)0);
                                }
                                byte[] astc = encode(source, Math.max(1, Math.min(4, threads))); need(astc != null && astc.length == row.target, "Invalid ASTC encoder output");
                                out.write(astc); outputHash.update(astc); ++done; bytesDone += row.source;
                            } else {
                                long left = row.stored;
                                while (left > 0) { check(); int n = in.read(buffer, 0, (int)Math.min(left, buffer.length));
                                    need(n > 0, "Truncated archive payload"); out.write(buffer, 0, n); outputHash.update(buffer, 0, n); left -= n; bytesDone += n; }
                            }
                        }
                        rows.position(11 + i * 29 + 4); rows.putLong(row.target).putLong(dxt(row.format) ? row.target : row.decoded)
                            .putLong(offset).put((byte)(dxt(row.format) ? 0 : row.flag));
                        message = "DXT → ASTC: " + done + " / " + total;
                    }
                    out.getFD().sync();
                }
                syncWrite(safe(work, NAMES[1]), index);
                need(archive.unchanged(), "Source textures changed during conversion"); check();
            }
            // The journal precedes every rename; recovery restores both originals after interruption.
            need(backup.mkdir(), "Cannot create the original texture backup");
            JSONObject receipt = new JSONObject().put("zip", fromZip).put("data", FrostpunkFiles.hex(outputHash.digest()))
                .put("quality", switchMips ? "switch" : "pc")
                .put("index", FrostpunkFiles.hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(safe(work, NAMES[1]).toPath()))))
                .put("originalIndex", FrostpunkFiles.hex(MessageDigest.getInstance("SHA-256").digest(originalIndex)));
            syncWrite(safe(backup, "conversion.json"), receipt.toString().getBytes("UTF-8"));
            syncWrite(safe(work, "commit"), (fromZip ? "FP_DXT_ZIP_0001\n" : "FP_DXT_BACKUP_1\n").getBytes("US-ASCII"));
            if (!fromZip) for (String name : NAMES) move(safe(game, name), safe(backup, name), false);
            for (String name : NAMES) move(safe(work, name), safe(game, name), false);
            Files.delete(safe(work, "commit").toPath()); cleanWork(game);
            message = "DXT → ASTC: " + total + " / " + total; return 0;
        } catch (Exception | LinkageError failure) {
            message = failure.getMessage();
            try { recover(game); } catch (IOException recovery) { message += "; " + recovery.getMessage(); }
            return -1;
        }
    }
    @Override public int undo(File game) {
        cancelled = false; done = bytesDone = 0; message = "Restoring original DXT textures";
        try (RandomAccessFile lock = new RandomAccessFile(safe(game, ".frostpunk-textures.lock"), "rw"); FileLock held = lock.getChannel().lock()) {
            recoverLocked(game); File backup = safe(game, BACKUP), work = safe(game, WORK);
            JSONObject receipt = new JSONObject(new String(Files.readAllBytes(safe(backup, "conversion.json").toPath()), "UTF-8"));
            boolean zip = receipt.getBoolean("zip");
            if (zip) need(FrostpunkFiles.archived(game, NAMES[0]) && FrostpunkFiles.archived(game, NAMES[1]), "Original ZIP is unavailable");
            total = 2; bytesTotal = safe(game, NAMES[0]).length() + safe(game, NAMES[1]).length();
            for (int i = 0; i < NAMES.length; ++i) {
                MessageDigest digest = MessageDigest.getInstance("SHA-256"); byte[] buffer = new byte[1 << 20];
                try (InputStream in = new FileInputStream(safe(game, NAMES[i]))) {
                    for (int n; (n = in.read(buffer)) > 0;) { check(); digest.update(buffer, 0, n); bytesDone += n; }
                }
                need(FrostpunkFiles.hex(digest.digest()).equals(receipt.getString(i == 0 ? "data" : "index")), "Converted textures changed; refusing to delete them"); ++done;
            }
            if (!zip) {
                need(FrostpunkFiles.hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(safe(backup, NAMES[1]).toPath()))).equals(receipt.getString("originalIndex")), "Original texture index changed");
                try (Archive original = new Archive(backup)) { need(original.dxt > 0, "Original DXT textures are unavailable"); }
            }
            need(work.mkdir(), "Cannot create texture restore workspace"); syncWrite(safe(work, "owner"), OWNER.getBytes("US-ASCII"));
            syncWrite(safe(work, "commit"), (zip ? "FP_DXT_ZIP_0001\n" : "FP_DXT_BACKUP_1\n").getBytes("US-ASCII"));
            recoverLocked(game); message = "Original DXT textures restored"; return 0;
        } catch (Exception failure) { message = failure.getMessage(); return -1; }
    }
    @Override public void cancel() { cancelled = true; }
    @Override public long[] progress() { return new long[]{done, total, bytesDone, bytesTotal}; }
    @Override public String message() { return message; }
}
