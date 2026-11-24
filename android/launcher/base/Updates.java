package port.launcher;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Build;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Properties;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Updates from the port's GitHub releases. Every release carries update.json (written by
 *  tools/update_manifest.py from the APK itself, see docs/UPDATES.md), so the newest one is at
 *  github.com/<repo>/releases/latest/download/update.json: one small request, no API limits. The
 *  result is kept in the state file "update", which the game reads too (Launch). */
final class Updates {
    static final long INTERVAL_MS = 6 * 60 * 60 * 1000L;
    private static final int TIMEOUT_MS = 15000, LIMIT = 1 << 20;

    static final class Info {
        long versionCode, size;
        int minSdk;
        String versionName = "", tag = "", page = "", notes = "", apkName = "", apkUrl = "", sha256 = "", signer = "";
    }

    private Updates() { }

    static boolean automatic(Context context) { return !"0".equals(State.get(context, State.CHOICES, "updates.auto", "1")); }

    static boolean due(Context context) {
        Spec spec = Spec.get(context);
        if (!spec.updates || !automatic(context)) return false;
        long checked = State.getLong(State.read(context, State.UPDATE), "checked", 0);
        return Math.abs(System.currentTimeMillis() - checked) > INTERVAL_MS;
    }

    /** Blocking: asks GitHub, stores what it found (or why it could not) and returns it. */
    static Info check(Context context) throws IOException {
        Spec spec = Spec.get(context);
        if (spec.github.isEmpty()) throw new IOException("no GitHub repository in port.json");
        String text = null;
        IOException direct = null;
        try {
            text = get(context, "https://github.com/" + spec.github + "/releases/latest/download/update.json");
        } catch (IOException failure) {
            direct = failure;
            /* /releases/latest skips prereleases, so a dev tag is absent there. The asset URL is
             * one more request and is limited to 60 an hour per address. */
            try {
                JSONObject release = new JSONObject(get(context, "https://api.github.com/repos/" + spec.github + "/releases/latest"));
                String url = assetUrl(release, "update.json");
                if (url == null) throw failure;
                text = get(context, url);
            } catch (JSONException | IOException fallback) {
                text = null;
            }
        }
        if (text != null) {
            try {
                Info info = parse(context, text);
                record(context, info, null);
                return info;
            } catch (JSONException e) {
                record(context, null, e.getMessage());
                throw new IOException("update.json: " + e.getMessage(), e);
            }
        }
        try {
            Info info = fromReleaseList(context, spec.github);
            record(context, info, null);
            return info;
        } catch (JSONException | IOException fallback) {
            String message = direct != null ? direct.getMessage() : fallback.getMessage();
            record(context, null, message);
            if (direct != null) throw direct;
            if (fallback instanceof IOException) throw (IOException) fallback;
            throw new IOException(fallback);
        }
    }

    /** Newest non-draft release. update.json wins; otherwise the tag is compared with this build. */
    private static Info fromReleaseList(Context context, String github) throws IOException, JSONException {
        JSONArray releases = new JSONArray(get(context, "https://api.github.com/repos/" + github + "/releases?per_page=10", 2 << 20));
        for (int i = 0; i < releases.length(); ++i) {
            JSONObject release = releases.optJSONObject(i);
            if (release == null || release.optBoolean("draft")) continue;
            String update = assetUrl(release, "update.json");
            if (update != null) return parse(context, get(context, update));
            return fromRelease(context, release);
        }
        throw new IOException("no GitHub release");
    }

    private static String assetUrl(JSONObject release, String name) {
        JSONArray assets = release.optJSONArray("assets");
        if (assets == null) return null;
        for (int i = 0; i < assets.length(); ++i) {
            JSONObject asset = assets.optJSONObject(i);
            if (asset != null && name.equals(asset.optString("name"))) return asset.optString("browser_download_url", null);
        }
        return null;
    }

    private static Info fromRelease(Context context, JSONObject release) {
        Info info = new Info();
        info.tag = release.optString("tag_name", "");
        info.page = release.optString("html_url", "");
        info.notes = release.optString("body", "");
        info.versionName = info.tag.startsWith("v") || info.tag.startsWith("V") ? info.tag.substring(1) : info.tag;
        JSONArray assets = release.optJSONArray("assets");
        if (assets != null) for (int i = 0; i < assets.length(); ++i) {
            JSONObject asset = assets.optJSONObject(i);
            if (asset == null || !asset.optString("name").endsWith(".apk")) continue;
            info.apkName = asset.optString("name");
            info.apkUrl = asset.optString("browser_download_url", "");
            info.size = asset.optLong("size", 0);
            break;
        }
        long installed = installedCode(context);
        info.versionCode = compareVersions(info.versionName, installedName(context)) > 0 ? installed + 1 : installed;
        return info;
    }

    /** Dotted numbers only, so 0.1.1-dev and 0.1.1 are the same release. */
    private static int compareVersions(String left, String right) {
        int[] a = versionTuple(left), b = versionTuple(right);
        int n = Math.max(a.length, b.length);
        for (int i = 0; i < n; ++i) {
            int av = i < a.length ? a[i] : 0, bv = i < b.length ? b[i] : 0;
            if (av != bv) return av > bv ? 1 : -1;
        }
        return 0;
    }

