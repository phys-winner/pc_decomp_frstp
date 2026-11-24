package port.launcher;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import java.util.regex.Pattern;

/** The game-mode tip of the launcher (port.json "app.gameModeNotice", on by default; it began as
 *  the HyperOS tip, hence the name). Xiaomi's HyperOS, Samsung's One UI, the realme/OPPO/OnePlus
 *  systems, vivo's and HONOR's hold the processor and the GPU of an app that is not on their list
 *  of games at power-saving speeds, so a game stutters at any graphics settings until the player
 *  adds it to the system's game tool. The tip names only the tool of the device it runs on. It is
 *  offered once per launcher process, on a start from the icon (LauncherActivity), until the
 *  player chooses "Don't show again"; the launcher's About section turns it back on.
 *  Names: Game Turbo and Game Assistant (with its GT / Pro Gamer modes) were read from the
 *  resources of the apps on a HyperOS 3 and a realme UI 6 device; Gaming Hub / Game Booster,
 *  Ultra Game Mode and Game Manager come from the vendors' support pages. */
final class HyperOs {
    /** The Security app that carries Game Turbo (MIUISecurityCenter, also on HyperOS 2 and 3). */
    static final String SECURITY = "com.miui.securitycenter";
    /** Game Turbo's exported WelcomActivity, without a permission. */
    static final String GAME_TURBO = "com.miui.gamebooster.action.ACCESS_MAINACTIVITY";
    /** Its entry way "00009" makes WelcomActivity open the list for adding games itself
     *  (SelectGameActivity / SelectGameLandActivity). Its own action SELECT_GAME needs a
     *  signatureOrSystem permission and is not used. */
    static final String ENTRY = "track_gamebooster_enter_way", ADD_GAMES = "00009";
    /** Game Turbo's main screen, exported without a permission. */
    static final String GAME_TURBO_MAIN = "com.miui.gamebooster.ui.GameBoosterMainActivity";
    /** State.CHOICES: "1" after "Don't show again". */
    static final String NEVER = "hyperos.notice.never";
    /** Build.VERSION.INCREMENTAL of HyperOS: "OS3.0.3.0.WNLRUXM", "OS2.0.1.0.VNCCNXM"; MIUI's
     *  are "V14.0.5.0.TMRMIXM". */
    private static final Pattern VERSION = Pattern.compile("^OS\\d+\\.\\d+");
    /** Steps of open(): the add-games list, Game Turbo, its main screen, the Security app. */
    static final int STEPS = 4;

    /** Vendors with a game tool that the tip names; NONE: no tip on this device. */
    static final int NONE = 0, XIAOMI = 1, SAMSUNG = 2, OPLUS = 3, VIVO = 4, HONOR = 5;
    /** Samsung's Gaming Hub (Game Launcher before One UI 5.1) and the realme/OPPO/OnePlus Game
     *  Assistant; both are opened by their launch intents. */
    static final String GAMING_HUB = "com.samsung.android.game.gamehome", GAME_ASSISTANT = "com.oplus.games";
    /** Game Assistant's exported main screen (business.module.desktop.JumpSpaceActivity, version 10.38). */
    static final String GAME_ASSISTANT_MAIN = "com.oplus.games.action.desktop.space.main";

    private static boolean shown;

    private HyperOs() { }

    /** The vendor by public API only. Xiaomi counts with HyperOS (detected()); MIUI is left out as before. */
    static int vendor(String manufacturer, String incremental) {
        if (manufacturer == null) return NONE;
        String m = manufacturer.toLowerCase(java.util.Locale.ROOT);
        if (m.equals("xiaomi")) return detected(manufacturer, incremental) ? XIAOMI : NONE;
        if (m.equals("samsung")) return SAMSUNG;
        if (m.equals("realme") || m.equals("oppo") || m.equals("oneplus")) return OPLUS;
        if (m.equals("vivo") || m.equals("iqoo")) return VIVO;
        if (m.equals("honor")) return HONOR;
        return NONE;
    }

