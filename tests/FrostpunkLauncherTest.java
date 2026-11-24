package com.frostpunk.staticrecomp.launcher;

import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.Deflater;

public final class FrostpunkLauncherTest {
    static void need(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    static void memoryWarning() {
        long threshold = 15L * 1024 * 1024 * 1024 / 2;
        need(!FrostpunkDevice.lowMemory(0), "unknown RAM does not warn");
        need(!FrostpunkDevice.lowMemory(-1), "invalid RAM does not warn");
        need(FrostpunkDevice.lowMemory(6L * 1024 * 1024 * 1024), "6 GiB warns");
        need(FrostpunkDevice.lowMemory(threshold), "7.5 GiB warns");
        need(!FrostpunkDevice.lowMemory(threshold + 1), "above threshold does not warn");
        need(!FrostpunkDevice.lowMemory(8L * 1024 * 1024 * 1024), "8 GiB does not warn");
    }
    static final String[] PAIR = {"textures-s3.dat", "textures-s3.idx"};
    static void storedZip(File root) throws Exception {
        File original = game(root, "zip-original", new byte[][]{texture(0x35545844, 8)}, true);
        byte[][] source = pair(original); File zip = new File(root, "textures.zip"), index = new File(root, "textures.zmi");
        try (java.util.zip.ZipOutputStream out = new java.util.zip.ZipOutputStream(new FileOutputStream(zip))) {
            for (int i = 0; i < PAIR.length; ++i) {
                java.util.zip.CRC32 crc = new java.util.zip.CRC32(); crc.update(source[i]);
                java.util.zip.ZipEntry entry = new java.util.zip.ZipEntry(PAIR[i]); entry.setMethod(0); entry.setSize(source[i].length); entry.setCompressedSize(source[i].length); entry.setCrc(crc.getValue());
                out.putNextEntry(entry); out.write(source[i]); out.closeEntry();
            }
        }
        byte[] archive = Files.readAllBytes(zip.toPath()); ByteBuffer raw = ByteBuffer.wrap(archive).order(ByteOrder.LITTLE_ENDIAN);
        ByteBuffer zmi = ByteBuffer.allocate(1024).order(ByteOrder.LITTLE_ENDIAN); zmi.putInt(0x31494d5a).putInt(2).putLong(archive.length); zmi.position(48);
        int position = 0;
        for (int i = 0; i < PAIR.length; ++i) {
            need(raw.getInt(position) == 0x04034b50 && (raw.getShort(position + 6) & 8) == 0 && raw.getShort(position + 8) == 0, "stored local ZIP header");
            int name = raw.getShort(position + 26) & 65535, extra = raw.getShort(position + 28) & 65535;
            int offset = position + 30 + name + extra; byte[] path = PAIR[i].getBytes("UTF-8");
            need(raw.getInt(position + 18) == source[i].length, "stored ZIP span");
            zmi.putLong(offset).putLong(source[i].length).putInt(raw.getInt(position + 14)).putInt(0).putLong(1000).putShort((short)path.length).put(path);
            position = offset + source[i].length;
        }
        Files.write(index.toPath(), Arrays.copyOf(zmi.array(), zmi.position()));
        File sidecar = new File(root, "zip-sidecar"); need(sidecar.mkdir(), "fresh ZIP sidecar");
        java.lang.reflect.Method mount = FrostpunkFiles.class.getDeclaredMethod("mount", File.class, File.class, android.os.ParcelFileDescriptor.class, String.class); mount.setAccessible(true);
        mount.invoke(null, sidecar, index, android.os.ParcelFileDescriptor.open(zip, android.os.ParcelFileDescriptor.MODE_READ_ONLY), "fixture");
        need(sidecar.list().length == 0 && FrostpunkFiles.isFile(sidecar, PAIR[0]), "ZIP data read without extraction");
        same(source, new byte[][]{FrostpunkFiles.read(sidecar, PAIR[0]), FrostpunkFiles.read(sidecar, PAIR[1])});
        FrostpunkTextures backend = new FrostpunkTextures(); need(backend.scan(sidecar).textures == 1, "DXT scan directly from ZIP");
        need(backend.convert(sidecar, TextureBackend.ASTC, 1) == 0, backend.message());
        need(backend.scan(sidecar).converted == TextureBackend.ASTC, "ZIP conversion publishes ASTC sidecars");
        byte[] converted = Files.readAllBytes(new File(sidecar, PAIR[0]).toPath()); byte[] changed = converted.clone(); changed[0] ^= 1;
        Files.write(new File(sidecar, PAIR[0]).toPath(), changed); need(backend.undo(sidecar) != 0, "undo refuses changed sidecars");
        need(Arrays.equals(changed, Files.readAllBytes(new File(sidecar, PAIR[0]).toPath())), "changed sidecar preserved");
        Files.write(new File(sidecar, PAIR[0]).toPath(), converted); need(backend.undo(sidecar) == 0, backend.message());
        need(!new File(sidecar, PAIR[0]).exists() && !new File(sidecar, PAIR[1]).exists() && backend.scan(sidecar).textures == 1, "undo returns to original DXT inside ZIP");
        for (int phase = 0; phase < 3; ++phase) {
            File work = new File(sidecar, ".frostpunk-astc-work"), backup = new File(sidecar, ".frostpunk-dxt"); need(work.mkdir() && backup.mkdir(), "ZIP rollback workspace");
            Files.write(new File(work, "owner").toPath(), "FROSTPUNK_ASTC_WORK_1\n".getBytes("US-ASCII"));
            Files.write(new File(work, "commit").toPath(), "FP_DXT_ZIP_0001\n".getBytes("US-ASCII"));
            if (phase > 0) Files.write(new File(sidecar, PAIR[0]).toPath(), new byte[]{42});
            if (phase > 1) Files.write(new File(sidecar, PAIR[1]).toPath(), new byte[]{42});
            FrostpunkTextures.recover(sidecar); need(!new File(sidecar, PAIR[0]).exists() && !new File(sidecar, PAIR[1]).exists(), "interrupted ZIP conversion rolls back");
        }
        FrostpunkFiles.verifyOnce(sidecar);
        File receipt = new File(index.getPath() + ".ok");
        need(receipt.isFile(), "one-time ZIP integrity receipt");
        byte[] receiptBytes = Files.readAllBytes(receipt.toPath());
        FrostpunkFiles.verifyOnce(sidecar);
        need(Arrays.equals(receiptBytes, Files.readAllBytes(receipt.toPath())), "ZIP integrity check does not repeat");
        long dataAt = ByteBuffer.wrap(Files.readAllBytes(index.toPath())).order(ByteOrder.LITTLE_ENDIAN).getLong(48);
        need(dataAt > 0 && dataAt < archive.length, "stored payload offset");
        try (RandomAccessFile flipped = new RandomAccessFile(zip, "rw")) {
            flipped.seek(dataAt);
            int value = flipped.read();
            need(value >= 0, "payload byte");
            flipped.seek(dataAt);
            flipped.write(value ^ 1);
            android.system.StructStat stat = android.system.Os.stat(zip.getPath());
            String indexSha = ZipIntegrity.hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(index.toPath())));
            String legacy = "1 " + indexSha + " " + stat.st_size + " " + stat.st_mtime;
            Files.write(receipt.toPath(), legacy.getBytes("US-ASCII"));
            boolean rejected = false;
            try { FrostpunkFiles.verifyOnce(sidecar); }
            catch (IOException failure) {
                rejected = failure.getMessage() != null && failure.getMessage().contains("CRC") && failure.getMessage().contains(PAIR[0]);
                if (!rejected) throw failure;
            }
            need(rejected, "damaged ZIP rejected despite the legacy integrity receipt");
            TextureBackend.Scan broken = new FrostpunkTextures().scan(sidecar);
            need(broken.failure != null && broken.failure.contains("CRC"), "texture check names the damaged ZIP: " + broken.failure);
            flipped.seek(dataAt);
            flipped.write(value);
        }
        if (receipt.exists()) need(receipt.delete(), "receipt cleared after the damage test");
        FrostpunkFiles.verifyOnce(sidecar);
        need(Arrays.equals(archive, Files.readAllBytes(zip.toPath())), "original stored ZIP unchanged");
    }
    static byte[] texture(int format, int width) {
        int block = format == 0x31545844 ? 8 : 16, bytes = ((width + 3) / 4) * ((width + 3) / 4) * block;
        byte[] result = new byte[144 + bytes]; ByteBuffer b = ByteBuffer.wrap(result).order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(width).putInt(width).putInt(format).putInt(1).putShort((short)width).putShort((short)width).putInt(bytes);
        for (int at = 144; at < result.length; at += block) {
            if (format == 0x33545844) Arrays.fill(result, at, at + 8, (byte)255);
            if (format == 0x35545844) result[at] = (byte)255;
            int color = at + (block == 8 ? 0 : 8);
            result[color] = 0; result[color + 1] = (byte)248; result[color + 2] = (byte)224; result[color + 3] = 7;
        }
        return result;
    }
    /** ETC2 beside ASTC: RGB8 for opaque textures, RGBA8 for alpha and for every texture array element. */
    static void etc2(File root) throws Exception {
        need(FrostpunkTextures.key("Environment/Terrain/Substances/Snow_Flat_NOR.dds") == 0xa453360f, "texture array element key");
        for (int format : new int[]{0x31545844, 0x33545844, 0x35545844}) for (boolean zipped : new boolean[]{false, true}) {
            File game = game(root, "etc2-" + Integer.toHexString(format) + (zipped ? "-gzip" : "-raw"), new byte[][]{texture(format, 8)}, zipped);
            byte[][] original = pair(game); FrostpunkTextures backend = new FrostpunkTextures();
            need(backend.scan(game).formats.equals(Arrays.asList(format == 0x31545844 ? "DXT1" : format == 0x33545844 ? "DXT3" : "DXT5")), "scan names the original format");
            need(backend.outputBytes(TextureBackend.ETC2) == 144 + 4 * 16 && backend.outputBytes(TextureBackend.ASTC) == 144 + 4 * 16, "planned sizes");
            need(FrostpunkTextures.convertedFormat(game) == 0, "originals have no conversion format");
            need(backend.convert(game, TextureBackend.ETC2, 2) == 0, backend.message());
            same(original, pair(new File(game, ".frostpunk-dxt")));
            TextureBackend.Scan after = backend.scan(game);
            need(after.failure == null && after.textures == 0 && after.converted == TextureBackend.ETC2 && after.convertedTextures == 1, "ETC2 output scan: " + after.failure);
            need(FrostpunkTextures.convertedFormat(game) == TextureBackend.ETC2 && FrostpunkTextures.coversQuality(game, false), "ETC2 receipt");
            /* The fixture's DXT1 and DXT5 are opaque (RGB8, 8 bytes a block), its DXT3 has alpha 255 as well. */
            byte[] data = pair(game)[0];
            need(ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt(8) == 0x32435445 && data.length == 144 + 4 * 8, "opaque texture becomes ETC2 RGB8");
            need(backend.undo(game) == 0, backend.message()); same(original, pair(game));
            need(backend.convert(game, TextureBackend.ASTC, 1) == 0 && FrostpunkTextures.convertedFormat(game) == TextureBackend.ASTC, "ASTC after an ETC2 undo");
        }
        need(FrostpunkTextures.rendererReads("native_vulkan_panvk", TextureBackend.ETC2) && FrostpunkTextures.rendererReads("native_vulkan", TextureBackend.ETC2)
            && !FrostpunkTextures.rendererReads("dxvk", TextureBackend.ETC2) && FrostpunkTextures.rendererReads("turnip", TextureBackend.ASTC)
            && FrostpunkTextures.rendererReads("dxvk_panvk", 0), "renderers that read each cache");
        byte[] alpha = texture(0x35545844, 16); alpha[144] = 0; alpha[145] = (byte)255;   /* alpha0 = 0: the image is transparent */
        byte[] descriptor = ("<TextureArrayDescriptor>\n<Prop Name=\"Texture file name\" Value=\"Environment/Terrain/Substances/Snow_Flat_NOR.dds\" />\n"
            + "<Prop Name=\"Texture file name\" Value=\"Environment\\Terrain\\Substances\\Ice2_NOR.dds\" />\n</TextureArrayDescriptor>").getBytes("UTF-8");
        File arrays = game(root, "etc2-array", new byte[][]{texture(0x35545844, 16), texture(0x35545844, 16), texture(0x35545844, 16), alpha, descriptor}, true);
        byte[] index = pair(arrays)[1]; ByteBuffer keys = ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN);
        keys.putInt(11, 0xa453360f).putInt(11 + 29, FrostpunkTextures.key("environment/terrain/substances/ice2_nor.texture"));
        Files.write(new File(arrays, PAIR[1]).toPath(), index);
        FrostpunkTextures backend = new FrostpunkTextures(); need(backend.convert(arrays, TextureBackend.ETC2, 1) == 0, backend.message());
        ByteBuffer converted = ByteBuffer.wrap(pair(arrays)[1]).order(ByteOrder.LITTLE_ENDIAN); byte[] data = pair(arrays)[0];
        int[] expected = {0x41435445, 0x41435445, 0x32435445, 0x41435445};
        for (int i = 0; i < 4; ++i) {
            long stored = converted.getLong(11 + i * 29 + 4), offset = converted.getLong(11 + i * 29 + 20);
            need(ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).getInt((int)offset + 8) == expected[i]
                && stored == 144 + 16 * (expected[i] == 0x41435445 ? 16 : 8), "array elements and alpha keep RGBA8, entry " + i);
        }
        File broken = game(root, "etc2-array-missing", new byte[][]{texture(0x35545844, 16), descriptor}, false);
        FrostpunkTextures lenient = new FrostpunkTextures();
        need(lenient.convert(broken, TextureBackend.ETC2, 1) == 0 && lenient.scan(broken).converted == TextureBackend.ETC2, "an array naming an absent texture does not stop the conversion");
    }
    static byte[] gzip(byte[] source) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(new byte[]{31, (byte)139, 8, 0, 0, 0, 0, 0, 0, (byte)255});
        Deflater codec = new Deflater(6, true); codec.setInput(source); byte[] chunk = new byte[source.length + 1024];
        int n = codec.deflate(chunk, 0, chunk.length, Deflater.SYNC_FLUSH); out.write(chunk, 0, n); codec.end();
        return out.toByteArray();
    }
    static void mipQuality(File root) throws Exception {
        byte[] chain = new byte[144 + (256 * 256 + 512 * 512 + 1024 * 1024)];
        ByteBuffer header = ByteBuffer.wrap(chain).order(ByteOrder.LITTLE_ENDIAN);
        header.putInt(1024).putInt(1024).putInt(0x35545844).putInt(3);
        int at = 144;
        for (int i = 0; i < 3; ++i) {
            int width = 256 << i; byte[] texture = texture(0x35545844, width);
            header.putShort((short)width).putShort((short)width).putInt(texture.length - 144);
            System.arraycopy(texture, 144, chain, at, texture.length - 144); at += texture.length - 144;
        }
        File reduced = game(root, "switch-mips", new byte[][]{chain}, true);
        byte[] index = pair(reduced)[1]; ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN).putInt(11, 0x00323991);
        Files.write(new File(reduced, PAIR[1]).toPath(), index); byte[][] original = pair(reduced);
        FrostpunkTextures backend = new FrostpunkTextures(); backend.quality(true);
        need(backend.scan(reduced).bytes == 144 + 256 * 256, "Switch progress excludes the two unused top mips");
        need(backend.convert(reduced, TextureBackend.ASTC, 4) == 0, backend.message());
        byte[] output = pair(reduced)[0], convertedIndex = pair(reduced)[1]; ByteBuffer h = ByteBuffer.wrap(output).order(ByteOrder.LITTLE_ENDIAN);
        need(h.getInt(0) == 256 && h.getInt(4) == 256 && h.getInt(12) == 1, "Switch conversion publishes a complete smaller mip chain");
        File control = game(root, "switch-mip-control", new byte[][]{texture(0x35545844, 256)}, false);
        need(new FrostpunkTextures().convert(control, TextureBackend.ASTC, 4) == 0, "mip control conversion");
        need(Arrays.equals(output, pair(control)[0]), "kept mip is byte-exact to an independent full conversion");
        need(FrostpunkTextures.coversQuality(reduced, true) && !FrostpunkTextures.coversQuality(reduced, false), "Switch cache cannot satisfy PC quality");
        need(FrostpunkTextures.coversQuality(control, true) && FrostpunkTextures.coversQuality(control, false), "full cache covers both qualities");
        File receipt = new File(control, ".frostpunk-dxt/conversion.json"); byte[] metadata = Files.readAllBytes(receipt.toPath());
        need(receipt.delete(), "remove fixture metadata");
        try { FrostpunkTextures.coversQuality(control, true); throw new AssertionError("unidentified cache accepted"); }
        catch (IOException expected) { }
        Files.write(receipt.toPath(), metadata);
        byte[] changed = pair(reduced)[1]; changed[11] ^= 1; Files.write(new File(reduced, PAIR[1]).toPath(), changed);
        try { FrostpunkTextures.coversQuality(reduced, true); throw new AssertionError("changed conversion index accepted"); }
        catch (IOException expected) { }
        Files.write(new File(reduced, PAIR[1]).toPath(), convertedIndex);
        need(backend.undo(reduced) == 0, backend.message()); same(original, pair(reduced));
        backend.quality(false); need(backend.convert(reduced, TextureBackend.ASTC, 4) == 0, backend.message());
        need(FrostpunkTextures.coversQuality(reduced, false), "PC quality can be rebuilt from the preserved originals");
    }
    static File game(File root, String name, byte[][] entries, boolean compressed) throws IOException {
        File game = new File(root, name); need(game.mkdir(), "fresh fixture directory");
        ByteBuffer index = ByteBuffer.allocate(11 + entries.length * 29).order(ByteOrder.LITTLE_ENDIAN);
        index.put(new byte[]{0, 2, 1}).putLong(entries.length);
        try (FileOutputStream data = new FileOutputStream(new File(game, PAIR[0]))) {
            for (int i = 0; i < entries.length; ++i) {
                byte[] bytes = compressed ? gzip(entries[i]) : entries[i];
                index.putInt(i + 1).putLong(bytes.length).putLong(entries[i].length).putLong(data.getChannel().position()).put((byte)(compressed ? 1 : 0));
                data.write(bytes);
            }
        }
        Files.write(new File(game, PAIR[1]).toPath(), index.array()); return game;
    }
    static byte[][] pair(File folder) throws IOException {
        return new byte[][]{Files.readAllBytes(new File(folder, PAIR[0]).toPath()), Files.readAllBytes(new File(folder, PAIR[1]).toPath())};
    }
    static void same(byte[][] a, byte[][] b) { need(Arrays.deepEquals(a, b), "original pair changed"); }
    static void profiles() {
        Map<String, String> r = new HashMap<>();
        Spec.Mode stock = new Spec.Mode(), turnip = new Spec.Mode();
        stock.kind = "vulkan";
        turnip.kind = "turnip";
        stock.probe = turnip.probe = "dxvk";
        r.put("stock.vendor", "1010"); r.put("stock.driverId", "7"); r.put("stock.gpuType", "1");
        r.put("stock.status", "ok"); r.put("stock.formats", "0");
        r.put("stock.astc", "1"); r.put("stock.bgra", "1"); r.put("stock.bcFeature", "0");
        need(FrostpunkDevice.profileProblem(r, stock) == null, "PowerVR ASTC accepted");
        need(!FrostpunkDevice.dxt(r, stock) && FrostpunkDevice.astc(r, stock), "format emulation is not the BC feature");
        r.put("stock.gpuType", "4"); need("software".equals(FrostpunkDevice.profileProblem(r, stock)), "CPU rejected");
        r.put("stock.gpuType", "1"); r.put("stock.astc", "0");
        need("no_texture_format".equals(FrostpunkDevice.profileProblem(r, stock)), "a readable format is required");
        r.put("stock.astc", "1"); r.put("stock.vendor", "13b5"); r.put("stock.driverId", "9");
        r.put("stock.partiallyBound", "1"); r.put("stock.robustBufferAccess2", "0");
        need(FrostpunkDevice.profileProblem(r, stock) == null, "Mali guarded descriptors accepted");
        r.put("stock.partiallyBound", "0"); need("descriptors".equals(FrostpunkDevice.profileProblem(r, stock)), "unsafe descriptors rejected");
        r.put("stock.robustBufferAccess2", "1"); need(FrostpunkDevice.profileProblem(r, stock) == null, "native robustness accepted");
        r.put("stock.vendor", "9999"); need("driver".equals(FrostpunkDevice.profileProblem(r, stock)), "unknown driver rejected");
        r.put("turnip.vendor", "5143"); r.put("turnip.driverId", "18"); r.put("turnip.bgra", "1");
        r.put("turnip.status", "ok"); r.put("turnip.formats", "54");
        need(FrostpunkDevice.profileProblem(r, turnip) == null && FrostpunkDevice.dxt(r, turnip), "Turnip BC accepted");
        r.put("turnip.formats", "0");
        need("no_texture_format".equals(FrostpunkDevice.profileProblem(r, turnip)), "Turnip without a readable format rejected");
        r.put("turnip.astc", "1");
        turnip.probe = "version";
        need(FrostpunkDevice.profileProblem(r, turnip) == null && !FrostpunkDevice.dxt(r, turnip),
            "native Turnip accepts ASTC when it cannot sample BC originals");
    }
    public static void main(String[] args) throws Exception {
        need(args.length == 1 && args[0].startsWith("/data/local/tmp/frostpunk-pixel10-startup-fix/") && args[0].endsWith("/texture-test-fixture"), "isolated fixture root required");
        File root = new File(args[0]); need(root.mkdir(), "fresh fixture root required"); profiles();
        for (int format : new int[]{0x31545844, 0x33545844, 0x35545844}) for (boolean zipped : new boolean[]{false, true}) {
            byte[] opaque = "<texture-metadata/>".getBytes("UTF-8");
            File game = game(root, Integer.toHexString(format) + (zipped ? "-gzip" : "-raw"), new byte[][]{texture(format, 8), opaque}, zipped);
            byte[][] original = pair(game); FrostpunkTextures backend = new FrostpunkTextures(); TextureBackend.Scan before = backend.scan(game);
            need(before.failure == null && before.textures == 1 && before.converted == 0, "DXT source scan");
            need(backend.convert(game, TextureBackend.ASTC, 1) == 0, backend.message());
            same(original, pair(new File(game, ".frostpunk-dxt")));
            TextureBackend.Scan after = backend.scan(game);
            need(after.failure == null && after.textures == 0 && after.converted == TextureBackend.ASTC && after.convertedTextures == 1, "ASTC output scan");
            byte[] data = pair(game)[0], unchanged = zipped ? gzip(opaque) : opaque;
            need(Arrays.equals(unchanged, Arrays.copyOfRange(data, data.length - unchanged.length, data.length)), "opaque entry preserves original storage bytes");
            need(ByteBuffer.wrap(pair(game)[1]).order(ByteOrder.LITTLE_ENDIAN).get(11 + 29 + 28) == (zipped ? 1 : 0), "opaque entry preserves gzip flag");
            long[] progress = backend.progress(); need(progress[0] == progress[1] && progress[2] == progress[3] && progress[1] == 1, "conversion completes progress");
        }
        etc2(root);
        for (int phase = 0; phase <= 4; ++phase) {
            File game = game(root, "interrupted-" + phase, new byte[][]{texture(0x31545844, 8)}, false);
            byte[][] original = pair(game); File work = new File(game, ".frostpunk-astc-work"), backup = new File(game, ".frostpunk-dxt");
            need(work.mkdir() && backup.mkdir(), "transaction fixture");
            Files.write(new File(work, "owner").toPath(), "FROSTPUNK_ASTC_WORK_1\n".getBytes("US-ASCII"));
            Files.write(new File(work, "commit").toPath(), "FP_DXT_BACKUP_1\n".getBytes("US-ASCII"));
            for (String name : PAIR) Files.write(new File(work, name).toPath(), new byte[]{42});
            if (phase >= 1) Files.move(new File(game, PAIR[0]).toPath(), new File(backup, PAIR[0]).toPath());
            if (phase >= 2) Files.move(new File(game, PAIR[1]).toPath(), new File(backup, PAIR[1]).toPath());
            if (phase >= 3) Files.move(new File(work, PAIR[0]).toPath(), new File(game, PAIR[0]).toPath());
            if (phase >= 4) Files.move(new File(work, PAIR[1]).toPath(), new File(game, PAIR[1]).toPath());
            FrostpunkTextures.recover(game); same(original, pair(game)); need(!work.exists(), "journal workspace removed");
        }
        File cancelled = game(root, "cancelled", new byte[][]{texture(0x35545844, 1024), texture(0x35545844, 1024)}, false);
        byte[][] original = pair(cancelled); FrostpunkTextures backend = new FrostpunkTextures(); int[] result = {0};
        Thread worker = new Thread(() -> result[0] = backend.convert(cancelled, TextureBackend.ASTC, 1)); worker.start();
        long deadline = System.currentTimeMillis() + 10000;
        while (backend.progress()[1] == 0 && worker.isAlive() && System.currentTimeMillis() < deadline) Thread.yield();
        backend.cancel(); worker.join(20000); need(!worker.isAlive() && result[0] != 0, "conversion cancelled"); same(original, pair(cancelled));
        File unknown = game(root, "unknown-workspace", new byte[][]{texture(0x31545844, 8)}, false);
        File reserved = new File(unknown, ".frostpunk-astc-work"); need(reserved.mkdir(), "unknown workspace");
        File keep = new File(reserved, PAIR[0]); Files.write(keep.toPath(), new byte[]{1, 2, 3});
        need(new FrostpunkTextures().scan(unknown).failure != null && keep.length() == 3, "unowned workspace preserved");
        File corrupt = game(root, "corrupt", new byte[][]{texture(0x31545844, 8)}, false);
        byte[] bad = pair(corrupt)[1]; ByteBuffer.wrap(bad).order(ByteOrder.LITTLE_ENDIAN).putLong(15, Long.MAX_VALUE); Files.write(new File(corrupt, PAIR[1]).toPath(), bad);
        need(new FrostpunkTextures().scan(corrupt).failure != null, "out of bounds entry rejected");
        File undo = game(root, "folder-undo", new byte[][]{texture(0x31545844, 8)}, true); byte[][] saved = pair(undo);
        FrostpunkTextures restore = new FrostpunkTextures(); need(restore.convert(undo, TextureBackend.ASTC, 1) == 0, restore.message());
        need(restore.undo(undo) == 0, restore.message()); same(saved, pair(undo));
        File serial = game(root, "one-thread", new byte[][]{texture(0x35545844, 128)}, false);
        File parallel = game(root, "four-threads", new byte[][]{texture(0x35545844, 128)}, false);
        need(new FrostpunkTextures().convert(serial, TextureBackend.ASTC, 1) == 0, "single-thread encoding");
        need(new FrostpunkTextures().convert(parallel, TextureBackend.ASTC, 4) == 0, "four-thread encoding"); same(pair(serial), pair(parallel));
        memoryWarning();
        storedZip(root);
        mipQuality(root);
        File paddedAtlas = game(root, "padded-8196-atlas", new byte[][]{texture(0x35545844, 8196)}, true);
        FrostpunkTextures atlas = new FrostpunkTextures(); need(atlas.scan(paddedAtlas).failure == null, "8196 atlas preflight");
        need(atlas.convert(paddedAtlas, TextureBackend.ASTC, 4) == 0, atlas.message());
        need(atlas.scan(paddedAtlas).converted == TextureBackend.ASTC, "8196 atlas converted");
        System.out.println("FROSTPUNK_LAUNCHER_TEST profiles=10 conversions=14 etc2=PASS etc2_array_elements_rgba=PASS recoveries=8 cancellation=1 malformed=1 ownership=1 folder_and_zip_undo=PASS zip_source_unchanged=PASS zip_integrity=PASS threads_1_vs_4_byte_exact=PASS quality_switch_vs_pc=PASS skipped_mips_byte_exact=PASS unchanged_entries_stay_compressed=PASS atlas_8196=PASS");
    }
}
