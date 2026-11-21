package org.gbos86.updater;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Settings, on two tabs: General, and Experimental features (off by default). */
public class SettingsActivity extends Activity {
    private LinearLayout body;
    private Button tabGeneral, tabExperimental;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle("gbOS86 Updater settings");
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabGeneral = tab("General", v -> show(false));
        tabExperimental = tab("Experimental features", v -> show(true));
        tabs.addView(tabGeneral, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        tabs.addView(tabExperimental, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        root.addView(tabs);
        body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(this);
        sv.addView(body);
        root.addView(sv);
        setContentView(root);
        show(saved != null && saved.getBoolean("exp"));
    }

    private boolean onExperimental;

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean("exp", onExperimental);
    }

    private void show(boolean experimental) {
        onExperimental = experimental;
        tabGeneral.setEnabled(experimental);
        tabExperimental.setEnabled(!experimental);
        tabGeneral.setTypeface(null, experimental ? Typeface.NORMAL : Typeface.BOLD);
        tabExperimental.setTypeface(null, experimental ? Typeface.BOLD : Typeface.NORMAL);
        body.removeAllViews();
        if (experimental) experimental(); else general();
    }

    private void general() {
        body.addView(heading("Checking for updates"));
        body.addView(check("Check for updates when the laptop starts", Prefs.CHECK_BOOT, true,
                "After each start, once a network is there, the app looks for a new update and shows a notification if there is one. "
                        + "It never downloads or installs anything without you."));
        body.addView(check("Also check once a day", Prefs.CHECK_DAILY, true, null));
        body.addView(heading("Installing"));
        body.addView(check("If the system partition is short of space, delete old boot logs", Prefs.PRUNE_LOGS, false,
                "The newest 3 boots' logs are always kept."));
        body.addView(heading("Update source"));
        body.addView(text("The GitHub repository whose releases hold gbOS86's updates (OWNER/NAME). Whatever the source, this laptop "
                + "installs only updates signed with the key in its gbos/sysupdate.pub."));
        final EditText src = new EditText(this);
        src.setSingleLine(true);
        src.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        src.setText(Prefs.source(this));
        body.addView(src);
        final TextView srcMsg = text("");
        LinearLayout row = new LinearLayout(this);
        row.addView(button("Save", v -> {
            String s = src.getText().toString().trim();
            if (!Prefs.validSource(s)) { srcMsg.setText("Write it as OWNER/NAME, for example the project's own repository."); return; }
            Prefs.get(this).edit().putString(Prefs.SOURCE, s.equals(BuildInfo.DEFAULT_REPO) ? "" : s).apply();
            srcMsg.setText("Saved.");
        }));
        row.addView(button("Default", v -> {
            Prefs.get(this).edit().remove(Prefs.SOURCE).apply();
            src.setText(Prefs.source(this));
            srcMsg.setText(BuildInfo.DEFAULT_REPO.isEmpty() ? "This app was built without a default source." : "Back to the default.");
        }));
        body.addView(row);
        body.addView(srcMsg);
    }

    private void experimental() {
        body.addView(heading("Experimental features"));
        TextView warn = text("These are new and less tested. They can fail, and they can use a lot of data and space. Leave them off "
                + "unless you need them.");
        warn.setTypeface(null, Typeface.ITALIC);
        body.addView(warn);
        body.addView(check("Download new Google builds directly from Google", Prefs.EXP_GOOGLE, false,
                "Some updates bring a new Google build (the Android system image, super.img). It is too big for GitHub and it is Google's, so "
                        + "it is not in the update. With this on, the app downloads Google's recovery image for that build from dl.google.com, "
                        + "keeps only the parts gbOS86 uses, and rebuilds super.img on this laptop.\n\n"
                        + "- Data: Google's file is read from the start up to the last part needed, several GB (up to about 8 GB). Use a Wi-Fi "
                        + "network without a data cap.\n"
                        + "- An interrupted connection resumes, but if the app is closed or the laptop sleeps, Google's part starts over.\n"
                        + "- Space: about 4 GB free on the data partition while it works, and about 3.8 GB on the system partition (GBOSSYS).\n"
                        + "- The result must match the signed update exactly; the laptop checks it again before installing.\n\n"
                        + "Without this, an update with a new Google build is installed with the USB stick (menu 1)."));
    }

    // ------------------------------------------------------------------------------------------------ helpers
    private View check(String label, final String key, boolean def, String help) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        CheckBox cb = new CheckBox(this);
        cb.setText(label);
        cb.setChecked(Prefs.get(this).getBoolean(key, def));
        cb.setOnCheckedChangeListener((b, on) -> {
            Prefs.get(this).edit().putBoolean(key, on).apply();
            if (key.equals(Prefs.CHECK_BOOT) || key.equals(Prefs.CHECK_DAILY)) CheckJob.schedule(this, false);
        });
        l.addView(cb);
        if (help != null) {
            TextView h = text(help);
            h.setPadding(dp(32), 0, 0, dp(8));
            l.addView(h);
        }
        return l;
    }

    private int dp(int v) { return (int) (v * getResources().getDisplayMetrics().density + 0.5f); }
    private TextView text(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }
    private TextView heading(String s) {
        TextView t = text(s);
        t.setTextSize(18);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(16), 0, dp(4));
        return t;
    }
    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setOnClickListener(l);
        return b;
    }
    private Button tab(String s, View.OnClickListener l) {
        Button b = button(s, l);
        b.setAllCaps(false);
        b.setGravity(Gravity.CENTER);
        return b;
    }
}