    static int vendor() { return vendor(Build.MANUFACTURER, Build.VERSION.INCREMENTAL); }

    /** The system's name for the title: "Smoother play on …". */
    static String system(int vendor, String manufacturer) {
        switch (vendor) {
        case XIAOMI: return "HyperOS";
        case SAMSUNG: return "One UI";
        case OPLUS: return "realme".equalsIgnoreCase(manufacturer) ? "realme UI" : "OnePlus".equalsIgnoreCase(manufacturer) ? "OxygenOS" : "ColorOS";
        case VIVO: return "iQOO".equalsIgnoreCase(manufacturer) ? "iQOO" : "vivo";
        case HONOR: return "MagicOS";
        default: return "";
        }
    }

    /** The tool the "Open …" button names; null: no known way in, and the tip has no such button. */
    static String tool(int vendor, int column) {
        switch (vendor) {
        case XIAOMI: return "Game Turbo";
        case SAMSUNG: return "Gaming Hub";
        case OPLUS: return pick(column, "Game Assistant", "Мастер игр", "Spielassistent", "Asystent gier");
        default: return null;
        }
    }

    /** What the player does, in the launcher's language (a column of Texts.LANGUAGES: en, ru, de,
     *  pl; another column reads English). Product names are the ones the device shows. */
    static String advice(int vendor, int column) {
        switch (vendor) {
        case XIAOMI: return pick(column,
            "add it to Game Turbo (Security app → Game Turbo → add game) and restart the game.",
            "добавьте её в Game Turbo (приложение «Безопасность» → Game Turbo → добавить игру) и перезапустите игру.",
            "fügen Sie es zu Game Turbo hinzu (App „Sicherheit“ → Game Turbo → Spiel hinzufügen) und starten Sie das Spiel neu.",
            "dodaj ją do Game Turbo (aplikacja „Bezpieczeństwo” → Game Turbo → dodaj grę) i uruchom grę ponownie.");
        case SAMSUNG: return pick(column,
            "add it to Gaming Hub (Gaming Hub → ⋮ → add games), select the performance profile in Game Booster and restart the game.",
            "добавьте её в Gaming Hub (Gaming Hub → ⋮ → добавить игры), выберите в Game Booster профиль производительности и перезапустите игру.",
            "fügen Sie es zu Gaming Hub hinzu (Gaming Hub → ⋮ → Spiele hinzufügen), wählen Sie in Game Booster das Leistungsprofil und starten Sie das Spiel neu.",
            "dodaj ją do Gaming Hub (Gaming Hub → ⋮ → dodaj gry), wybierz w Game Booster profil wydajności i uruchom grę ponownie.");
        case OPLUS: return pick(column,
            "add it to Game Assistant (Game Assistant → Add games), turn on GT mode or Pro Gamer mode there and restart the game.",
            "добавьте её в «Мастер игр» («Мастер игр» → «Добавить игры»), включите там «Режим GT» или «игровой режим Pro» и перезапустите игру.",
            "fügen Sie es zum Spielassistenten hinzu (Spielassistent → „Spiele hinzufügen“), schalten Sie dort den GT-Modus oder den Pro Gamer-Modus ein und starten Sie das Spiel neu.",
            "dodaj ją do Asystenta gier (Asystent gier → „Dodaj gry”), włącz tam Tryb GT lub Tryb Pro Gamer i uruchom grę ponownie.");
        case VIVO: return pick(column,
            "add it to Ultra Game Mode (Settings → Ultra Game Mode → add game) and restart the game.",
            "добавьте её в Ultra Game Mode (Настройки → Ultra Game Mode → добавить игру) и перезапустите игру.",
            "fügen Sie es zum Ultra Game Mode hinzu (Einstellungen → Ultra Game Mode → Spiel hinzufügen) und starten Sie das Spiel neu.",
            "dodaj ją do Ultra Game Mode (Ustawienia → Ultra Game Mode → dodaj grę) i uruchom grę ponownie.");
        case HONOR: return pick(column,
            "add it to Game Manager (Settings → Apps → Game Manager → Add games) and restart the game.",
            "добавьте её в Game Manager (Настройки → Приложения → Game Manager → добавить игры) и перезапустите игру.",
            "fügen Sie es zum Game Manager hinzu (Einstellungen → Apps → Game Manager → Spiele hinzufügen) und starten Sie das Spiel neu.",
            "dodaj ją do Game Manager (Ustawienia → Aplikacje → Game Manager → dodaj gry) i uruchom grę ponownie.");
        default: return "";
        }
    }

