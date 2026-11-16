package port.launcher;

import android.app.ActivityManager;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import java.util.Locale;

/** A report the player can copy. It names the device model, free and total RAM, and whether the
 *  game is a ZIP or an unpacked folder. It leaves out unique device ids and personal folder paths. */
final class Diagnostics {
    static String report(Context context, String edition, String gpu, String renderer, String textures, String error, String install) {
        String version = "?";
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            version = info.versionName + " (" + (Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode) + ")";
        } catch (PackageManager.NameNotFoundException ignored) { }
        String kind = install == null || install.isEmpty() ? "unknown" : install;
        return "Package: " + context.getPackageName() + "\nVersion: " + version + "\nGame: " + edition
            + "\nDevice: " + deviceName()
            + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
            + "\nABI: " + String.join(", ", Build.SUPPORTED_ABIS)
            + "\nRAM: " + memory(context)
            + "\nGPU: " + gpu + "\nRenderer: " + renderer
            + "\nInstall: " + kind
            + "\nTextures: " + textures + "\nLast error: " + (error == null || error.isEmpty() ? "none" : error);
    }

    /** Manufacturer and model, for example "Xiaomi 2201117PG". A model that already starts with the
     *  manufacturer is kept as it is. This is not a serial number. */
    private static String deviceName() {
        String maker = Build.MANUFACTURER == null ? "" : Build.MANUFACTURER.trim();
        String model = Build.MODEL == null ? "" : Build.MODEL.trim();
        if (maker.equalsIgnoreCase("unknown")) maker = "";
        if (model.isEmpty()) return maker.isEmpty() ? "unknown" : maker;
        if (maker.isEmpty() || model.toLowerCase(Locale.ROOT).startsWith(maker.toLowerCase(Locale.ROOT))) return model;
        return maker + " " + model;
    }

    /** ActivityManager availMem and totalMem, in mebibytes. */
    private static String memory(Context context) {
        ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (manager == null) return "unknown";
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        manager.getMemoryInfo(info);
        return mebibytes(info.availMem) + " MiB free / " + mebibytes(info.totalMem) + " MiB";
    }

    private static long mebibytes(long bytes) {
        if (bytes < 0) return 0;
        return (bytes + (1L << 19)) >> 20;
    }

    static boolean copy(Context context, String report) {
        try {
            ClipboardManager clipboard = context.getSystemService(ClipboardManager.class);
            if (clipboard == null) return false;
            clipboard.setPrimaryClip(ClipData.newPlainText("Launcher diagnostics", report));
            return true;
        } catch (RuntimeException unavailable) { return false; }
    }
}
