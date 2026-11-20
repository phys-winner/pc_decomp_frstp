package com.frostpunk.staticrecomp.launcher;

import android.content.Context;
import android.system.Os;
import java.io.*;
import java.security.MessageDigest;
import java.util.*;
import org.json.*;

/** Verify optional prefixed resources; the native reader joins their DAT tails in memory. */
public final class FrostpunkData extends Work.Job {
    static final String[] KINDS = {"port-ui", "switch-ui"};
    private final Context context;
    private final File game;

    FrostpunkData(Context context, File game) {
        super(Texts.PLAY, FrostpunkDevice.tr(context, "Checking game resources", "Проверка ресурсов игры"), game);
        this.context = context.getApplicationContext(); this.game = game;
    }
    private static JSONObject manifest(Context context, String kind) throws Exception {
        if (!Arrays.asList(KINDS).contains(kind)) throw new IOException("Unknown resource kind");
        try (InputStream in = context.getAssets().open("game-data-" + kind + ".json")) {
            ByteArrayOutputStream data = new ByteArrayOutputStream(); byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) > 0;) { if (data.size() + n > (1 << 20)) throw new IOException("Resource metadata is too large"); data.write(buffer, 0, n); }
            JSONObject result = new JSONObject(new String(data.toByteArray(), "UTF-8"));
            if (result.getInt("schema") != 1 || !kind.equals(result.getString("kind"))) throw new IOException("Unknown resource schema");
            return result;
        }
    }
    private static boolean matches(File file, JSONObject pin) throws Exception {
        if (java.nio.file.Files.isSymbolicLink(file.toPath())) throw new IOException("Resource is a symbolic link");
        if (!file.isFile() || file.length() != pin.getLong("bytes")) return false;
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(file)) {
            byte[] buffer = new byte[65536];
            for (int n; (n = in.read(buffer)) > 0;) digest.update(buffer, 0, n);
        }
        return FrostpunkFiles.hex(digest.digest()).equals(pin.getString("sha256"));
    }
    private static synchronized void bundled(Context context, File game) throws Exception {
        if (!game.isDirectory()) throw new IOException("Game folder is missing");
        JSONObject expected = manifest(context, "port-ui");
        JSONObject payloads = expected.getJSONObject("payloads");
        for (Iterator<String> it = payloads.keys(); it.hasNext();) {
            String name = it.next();
            if (!name.startsWith("port-ui.") || name.contains("/") || name.contains("\\"))
                throw new IOException("Invalid bundled resource name");
            JSONObject pin = payloads.getJSONObject(name);
            File target = new File(game, name);
            if (matches(target, pin)) continue;
            File part = File.createTempFile("port-ui-", ".part", game);
            try {
                try (InputStream in = context.getAssets().open("port-ui/" + name);
                     FileOutputStream out = new FileOutputStream(part)) {
                    byte[] buffer = new byte[65536];
                    for (int n; (n = in.read(buffer)) > 0;) out.write(buffer, 0, n);
                    out.getFD().sync();
                }
                if (!matches(part, pin)) throw new IOException("Bundled resource checksum mismatch: " + name);
                Os.rename(part.getAbsolutePath(), target.getAbsolutePath());
            } finally { if (part.exists()) part.delete(); }
        }
        File marker = new File(game, "port-ui.json");
        byte[] text = expected.toString().getBytes("UTF-8");
        if (java.nio.file.Files.isSymbolicLink(marker.toPath())) throw new IOException("Resource marker is a symbolic link");
        if (!marker.isFile() || marker.length() != text.length || !Arrays.equals(java.nio.file.Files.readAllBytes(marker.toPath()), text)) {
            File part = File.createTempFile("port-ui-", ".part", game);
            try {
                try (FileOutputStream out = new FileOutputStream(part)) { out.write(text); out.getFD().sync(); }
                Os.rename(part.getAbsolutePath(), marker.getAbsolutePath());
            } finally { if (part.exists()) part.delete(); }
        }
        if (!State.put(context, "resources", "port-ui", fingerprint(context, game, "port-ui")))
            throw new IOException("Cannot record bundled UI verification");
        State.put(context, "resources", "port-ui.error", null);
    }
    private static String fingerprint(Context context, File game, String kind) throws Exception {
        JSONObject expected = manifest(context, kind);
        JSONObject actual = new JSONObject(new String(FrostpunkFiles.read(game, kind + ".json"), "UTF-8"));
        if (!actual.toString().equals(expected.toString())) throw new IOException("Resource data is for another port build");
        MessageDigest digest = MessageDigest.getInstance("SHA-256"); digest.update(expected.toString().getBytes("UTF-8"));
        JSONObject payloads = expected.getJSONObject("payloads");
        List<String> names = new ArrayList<>(); for (Iterator<String> it = payloads.keys(); it.hasNext();) names.add(it.next()); Collections.sort(names);
        names.add(kind + ".json");
        for (String name : names) {
            if (!name.equals(kind + ".json") && FrostpunkFiles.length(game, name) != payloads.getJSONObject(name).getLong("bytes")) throw new IOException("Incomplete resource data");
            digest.update(name.getBytes("UTF-8")); digest.update(FrostpunkFiles.stamp(game, name).getBytes("UTF-8"));
        }
        return FrostpunkFiles.hex(digest.digest());
    }
    static boolean packagePresent(Context context, File game, String kind) {
        if ("port-ui".equals(kind)) return true;
        try { fingerprint(context, game, kind); return true; } catch (Exception failure) { return false; }
    }
    static boolean installed(Context context, File game, String kind) {
        try { return fingerprint(context, game, kind).equals(State.get(context, "resources", kind, "")); }
        catch (Exception failure) { return false; }
    }
    @Override void work() throws Exception {
        bundled(context, game);
        FrostpunkFiles.verifyOnce(game, (doneBytes, totalBytes, name) -> { done = doneBytes; total = totalBytes; file = name; check(); });
        for (String kind : KINDS) {
            State.put(context, "resources", kind, "");
            if (!packagePresent(context, game, kind)) continue;
            try {
                String before = fingerprint(context, game, kind);
                JSONObject payloads = manifest(context, kind).getJSONObject("payloads");
                for (Iterator<String> it = payloads.keys(); it.hasNext();) {
                    String name = it.next(); file = name; JSONObject pin = payloads.getJSONObject(name); total += pin.getLong("bytes");
                    MessageDigest digest = MessageDigest.getInstance("SHA-256"); byte[] buffer = new byte[1 << 20]; long size = 0;
                    try (InputStream in = FrostpunkFiles.reader(game, name)) {
                        for (int n; (n = in.read(buffer)) > 0;) { check(); digest.update(buffer, 0, n); size += n; done += n; }
                    }
                    if (size != pin.getLong("bytes") || !FrostpunkFiles.hex(digest.digest()).equals(pin.getString("sha256"))) throw new IOException("Resource checksum mismatch: " + name);
                }
                if (!before.equals(fingerprint(context, game, kind))) throw new IOException("Resource data changed during verification");
                State.put(context, "resources", kind, before);
                State.put(context, "resources", kind + ".error", null);
            } catch (Failure cancelled) { throw cancelled; }
            catch (Exception failure) { State.put(context, "resources", kind + ".error", failure.getMessage()); Work.warn("Optional " + kind + " disabled", failure); }
        }
    }
    public static void attach(Context context) throws Exception {
        android.os.LocaleList locales = android.content.res.Resources.getSystem().getConfiguration().getLocales();
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            android.app.LocaleManager manager = context.getSystemService(android.app.LocaleManager.class);
            if (manager != null) locales = manager.getSystemLocales();
        }
        Os.setenv("FROSTPUNK_SYSTEM_LANGUAGE", locales.isEmpty() ? "en" : locales.get(0).toLanguageTag(), true);
        FrostpunkFiles.attach(context); File game = FrostpunkFiles.game(context);
        bundled(context, game);
        boolean portUi = installed(context, game, "port-ui");
        Os.setenv("FROSTPUNK_PORT_DATA", portUi ? "1" : "0", true);
        JSONArray fan = portUi ? manifest(context, "port-ui").optJSONArray("fan_languages") : null;
        List<String> fanLanguages = new ArrayList<>();
        if (fan != null) for (int i = 0; i < fan.length(); i++) fanLanguages.add(fan.getString(i));
        Os.setenv("FROSTPUNK_FAN_LANGUAGES", String.join(",", fanLanguages), true);
        Os.setenv("FROSTPUNK_SWITCH_DATA", installed(context, game, "switch-ui") ? "1" : "0", true);
        Os.setenv("FROSTPUNK_TEXTURE_QUALITY", new File(game, ".frostpunk-dxt/conversion.json").isFile()
            ? FrostpunkTextures.convertedQuality(game) : "pc", true);
    }
}
