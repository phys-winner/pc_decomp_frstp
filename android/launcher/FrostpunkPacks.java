package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.json.JSONArray;
import org.json.JSONObject;

/** Optional port-ui and switch-ui sidecars from the port's own GitHub release. */
final class FrostpunkPacks {
    static final long MAX_BYTES = 64L << 20;
    static final class Offer {
        final String kind, name, url;
        final long size;
        Offer(String kind, String name, String url, long size) {
            this.kind = kind; this.name = name; this.url = url; this.size = size;
        }
    }

    private FrostpunkPacks() { }

    static List<Offer> offers(Context context, String repo, List<String> missing) throws Exception {
        if (repo == null || repo.isEmpty() || missing.isEmpty()) return Collections.emptyList();
        JSONArray releases = FrostpunkGithub.releases(context, repo);
        List<Offer> found = new ArrayList<>();
        for (String kind : missing) {
            Offer offer = find(releases, kind);
            if (offer != null) found.add(offer);
        }
        return found;
    }

    private static Offer find(JSONArray releases, String kind) {
        for (int i = 0; i < releases.length(); ++i) {
            JSONObject release = releases.optJSONObject(i);
            if (release == null || release.optBoolean("draft")) continue;
            JSONArray assets = release.optJSONArray("assets");
            if (assets == null) continue;
            for (int n = 0; n < assets.length(); ++n) {
                JSONObject asset = assets.optJSONObject(n);
                if (asset == null || !named(kind, asset.optString("name"))) continue;
                return new Offer(kind, asset.optString("name"), asset.optString("browser_download_url"), asset.optLong("size"));
            }
        }
        return null;
    }

    static boolean named(String kind, String name) {
        String lower = name == null ? "" : name.toLowerCase(Locale.ROOT);
        return lower.equals(kind + ".zip") || lower.equals("frostpunk-" + kind + ".zip");
    }

    /** Checks every payload against the embedded manifest, then replaces the sidecar files. */
    static void install(Context context, File game, Offer offer) throws Exception {
        if (offer.size > MAX_BYTES) throw new IOException("resource pack is too large");
        if (!game.isDirectory() && !game.mkdirs()) throw new IOException("Cannot create the game folder");
        JSONObject manifest = manifest(context, offer.kind);
        JSONObject payloads = manifest.getJSONObject("payloads");
        List<String> names = new ArrayList<>();
        for (Iterator<String> it = payloads.keys(); it.hasNext(); ) names.add(it.next());
        Collections.sort(names);
        File zip = new File(game, offer.kind + ".download");
        try {
            FrostpunkGithub.download(context, offer.url, zip, MAX_BYTES);
            List<File> ready = new ArrayList<>();
            try (ZipFile archive = new ZipFile(zip)) {
                if (archive.size() > 64) throw new IOException("resource pack has too many files");
                long total = 0;
                for (String name : names) {
                    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) throw new IOException("invalid resource name");
                    ZipEntry entry = archive.getEntry(name);
                    JSONObject pin = payloads.getJSONObject(name);
                    long bytes = pin.getLong("bytes");
                    if (entry == null || !name.equals(entry.getName()) || entry.isDirectory() || entry.getSize() != bytes)
                        throw new IOException("resource pack lacks " + name);
                    total += bytes;
                    if (total > MAX_BYTES) throw new IOException("resource pack is too large");
                    File part = new File(game, name + ".part");
                    MessageDigest digest = MessageDigest.getInstance("SHA-256");
                    try (InputStream in = archive.getInputStream(entry); FileOutputStream out = new FileOutputStream(part)) {
                        byte[] buffer = new byte[65536];
                        long wrote = 0;
                        for (int n; (n = in.read(buffer)) > 0; ) {
                            wrote += n;
                            if (wrote > bytes) throw new IOException("resource pack entry grew: " + name);
                            digest.update(buffer, 0, n);
                            out.write(buffer, 0, n);
                        }
                        if (wrote != bytes || !hex(digest.digest()).equals(pin.getString("sha256")))
                            throw new IOException("resource checksum mismatch: " + name);
                    }
                    ready.add(part);
                }
            }
            for (int i = 0; i < names.size(); ++i) replace(ready.get(i), new File(game, names.get(i)));
            ready.clear();
            writeAsset(context, "game-data-" + offer.kind + ".json", new File(game, offer.kind + ".json"));
        } finally {
            if (zip.exists() && !zip.delete()) zip.deleteOnExit();
            for (String name : names) {
                File part = new File(game, name + ".part");
                if (part.exists() && !part.delete()) part.deleteOnExit();
            }
        }
    }

    private static JSONObject manifest(Context context, String kind) throws Exception {
        try (InputStream in = context.getAssets().open("game-data-" + kind + ".json")) {
            byte[] data = readAll(in, 1 << 20);
            JSONObject result = new JSONObject(new String(data, "UTF-8"));
            if (result.getInt("schema") != 1 || !kind.equals(result.getString("kind"))) throw new IOException("Unknown resource schema");
            return result;
        }
    }

    private static void writeAsset(Context context, String asset, File target) throws IOException {
        File part = new File(target.getParentFile(), target.getName() + ".part");
        try (InputStream in = context.getAssets().open(asset); FileOutputStream out = new FileOutputStream(part)) {
            byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        }
        replace(part, target);
    }

    private static void replace(File part, File target) throws IOException {
        if (target.exists() && !target.delete()) throw new IOException("cannot replace " + target.getName());
        if (!part.renameTo(target)) throw new IOException("cannot store " + target.getName());
    }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        byte[] buffer = new byte[8192];
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        for (int n; (n = in.read(buffer)) > 0; ) {
            if (out.size() + n > limit) throw new IOException("resource metadata is too large");
            out.write(buffer, 0, n);
        }
        return out.toByteArray();
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) text.append(String.format(Locale.ROOT, "%02x", b & 0xFF));
        return text.toString();
    }
}
