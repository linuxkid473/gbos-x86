package org.gbos86.updater;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.gbos86.updater.ui.M3;
import org.gbos86.updater.ui.M3Theme;

/**
 * Settings (Material 3 Edition):
 * Organised into General and Experimental categories via M3 Segmented Buttons,
 * housed in surface container cards.
 */
public class SettingsActivity extends Activity {
    private LinearLayout contentCol;
    private boolean onExperimental;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = M3.dp(this, 16);
        root.setPadding(pad, pad, pad, pad * 2);

        // ---------------------------------------------------------------- Top App Header with Back
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(0, M3.dp(this, 8), 0, M3.dp(this, 16));

        Button backBtn = M3.tonalButton(this, "← Back", v -> finish());
        header.addView(backBtn);

        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        titleCol.setPadding(M3.dp(this, 16), 0, 0, 0);
        titleCol.addView(M3.eyebrow(this, "PREFERENCES"));
        titleCol.addView(M3.headline(this, "Settings"));
        header.addView(titleCol, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        root.addView(header);

        // ---------------------------------------------------------------- Segmented Control (Tabs)
        LinearLayout tabs = M3.segmentedControl(this, new String[] {"General", "Experimental"},
                (saved != null && saved.getBoolean("exp")) ? 1 : 0, index -> show(index == 1));
        root.addView(tabs);
        addMarginBottom(tabs, 16);

        // ---------------------------------------------------------------- Content Container
        contentCol = new LinearLayout(this);
        contentCol.setOrientation(LinearLayout.VERTICAL);
        root.addView(contentCol);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(M3Theme.surface(this));
        View scaffold = M3.responsiveScaffold(this, root, 720);
        scroll.addView(scaffold);
        setContentView(scroll);

        show(saved != null && saved.getBoolean("exp"));
    }

