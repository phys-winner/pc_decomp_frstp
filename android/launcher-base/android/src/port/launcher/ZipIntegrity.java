package port.launcher;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Locale;

/** One successful CRC-32 pass of a stored ZIP, remembered beside its ZMI1 index.
 *  A later open of the same index, archive size and mtime skips the read. */
final class ZipIntegrity {
    private ZipIntegrity() { }

    static File receipt(File index) { return new File(index.getPath() + ".ok"); }

    static String stamp(String indexSha256, long size, long mtime) {
        return "1 " + indexSha256 + " " + size + " " + mtime;
    }

    static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) text.append(String.format(Locale.ROOT, "%02x", value & 255));
        return text.toString();
    }

    static boolean matches(File index, String stamp) {
        File file = receipt(index);
        if (!file.isFile() || stamp.length() > 256 || file.length() != stamp.length()) return false;
        try {
            return stamp.equals(new String(Files.readAllBytes(file.toPath()), "US-ASCII"));
        } catch (IOException failure) {
            return false;
        }
    }

    static void write(File index, String stamp) throws IOException {
        File file = receipt(index);
        File part = new File(file.getPath() + ".part");
        try (FileOutputStream out = new FileOutputStream(part)) {
            out.write(stamp.getBytes("US-ASCII"));
            out.getFD().sync();
        }
        if (file.exists() && !file.delete()) throw new IOException("Cannot replace the ZIP integrity receipt");
        if (!part.renameTo(file)) {
            part.delete();
            throw new IOException("Cannot record the ZIP integrity receipt");
        }
    }
}
