package com.frostpunk.staticrecomp.probe;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;
import java.io.*;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.function.Consumer;

/** Prepares the XZ runtime on the game worker, before its first dlopen. */
final class NativeLibrary {
    private static final String NAME = "libfrostpunk_runtime.so";
    private static native long nativeUnpack(AssetManager assets, String name, String path,
                                           long total, int chunk, int[] sizes);

    private NativeLibrary() { }

    static synchronized String prepare(Context context, Consumer<String> status) throws IOException {
        String index;
        try (InputStream in = context.getAssets().open("native/index.txt")) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            for (int n; (n = in.read(buffer)) != -1;) {
                if (bytes.size() + n > 4096) throw new IOException("Native runtime index too large");
                bytes.write(buffer, 0, n);
            }
            index = new String(bytes.toByteArray(), StandardCharsets.US_ASCII);
        } catch (FileNotFoundException absent) {
            return new File(context.getApplicationInfo().nativeLibraryDir, NAME).getAbsolutePath();
        }
        String[] fields = index.trim().split("\\s+");
        long total;
        int chunk;
        int[] sizes;
        try {
            if (fields.length != 5 || !NAME.equals(fields[0]) || !fields[2].matches("[0-9a-f]{64}"))
                throw new IllegalArgumentException();
            total = Long.parseLong(fields[1]);
            chunk = Integer.parseInt(fields[3]);
            String[] packed = fields[4].split(",", -1);
            if (total <= 0 || total > (1L << 30) || chunk < (1 << 20) || chunk > (128 << 20)
                    || packed.length < 1 || packed.length > 32 || (total + chunk - 1) / chunk != packed.length)
                throw new IllegalArgumentException();
            sizes = new int[packed.length];
            for (int i = 0; i < sizes.length; i++) {
                sizes[i] = Integer.parseInt(packed[i]);
                if (sizes[i] <= 0) throw new IllegalArgumentException();
            }
        } catch (IllegalArgumentException invalid) {
            throw new IOException("Invalid native runtime index", invalid);
        }
        File dir = new File(context.getFilesDir(), "native");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create native runtime directory");
        File target = new File(dir, NAME), stamp = new File(dir, NAME + ".sha256");
        try (RandomAccessFile lock = new RandomAccessFile(new File(dir, "runtime.lock"), "rw");
             FileLock held = lock.getChannel().lock()) {
            boolean current = target.isFile() && target.length() == total && stamp.isFile()
                    && stamp.length() == 65
                    && fields[2].equals(new String(Files.readAllBytes(stamp.toPath()), StandardCharsets.US_ASCII).trim());
            if (current) {
                status.accept("Checking native runtime…");
                current = fields[2].equals(sha256(target));
            }
            if (current) {
                if(!target.setReadOnly())throw new IOException("Cannot protect cached native runtime");
                Log.i("FrostpunkNative", "NATIVE_RUNTIME_CACHE_HIT sha256=" + fields[2]);
                return target.getAbsolutePath();
            }
            if (dir.getUsableSpace() < total + (8 << 20))
                throw new IOException("Not enough free space to unpack native runtime (" + total + " bytes required)");
            File part = new File(dir, NAME + ".part"), stampPart = new File(dir, NAME + ".sha256.part");
            Files.deleteIfExists(part.toPath());
            Files.deleteIfExists(stampPart.toPath());
            status.accept("Unpacking native runtime for this build…");
            long started = android.os.SystemClock.elapsedRealtime();
            System.loadLibrary("frostpunk_xz");
            try {
                long written = nativeUnpack(context.getAssets(), "native/" + NAME + ".xz",
                        part.getAbsolutePath(), total, chunk, sizes);
                if (written != total || part.length() != total || !fields[2].equals(sha256(part)))
                    throw new IOException("Native runtime unpack/verification failed (" + written + ")");
                if (!part.setReadOnly()) throw new IOException("Cannot protect native runtime");
                Files.move(part.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                try (FileOutputStream out = new FileOutputStream(stampPart)) {
                    out.write((fields[2] + "\n").getBytes(StandardCharsets.US_ASCII));
                    out.getFD().sync();
                }
                Files.move(stampPart.toPath(), stamp.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } finally {
                Files.deleteIfExists(part.toPath());
                Files.deleteIfExists(stampPart.toPath());
            }
            Log.i("FrostpunkNative", "NATIVE_RUNTIME_UNPACKED bytes=" + total + " ms="
                    + (android.os.SystemClock.elapsedRealtime() - started) + " sha256=" + fields[2]);
            return target.getAbsolutePath();
        }
    }

    private static String sha256(File file) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream in = new FileInputStream(file)) {
                byte[] buffer = new byte[262144];
                for (int n; (n = in.read(buffer)) != -1;) digest.update(buffer, 0, n);
            }
            StringBuilder hex = new StringBuilder(64);
            for (byte b : digest.digest()) hex.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            return hex.toString();
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("SHA-256 unavailable", impossible);
        }
    }
}