    private static String pick(int column, String... texts) { return texts[column >= 0 && column < texts.length ? column : 0]; }

    /** HyperOS by public API only: Xiaomi as the manufacturer (any case) and an OS version. */
    static boolean detected(String manufacturer, String incremental) {
        return "Xiaomi".equalsIgnoreCase(manufacturer) && incremental != null && VERSION.matcher(incremental).find();
    }

    static boolean detected() { return detected(Build.MANUFACTURER, Build.VERSION.INCREMENTAL); }

    /** The tip is still to be offered in this process: the port asks for it, the device runs
     *  HyperOS and the player has not turned it off. */
    static boolean due(Context context, Spec spec) {
        return spec.hyperosNotice && !shown && vendor() != NONE && !never(context);
    }

    /** The tip was offered: not again in this process. */
    static void shown() { shown = true; }

    static boolean never(Context context) { return "1".equals(State.get(context, State.CHOICES, NEVER, "0")); }

    static void never(Context context, boolean never) { State.put(context, State.CHOICES, NEVER, never ? "1" : null); }

    /** Opens Game Turbo, trying open()'s steps in order; false when none could start. Game Turbo
     *  may still close at once by its own rules (turned off, not set up yet): nothing tells that
     *  apart, so the tip's text says where Game Turbo is. */
    static boolean open(Activity activity) {
        int vendor = vendor();
        /* Xiaomi: every step. Game Assistant: its exported desktop action (its launcher icon is
           hidden until the player turns it on), then its launch intent. Gaming Hub: the launch intent. */
        for (int step = vendor == XIAOMI ? 0 : vendor == OPLUS ? STEPS - 2 : STEPS - 1; step < STEPS; ++step) {
            Intent intent = vendor == XIAOMI ? intent(activity, step)
                : vendor == SAMSUNG ? activity.getPackageManager().getLaunchIntentForPackage(GAMING_HUB)
                : vendor != OPLUS ? null
                : step == STEPS - 2 ? new Intent(GAME_ASSISTANT_MAIN).setPackage(GAME_ASSISTANT).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                : activity.getPackageManager().getLaunchIntentForPackage(GAME_ASSISTANT);
            if (intent == null) continue;
            try {
                activity.startActivity(intent);
                return true;
            } catch (ActivityNotFoundException | SecurityException e) {
                android.util.Log.w("Launcher", "Game Turbo, step " + (step + 1) + ": " + e);
            }
        }
        return false;
    }

    /** A step's intent; null when the step has nothing to start. In Game Turbo's own task (its
     *  activities have a task affinity of their own), so "back" returns to the launcher. The
     *  implicit intents need no package visibility; the Security app's launch intent does
     *  (the manifest's queries name com.miui.securitycenter when "hyperosNotice" is set). */
    static Intent intent(Context context, int step) {
        switch (step) {
        case 0: return new Intent(GAME_TURBO).setPackage(SECURITY).putExtra(ENTRY, ADD_GAMES).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        case 1: return new Intent(GAME_TURBO).setPackage(SECURITY).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        case 2: return new Intent().setClassName(SECURITY, GAME_TURBO_MAIN).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        default: return context.getPackageManager().getLaunchIntentForPackage(SECURITY);
        }
    }
}
