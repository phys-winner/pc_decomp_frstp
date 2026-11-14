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
        try { fingerprint(context, game, kind); return true; } catch (Exception failure) { return false; }
    }
    static boolean installed(Context context, File game, String kind) {
        try { return fingerprint(context, game, kind).equals(State.get(context, "resources", kind, "")); }
        catch (Exception failure) { return false; }
    }
    @Override void work() throws Exception {
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
        Os.setenv("FROSTPUNK_PORT_DATA", installed(context, game, "port-ui") ? "1" : "0", true);
        Os.setenv("FROSTPUNK_SWITCH_DATA", installed(context, game, "switch-ui") ? "1" : "0", true);
        Os.setenv("FROSTPUNK_TEXTURE_QUALITY", new File(game, ".frostpunk-dxt/conversion.json").isFile()
            ? FrostpunkTextures.convertedQuality(game) : "pc", true);
    }
}