    private static int[] versionTuple(String text) {
        if (text == null) return new int[0];
        int[] values = new int[8];
        int count = 0, value = -1;
        for (int i = 0; i <= text.length() && count < values.length; ++i) {
            char c = i < text.length() ? text.charAt(i) : '.';
            if (c >= '0' && c <= '9' && value < 100000000) value = (value < 0 ? 0 : value) * 10 + (c - '0');
            else if (value >= 0) {
                values[count++] = value;
                value = -1;
                if (c != '.') break;
            } else if (count > 0) break;
        }
        return java.util.Arrays.copyOf(values, count);
    }

    static Info parse(Context context, String text) throws JSONException {
        JSONObject json = new JSONObject(text);
        if (json.optInt("schema", 0) != 1) throw new JSONException("schema " + json.optInt("schema"));
        String pkg = json.optString("package", "");
        if (!pkg.isEmpty() && !pkg.equals(context.getPackageName())) throw new JSONException("package " + pkg);
        JSONObject latest = json.optJSONObject("latest");
        Info info = new Info();
        if (latest == null) return info;   /* no release yet: nothing newer than this build */
        info.versionCode = latest.getLong("versionCode");
        info.versionName = latest.optString("versionName", "");
        info.tag = latest.optString("tag", "");
        info.page = latest.optString("page", "");
        info.notes = latest.optString("notes", "");
        JSONObject apk = latest.optJSONObject("apk");
        if (apk != null) {
            info.apkName = apk.optString("name", "");
            info.apkUrl = apk.optString("url", "");
            info.size = apk.optLong("size", 0);
            info.sha256 = apk.optString("sha256", "");
            info.signer = apk.optString("signer", "");
            info.minSdk = apk.optInt("minSdk", 0);
        }
        return info;
    }

    private static void record(Context context, Info info, String error) {
        State.update(context, State.UPDATE, values -> {
            values.setProperty("checked", Long.toString(System.currentTimeMillis()));
            if (error != null) { values.setProperty("error", error); return; }
            values.remove("error");
            values.setProperty("versionCode", Long.toString(info.versionCode));
            values.setProperty("versionName", info.versionName);
            values.setProperty("tag", info.tag);
            values.setProperty("page", info.page);
            values.setProperty("notes", info.notes);
            values.setProperty("apkName", info.apkName);
            values.setProperty("apkUrl", info.apkUrl);
            values.setProperty("apkSize", Long.toString(info.size));
            values.setProperty("sha256", info.sha256);
            values.setProperty("signer", info.signer);
            values.setProperty("minSdk", Integer.toString(info.minSdk));
        });
    }

    /** The newest release when it is newer than this build, runs here and was not skipped. */
    static Info available(Context context) {
        Properties values = State.read(context, State.UPDATE);
        long code = State.getLong(values, "versionCode", 0);
        if (code <= installedCode(context) || code == State.getLong(values, "skipped", -1)) return null;
        Info info = new Info();
        info.versionCode = code;
        info.versionName = values.getProperty("versionName", "");
        info.tag = values.getProperty("tag", "");
        info.page = values.getProperty("page", "");
        info.notes = values.getProperty("notes", "");
        info.apkName = values.getProperty("apkName", "");
        info.apkUrl = values.getProperty("apkUrl", "");
        info.size = State.getLong(values, "apkSize", 0);
        info.sha256 = values.getProperty("sha256", "");
        info.signer = values.getProperty("signer", "");
        info.minSdk = (int) State.getLong(values, "minSdk", 0);
        if (info.minSdk > Build.VERSION.SDK_INT) return null;
        return info;
    }

    static String lastError(Context context) { return State.read(context, State.UPDATE).getProperty("error"); }

    static void skip(Context context, long versionCode) { State.put(context, State.UPDATE, "skipped", Long.toString(versionCode)); }

    static long installedCode(Context context) {
        PackageInfo info = installed(context);
        return info == null ? 0 : Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
    }

    static String installedName(Context context) {
        PackageInfo info = installed(context);
        return info == null || info.versionName == null ? "" : info.versionName;
    }

    private static PackageInfo installed(Context context) {
        try {
            return context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
    }

    /** SHA-256 of this build's signing certificate, as apksigner prints it. */
    static String installedSigner(Context context) {
        if (Build.VERSION.SDK_INT < 28) return "";
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            Signature[] signers = info.signingInfo == null ? null : info.signingInfo.getApkContentsSigners();
            if (signers == null || signers.length == 0) return "";
            return hex(MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray()));
        } catch (PackageManager.NameNotFoundException | NoSuchAlgorithmException e) {
            return "";
        }
    }

    /** Whether the release would install over this build: same signing certificate. */
    static boolean sameSigner(Context context, Info info) {
        String own = installedSigner(context);
        return info.signer.isEmpty() || own.isEmpty() || own.equalsIgnoreCase(info.signer);
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) text.append(String.format(java.util.Locale.ROOT, "%02x", b & 0xFF));
        return text.toString();
    }

    private static String get(Context context, String address) throws IOException { return get(context, address, LIMIT); }

    private static String get(Context context, String address, int limit) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        try {
            connection.setConnectTimeout(TIMEOUT_MS);
            connection.setReadTimeout(TIMEOUT_MS);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("Accept", "application/json");
            connection.setRequestProperty("User-Agent", context.getPackageName() + "/" + installedName(context));
            int code = connection.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code);
            try (InputStream in = connection.getInputStream(); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[8192];
                for (int n; (n = in.read(buffer)) > 0;) {
                    out.write(buffer, 0, n);
                    if (out.size() > limit) throw new IOException("response is too large");
                }
                return new String(out.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            connection.disconnect();
        }
    }
}
