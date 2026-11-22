package org.gbos86.updater.ui;

import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Build;

/**
 * Material Design 3 (M3 / Material You) dynamic color tokens.
 * Adapts to system night mode and queries Android 12+ (API 31+) system dynamic colors
 * when available, falling back gracefully to Google Pixel M3 baseline tones.
 */
public final class M3Theme {
    private M3Theme() {}

    public static boolean isDark(Context c) {
        int uiMode = c.getResources().getConfiguration().uiMode;
        return (uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
    }

    private static int getSysColor(Context c, String name, int fallback) {
        if (Build.VERSION.SDK_INT >= 31) {
            try {
                int id = c.getResources().getIdentifier(name, "color", "android");
                if (id != 0) return c.getColor(id);
            } catch (Exception ignored) {}
        }
        return fallback;
    }

    public static int surface(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral1_900", Color.parseColor("#111318"))
                : getSysColor(c, "system_neutral1_50", Color.parseColor("#F9F9FC"));
    }

    public static int surfaceContainer(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral1_800", Color.parseColor("#1D2024"))
                : getSysColor(c, "system_neutral1_100", Color.parseColor("#EEF2F6"));
    }

    public static int surfaceContainerHigh(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral1_700", Color.parseColor("#282A2F"))
                : getSysColor(c, "system_neutral1_200", Color.parseColor("#E5E9EE"));
    }

    public static int surfaceContainerHighest(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral2_700", Color.parseColor("#33353A"))
                : getSysColor(c, "system_neutral2_200", Color.parseColor("#DCE1E7"));
    }

    public static int onSurface(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral1_100", Color.parseColor("#E2E2E6"))
                : getSysColor(c, "system_neutral1_900", Color.parseColor("#191C20"));
    }

    public static int onSurfaceVariant(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral2_200", Color.parseColor("#C4C7D0"))
                : getSysColor(c, "system_neutral2_700", Color.parseColor("#43474E"));
    }

    public static int outline(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral2_400", Color.parseColor("#8E9099"))
                : getSysColor(c, "system_neutral2_500", Color.parseColor("#73777F"));
    }

    public static int outlineVariant(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_neutral2_800", Color.parseColor("#44474F"))
                : getSysColor(c, "system_neutral2_100", Color.parseColor("#C3C7D0"));
    }

    public static int primary(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_accent1_200", Color.parseColor("#A8C7FA"))
                : getSysColor(c, "system_accent1_600", Color.parseColor("#0B57D0"));
    }

    public static int onPrimary(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_accent1_800", Color.parseColor("#062E6F"))
                : Color.WHITE;
    }

    public static int primaryContainer(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_accent1_700", Color.parseColor("#0842A0"))
                : getSysColor(c, "system_accent1_100", Color.parseColor("#D3E3FD"));
    }

    public static int onPrimaryContainer(Context c) {
        return isDark(c)
                ? getSysColor(c, "system_accent1_100", Color.parseColor("#D3E3FD"))
                : getSysColor(c, "system_accent1_900", Color.parseColor("#041E49"));
    }

    public static int success(Context c) {
        return isDark(c) ? Color.parseColor("#6DD58C") : Color.parseColor("#146C2E");
    }

    public static int successContainer(Context c) {
        return isDark(c) ? Color.parseColor("#0F5223") : Color.parseColor("#C4EED0");
    }

    public static int onSuccessContainer(Context c) {
        return isDark(c) ? Color.parseColor("#C4EED0") : Color.parseColor("#00210B");
    }

    public static int warning(Context c) {
        return isDark(c) ? Color.parseColor("#FFBA38") : Color.parseColor("#8D5E00");
    }

    public static int warningContainer(Context c) {
        return isDark(c) ? Color.parseColor("#583E00") : Color.parseColor("#FFDF99");
    }

    public static int onWarningContainer(Context c) {
        return isDark(c) ? Color.parseColor("#FFDF99") : Color.parseColor("#261900");
    }

    public static int error(Context c) {
        return isDark(c) ? Color.parseColor("#FFB4AB") : Color.parseColor("#BA1A1A");
    }

    public static int errorContainer(Context c) {
        return isDark(c) ? Color.parseColor("#93000A") : Color.parseColor("#FFDAD6");
    }

    public static int onErrorContainer(Context c) {
        return isDark(c) ? Color.parseColor("#FFDAD6") : Color.parseColor("#410002");
    }
}
