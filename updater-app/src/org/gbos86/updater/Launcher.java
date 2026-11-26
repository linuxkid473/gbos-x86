package org.gbos86.updater;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.content.res.Resources;

/**
 * The app's launcher icon (the .LauncherEntry alias) and Google's Settings entry "gbOS86 updates"
 * (Settings > System > Software update, turned on by the overlay usb/display/gbos-updater-settings.apk).
 * The icon may be hidden only while Settings shows the entry, so the app always stays reachable; BootReceiver shows it again
 * when the entry is gone (overlay removed, SETTINGS_UPDATER=0). Android 10+ puts an "App info" placeholder in the app list for a
 * hidden app that requests permissions unless the global setting show_hidden_icon_apps_enabled is 0: the root service sets it
 * (sysupdate.sh request, action launcher).
 */
final class Launcher {
    private Launcher() {}

    static final String SETTINGS = "com.android.settings";

    static ComponentName alias(Context c) { return new ComponentName(c, "org.gbos86.updater.LauncherEntry"); }

    static boolean hidden(Context c) {
        return c.getPackageManager().getComponentEnabledSetting(alias(c)) == PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
    }

    /** Does Google's Settings show "gbOS86 updates" (the overlay is active and points at this app)? */
    static boolean settingsEntry(Context c) {
        try {
            Resources r = c.getPackageManager().getResourcesForApplication(SETTINGS);
            int on = r.getIdentifier("config_additional_system_update_setting_enable", "bool", SETTINGS);
            int pkg = r.getIdentifier("additional_system_update", "string", SETTINGS);
            int cls = r.getIdentifier("additional_system_update_menu", "string", SETTINGS);
            return on != 0 && pkg != 0 && cls != 0 && r.getBoolean(on) && c.getPackageName().equals(r.getString(pkg))
                    && MainActivity.class.getName().equals(r.getString(cls));
        } catch (Exception e) {
            return false;
        }
    }

    /** Hides or shows the icon. Refuses to hide while Settings has no entry. Returns a message for the user. */
    static String setHidden(Context c, boolean hide) {
        if (hide && !settingsEntry(c)) return "Not hidden: Settings has no \"gbOS86 updates\" entry on this system, so the app would be unreachable.";
        c.getPackageManager().setComponentEnabledSetting(alias(c),
                hide ? PackageManager.COMPONENT_ENABLED_STATE_DISABLED : PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
                PackageManager.DONT_KILL_APP);
        String extra = "";
        try {
            Root.Result r = Root.call(c, "launcher", Root.extra("hide", hide ? "1" : "0"), null);
            if (!r.ok) extra = " (the app list may show an \"App info\" placeholder: " + r.body.trim() + ")";
        } catch (Exception e) {
            extra = " (the app list may show an \"App info\" placeholder: " + e.getMessage() + ")";
        }
        return (hide ? "Hidden from the app list. Open it from Settings > System > Software update > gbOS86 updates."
                : "Shown in the app list again.") + extra;
    }

    /** At boot: an icon hidden while Settings no longer has the entry comes back. */
    static void ensureReachable(Context c) {
        if (hidden(c) && !settingsEntry(c))
            c.getPackageManager().setComponentEnabledSetting(alias(c), PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, PackageManager.DONT_KILL_APP);
    }
}