    private void addMarginBottom(View v, int dp) {
        LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) v.getLayoutParams();
        if (lp == null) lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = M3.dp(this, dp);
        v.setLayoutParams(lp);
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean("exp", onExperimental);
    }

    private void show(boolean exp) {
        onExperimental = exp;
        contentCol.removeAllViews();
        if (exp) experimental();
        else general();
    }

    // ---------------------------------------------------------------- General Settings Tab
    private void general() {
        // Card 1: Automated Checks
        LinearLayout schedCard = M3.card(this, 20);
        schedCard.addView(M3.eyebrow(this, "AUTOMATED CHECKS"));
        TextView schedTitle = M3.titleLarge(this, "Background Updates");
        schedTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 8));
        schedCard.addView(schedTitle);

        schedCard.addView(checkTile("Check for updates at boot", Prefs.CHECK_BOOT, true,
                "Checks the signed feed automatically when the laptop finishes starting."));
        schedCard.addView(M3.divider(this));
        schedCard.addView(checkTile("Check once a day", Prefs.CHECK_DAILY, true,
                "Schedules a low-power background check every 24 hours."));
        contentCol.addView(schedCard);
        addMarginBottom(schedCard, 16);

        // Card 2: Storage Maintenance
        LinearLayout storageCard = M3.card(this, 20);
        storageCard.addView(M3.eyebrow(this, "STORAGE MANAGEMENT"));
        TextView storTitle = M3.titleLarge(this, "Disk Maintenance");
        storTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 8));
        storageCard.addView(storTitle);

        storageCard.addView(checkTile("Prune old boot logs when space is tight", Prefs.PRUNE_LOGS, false,
                "Deletes older boot logs from GBOSSYS if needed before staging an update. The newest 3 boots' logs are always kept."));
        contentCol.addView(storageCard);
        addMarginBottom(storageCard, 16);

        // Card: Google builds (the Android system image, super.img), from Google's own download
        LinearLayout featCard = M3.card(this, 20);
        featCard.addView(M3.eyebrow(this, "GOOGLE BUILDS"));
        TextView featTitle = M3.titleLarge(this, "Google system image");
        featTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 8));
        featCard.addView(featTitle);

        featCard.addView(checkTile("Download new Google builds from Google", Prefs.EXP_GOOGLE, Prefs.GOOGLE_DIRECT_DEFAULT,
                "Some updates bring a new Google build (the Android system image, super.img). It is Google's, so it is not on GitHub: "
                        + "the app reads Google's official recovery image from dl.google.com, takes only the partitions it needs, and "
                        + "rebuilds super.img on this laptop. The update's own patches for that build come signed with the update.\n\n"
                        + "Before it starts, the app shows how much it will download and asks you.\n"
                        + "• Network: several GB (Google's file is about 7.7 GB; it is read up to the last partition needed). Use Wi-Fi "
                        + "without a data limit.\n"
                        + "• Storage: about 4 GB free on this laptop's data partition while it works, and about 3.8 GB on GBOSSYS.\n"
                        + "• Time: often an hour or more. Keep the laptop plugged in and awake; if the app is closed, Google's part starts over.\n"
                        + "• Safety: every partition and the result must match the hashes in the signed update before anything is installed.\n\n"
                        + "Turned off, an update that needs a new Google build is installed with the USB stick (menu 1)."));
        contentCol.addView(featCard);
        addMarginBottom(featCard, 16);

        // Card 3: Update Source Feed
        LinearLayout sourceCard = M3.card(this, 20);
        sourceCard.addView(M3.eyebrow(this, "UPDATE SOURCE"));
        TextView srcTitle = M3.titleLarge(this, "Release Repository");
        srcTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 4));
        sourceCard.addView(srcTitle);

        TextView srcDesc = M3.body(this, "The GitHub repository whose releases hold gbOS86's updates (OWNER/NAME). Whatever the source, this laptop "
                + "installs only updates signed with the key in its gbos/sysupdate.pub.");
        srcDesc.setPadding(0, 0, 0, M3.dp(this, 12));
        sourceCard.addView(srcDesc);

        final EditText src = new EditText(this);
        src.setSingleLine(true);
        src.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        src.setText(Prefs.source(this));
        src.setTextColor(M3Theme.onSurface(this));
        src.setTextSize(14);
        int epad = M3.dp(this, 14);
        src.setPadding(epad, epad, epad, epad);
        src.setBackground(M3.roundRectBorder(M3Theme.surfaceContainerHigh(this), M3Theme.outlineVariant(this), M3.dp(this, 1), M3.dp(this, 12)));
        sourceCard.addView(src);

        final TextView srcMsg = M3.bodySmall(this, "");
        srcMsg.setPadding(0, M3.dp(this, 6), 0, M3.dp(this, 10));

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setPadding(0, M3.dp(this, 12), 0, 0);

        Button saveBtn = M3.filledButton(this, "Save source", v -> {
            String s = src.getText().toString().trim();
            if (!Prefs.validSource(s)) {
                srcMsg.setText("Enter as OWNER/NAME, for example the project's own repository.");
                srcMsg.setTextColor(M3Theme.error(this));
                return;
            }
            Prefs.get(this).edit().putString(Prefs.SOURCE, s.equals(BuildInfo.DEFAULT_REPO) ? "" : s).apply();
            srcMsg.setText("✓ Saved.");
            srcMsg.setTextColor(M3Theme.success(this));
        });

        Button defBtn = M3.outlinedButton(this, "Restore default", v -> {
            Prefs.get(this).edit().remove(Prefs.SOURCE).apply();
            src.setText(Prefs.source(this));
            srcMsg.setText(BuildInfo.DEFAULT_REPO.isEmpty() ? "This app was built without a default source." : "Restored to default source.");
            srcMsg.setTextColor(M3Theme.onSurfaceVariant(this));
        });

        btnRow.addView(saveBtn);
        LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) saveBtn.getLayoutParams();
        lp.setMarginEnd(M3.dp(this, 10));
        saveBtn.setLayoutParams(lp);
        btnRow.addView(defBtn);

        sourceCard.addView(btnRow);
        sourceCard.addView(srcMsg);

        contentCol.addView(sourceCard);
    }

    // ---------------------------------------------------------------- Experimental Tab
    private void experimental() {
        // Warning Banner Card
        LinearLayout warnCard = M3.card(this, 20);
        warnCard.setBackground(M3.roundRectBorder(M3Theme.warningContainer(this), M3Theme.warning(this), M3.dp(this, 1), M3.dp(this, 20)));

        TextView warnEyebrow = M3.eyebrow(this, "EXPERIMENTAL FEATURES");
        warnEyebrow.setTextColor(M3Theme.onWarningContainer(this));
        warnCard.addView(warnEyebrow);

        TextView warnTitle = M3.titleLarge(this, "Proceed with Caution");
        warnTitle.setTextColor(M3Theme.onWarningContainer(this));
        warnTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 6));
        warnCard.addView(warnTitle);

        TextView warn = M3.body(this, "These features are new and less tested. They can consume substantial network data and disk space. "
                + "Leave them disabled unless you specifically require them.");
        warn.setTextColor(M3Theme.onWarningContainer(this));
        warnCard.addView(warn);
        contentCol.addView(warnCard);
        addMarginBottom(warnCard, 16);

        TextView none = M3.body(this, "No experimental features right now. Downloading new Google builds from Google is now under "
                + "General > Google builds.");
        none.setPadding(0, M3.dp(this, 4), 0, 0);
        contentCol.addView(none);
    }

    // ---------------------------------------------------------------- UI Helpers
    private View checkTile(String label, final String key, boolean def, String help) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(0, M3.dp(this, 6), 0, M3.dp(this, 6));

        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setTextSize(15);
        cb.setTextColor(M3Theme.onSurface(this));
        cb.setChecked(Prefs.get(this).getBoolean(key, def));
        cb.setOnCheckedChangeListener((b, on) -> {
            Prefs.get(this).edit().putBoolean(key, on).apply();
            if (key.equals(Prefs.CHECK_BOOT) || key.equals(Prefs.CHECK_DAILY)) CheckJob.schedule(this, false);
        });
        l.addView(cb);

        if (help != null) {
            TextView h = M3.bodySmall(this, help);
            h.setPadding(M3.dp(this, 32), M3.dp(this, 2), 0, M3.dp(this, 4));
            l.addView(h);
        }
        return l;
    }
}
