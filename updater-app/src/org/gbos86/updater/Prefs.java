package org.gbos86.updater;

import android.content.Context;
import java.util.List;
import android.content.SharedPreferences;

/** Settings (Settings screen) and small caches. */
final class Prefs {
    private Prefs() {}

    static final String CHECK_BOOT = "check_boot", CHECK_DAILY = "check_daily", PRUNE_LOGS = "prune_logs", SOURCE = "source";
    static final String EXP_GOOGLE = "exp_google_direct";

    static SharedPreferences get(Context c) { return c.getSharedPreferences("settings", Context.MODE_PRIVATE); }

    static boolean checkAtBoot(Context c) { return get(c).getBoolean(CHECK_BOOT, true); }
    static boolean checkDaily(Context c) { return get(c).getBoolean(CHECK_DAILY, true); }
    static boolean pruneLogs(Context c) { return get(c).getBoolean(PRUNE_LOGS, false); }
    /** Rebuild super.img from Google's own download when an update brings a new Google build (Settings > General > Google builds). */
    /** The warning codes the user accepted with "Install anyway" for the download in progress (null: there was no warning). */
    static String acceptedWarning(Context c) { return get(c).getString("accepted_warning", null); }
    static void setAcceptedWarning(Context c, List<String> codes) {
        get(c).edit().putString("accepted_warning", codes == null || codes.isEmpty() ? null : String.join(",", codes)).apply();
    }
    /** On by default since 2026-11-23 (was an experimental opt-in); a user who turned it off keeps it off. */
    static final boolean GOOGLE_DIRECT_DEFAULT = true;
    static boolean googleDirect(Context c) { return get(c).getBoolean(EXP_GOOGLE, GOOGLE_DIRECT_DEFAULT); }

    /** OWNER/NAME of the GitHub repository whose releases hold the feed. Default: the repository the app was built from. */
    static String source(Context c) {
        String s = get(c).getString(SOURCE, "");
        return s.isEmpty() ? BuildInfo.DEFAULT_REPO : s;
    }

    static boolean validSource(String s) { return s.matches("[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}"); }

    static String feedUrl(Context c) {
        String s = source(c);
        return validSource(s) ? "https://github.com/" + s + "/releases/download/sysupdate-feed/gbos86-update.feed" : null;
    }
}
