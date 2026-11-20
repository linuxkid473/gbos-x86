package org.gbos86.updater;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.gbos86.updater.core.Feed;
import org.gbos86.updater.core.Util;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * gbOS86 Updater. Not a trust anchor: it never installs anything itself. It finds updates (the signed feed on the project's GitHub
 * releases), downloads them, and asks the root service (sysupdate.sh request) to copy them into GBOSSYS's update.inbox. The next
 * boot's stage0 checks the signature and every file again before anything changes, and rolls back an update that does not start.
 * It also shows whether the installed system matches the published sources (the modification check).
 */
public class MainActivity extends Activity {
    private static final int PICK_FOLDER = 1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView laptopView, integrityView, updateView, notesView, progressView, folderView;
    private Button checkButton, installButton, stopButton, rebootButton, auditButton, deepButton, pickButton, stageFolderButton, cancelButton;
    private ProgressBar bar;
    private String relPath;
    private volatile boolean busy;
    private static volatile Updater.Info info;
    private static volatile Updater.Audit audit;
    private static volatile Updater.Decision decision;
    private boolean visible;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        int pad = dp(16);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(pad, pad, pad, pad);

        col.addView(heading("This laptop"));
        laptopView = text("Asking the system ...");
        col.addView(laptopView);
        integrityView = text("");
        col.addView(integrityView);
        LinearLayout ar = row();
        auditButton = button("Check for changes", v -> runAudit(false));
        deepButton = button("Full check (slower)", v -> runAudit(true));
        ar.addView(auditButton);
        ar.addView(deepButton);
        col.addView(ar);

        col.addView(heading("Updates"));
        updateView = text("");
        col.addView(updateView);
        notesView = text("");
        col.addView(notesView);
        LinearLayout ur = row();
        checkButton = button("Check for updates", v -> refresh(true));
        installButton = button("Download and install", v -> confirmInstall());
        ur.addView(checkButton);
        ur.addView(installButton);
        col.addView(ur);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        col.addView(bar);
        progressView = text("");
        progressView.setTypeface(Typeface.MONOSPACE);
        col.addView(progressView);
        LinearLayout pr = row();
        stopButton = button("Stop", v -> UpdateService.stop());
        rebootButton = button("Restart now", v -> confirmReboot());
        pr.addView(stopButton);
        pr.addView(rebootButton);
        col.addView(pr);

        col.addView(heading("Install from a folder"));
        col.addView(text("An update folder (gbos86-update-...) you copied to this laptop yourself, for example to Downloads."));
        pickButton = button("Choose update folder", v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER));
        col.addView(pickButton);
        folderView = text("");
        col.addView(folderView);
        stageFolderButton = button("Copy this folder to the system", v -> confirmStageFolder());
        col.addView(stageFolderButton);

        col.addView(heading("Other"));
        LinearLayout or = row();
        cancelButton = button("Remove waiting updates", v -> simple("cancel"));
        or.addView(cancelButton);
        or.addView(button("Settings", v -> startActivity(new Intent(this, SettingsActivity.class))));
        col.addView(or);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(col);
        setContentView(scroll);
        Notifier.channels(this);
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 2);
        CheckJob.schedule(this, false);
        Notifier.cancelAvailable(this);
        refresh(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        visible = true;
        tick();
    }

    @Override
    protected void onPause() {
        super.onPause();
        visible = false;
    }

    /** Twice a second while visible: the download service's progress and the buttons. */
    private void tick() {
        if (!visible) return;
        boolean run = UpdateService.State.running;
        if (run || UpdateService.State.done || UpdateService.State.failed) {
            long t = UpdateService.State.total, d = UpdateService.State.progress;
            bar.setIndeterminate(run && t <= 0);
            bar.setProgress(t > 0 ? (int) (d * 1000 / t) : (run ? 0 : 1000));
            String s = run ? UpdateService.State.phase + (t > 0 ? "\n" + Util.size(d) + " of " + Util.size(t) : "") : UpdateService.State.message;
            if (!s.equals(progressView.getText().toString())) progressView.setText(s);
        }
        stopButton.setVisibility(run ? View.VISIBLE : View.GONE);
        bar.setVisibility(run ? View.VISIBLE : View.GONE);
        boolean waiting = UpdateService.State.done || (decision != null && decision.kind == Updater.Kind.WAITING);
        rebootButton.setVisibility(waiting ? View.VISIBLE : View.GONE);
        installButton.setVisibility(!run && decision != null && decision.kind == Updater.Kind.AVAILABLE && !UpdateService.State.done ? View.VISIBLE : View.GONE);
        for (Button b : new Button[] {checkButton, auditButton, deepButton, pickButton, cancelButton}) b.setEnabled(!busy && !run);
        stageFolderButton.setEnabled(!busy && !run && relPath != null);
        if (UpdateService.State.done && run != wasRunning) refresh(false);
        wasRunning = run;
        ui.postDelayed(this::tick, 500);
    }
    private boolean wasRunning;

    // ------------------------------------------------------------------------------------------------ the checks
    private void refresh(boolean userAsked) {
        if (busy) return;
        busy = true;
        if (userAsked) updateView.setText("Checking ...");
        new Thread(() -> {
            try {
                Updater.Info i = Updater.info(this);
                info = i;
                ui.post(this::showLaptop);
                Updater.Audit a = audit;
                if (a == null || !a.set.equals(i.set) || System.currentTimeMillis() - a.time > 6 * 3600 * 1000L) {
                    ui.post(() -> integrityView.setText("Checking for modifications ..."));
                    try {
                        audit = Updater.audit(this, false);
                    } catch (Exception e) {
                        ui.post(() -> integrityView.setText("The modification check failed: " + e.getMessage()));
                    }
                    ui.post(this::showIntegrity);
                } else ui.post(this::showIntegrity);
                try {
                    Feed f = Updater.fetchFeed(this, i);
                    Updater.Decision d = Updater.decide(this, i, audit, f);
                    decision = d;
                    Prefs.get(this).edit().putString("last_check", d.message).putLong("last_check_time", System.currentTimeMillis()).apply();
                    ui.post(() -> {
                        updateView.setText(d.message);
                        notesView.setText(f.notes().isEmpty() || d.kind == Updater.Kind.UP_TO_DATE ? "" : "What's new:\n- " + String.join("\n- ", f.notes()));
                    });
                } catch (Exception e) {
                    decision = null;
                    ui.post(() -> { updateView.setText(e.getMessage()); notesView.setText(""); });
                }
            } catch (Root.NoService e) {
                ui.post(() -> laptopView.setText(e.getMessage()));
            } catch (Exception e) {
                ui.post(() -> laptopView.setText("Failed: " + e.getMessage()));
            } finally {
                busy = false;
            }
        }).start();
    }

    private void showLaptop() {
        Updater.Info i = info;
        if (i == null) return;
        StringBuilder sb = new StringBuilder();
        sb.append(i.seq > 0 ? "gbOS86 update " + i.seq : "gbOS86 (installed from the USB stick)");
        sb.append(", Google build ").append(i.googleBuild > 0 ? i.googleBuild : "unknown");
        if (i.freeKb >= 0) sb.append("\nFree on the system partition: ").append(Util.size(i.freeKb * 1024));
        if (!i.hasKey()) sb.append("\nNo update key installed: updates are refused. Update once with the USB stick.");
        if (!"ok".equals(i.hook)) sb.append("\nThe boot hook that installs updates is missing: update once with the USB stick (menu 1).");
        if (!i.last.isEmpty()) sb.append("\nLast result: ").append(i.last);
        laptopView.setText(sb.toString());
    }

    private void showIntegrity() {
        Updater.Audit a = audit;
        if (a == null || info == null) return;
        String w = Updater.warning(a);
        String s = Updater.describe(a, info) + (w != null ? "\n\n⚠ " + w : "");
        integrityView.setText(s);
        integrityView.setTextColor(w != null ? Color.rgb(0xd0, 0x6b, 0x00) : laptopView.getCurrentTextColor());
    }

    private void runAudit(boolean deep) {
        if (busy) return;
        busy = true;
        integrityView.setText(deep ? "Checking every file, including super.img (a minute or two) ..." : "Checking for modifications ...");
        new Thread(() -> {
            try {
                audit = Updater.audit(this, deep);
                ui.post(this::showIntegrity);
            } catch (Exception e) {
                ui.post(() -> integrityView.setText("The check failed: " + e.getMessage()));
            } finally {
                busy = false;
            }
        }).start();
    }

    // ------------------------------------------------------------------------------------------------ install
    private void confirmInstall() {
        final Updater.Decision d = decision;
        if (d == null || d.kind != Updater.Kind.AVAILABLE) return;
        final Updater.Audit a = audit;
        String w = Updater.warning(a);
        if (w != null) {
            new AlertDialog.Builder(this)
                    .setTitle("⚠ Modified system")
                    .setMessage(w + "\n\nInstall the update anyway?")
                    .setPositiveButton("Install anyway", (x, y) -> { Prefs.setAcceptedWarning(this, Updater.warningCodes(a)); confirmDownload(d); })
                    .setNegativeButton("Cancel", null)
                    .show();
        } else {
            Prefs.setAcceptedWarning(this, null);
            confirmDownload(d);
        }
    }

    private void confirmDownload(Updater.Decision d) {
        String msg = d.message + "\n\nIt is downloaded now and installed at the next restart, after its signature and every file are checked "
                + "again. Your apps and data are kept. If the new system does not finish starting, the previous one comes back by itself.";
        if (d.needsSuper)
            msg += "\n\nExperimental: the new Google build is downloaded from Google and super.img is rebuilt here. Use Wi-Fi; this can take "
                    + "a long time. Keep the laptop plugged in and awake.";
        new AlertDialog.Builder(this)
                .setTitle("Download update " + d.feed.seq + "?")
                .setMessage(msg)
                .setPositiveButton("Download", (x, y) -> { UpdateService.start(this, d); progressView.setText("Starting ..."); })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void confirmReboot() {
        new AlertDialog.Builder(this)
                .setTitle("Restart now?")
                .setMessage("The waiting update is checked and installed while the laptop starts. Save your work first.")
                .setPositiveButton("Restart", (x, y) -> simple("reboot"))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void simple(String action) {
        if (busy) return;
        busy = true;
        progressView.setText("Asking the system ...");
        new Thread(() -> {
            String r;
            try {
                Root.Result res = Root.call(this, action, null, null);
                r = (res.ok ? "" : "Failed: ") + res.body;
            } catch (Exception e) {
                r = "Failed: " + e.getMessage();
            }
            final String s = r;
            ui.post(() -> progressView.setText(s));
            busy = false;
            if (!"reboot".equals(action)) ui.post(() -> refresh(false));
        }).start();
    }

    // ------------------------------------------------------------------------------------------------ install from a folder
    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        relPath = null;
        String docId;
        try {
            docId = DocumentsContract.getTreeDocumentId(tree);
        } catch (Exception e) {
            folderView.setText("This folder cannot be used: " + e.getMessage());
            return;
        }
        // The root service reads the folder directly from /data/media/<user>/, so only this device's own shared storage works.
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority()) || !docId.startsWith("primary:")) {
            folderView.setText("Choose a folder in this device's own storage (for example Downloads). USB drives and SD cards "
                    + "are not supported here: copy the folder to Downloads first, or from Windows into update.inbox on GBOSSYS.");
            return;
        }
        String rel = docId.substring("primary:".length());
        if (rel.isEmpty() || !rel.matches(Root.ALLOWED) || ("/" + rel + "/").contains("/../") || ("/" + rel + "/").contains("/./")) {
            folderView.setText("The folder's name or path has characters the updater does not accept: " + rel);
            return;
        }
        String summary;
        try {
            summary = readManifestSummary(DocumentsContract.buildDocumentUriUsingTree(tree, docId + "/manifest"));
        } catch (Exception e) {
            folderView.setText("No update manifest in \"" + rel + "\". Choose the gbos86-update-... folder itself.");
            return;
        }
        relPath = rel;
        folderView.setText(rel + "\n" + summary + "\n(not checked yet: the signature is checked when it is copied, and again at restart)");
    }

    private String readManifestSummary(Uri manifest) throws Exception {
        StringBuilder sb = new StringBuilder();
        try (InputStream in = getContentResolver().openInputStream(manifest);
             BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.US_ASCII))) {
            if (!"gbos86-system-set 1".equals(r.readLine())) throw new Exception("not a gbOS86 system set");
            for (int i = 0; i < 5; i++) {
                String l = r.readLine();
                if (l == null) break;
                if (l.startsWith("seq ")) sb.append("update ").append(l.substring(4)).append(", ");
                else if (l.startsWith("google_build ")) sb.append("Google build ").append(l.substring(13)).append(", ");
                else if (l.startsWith("commit ")) sb.append("commit ").append(l.substring(7, Math.min(l.length(), 19))).append(", ");
                else if (l.startsWith("created ")) sb.append("made ").append(l.substring(8));
            }
        }
        return sb.toString();
    }

    private void confirmStageFolder() {
        if (relPath == null) return;
        final List<String> codes = Updater.warningCodes(audit);
        String w = Updater.warning(audit);
        new AlertDialog.Builder(this)
                .setTitle(w != null ? "⚠ Modified system" : "Copy this update?")
                .setMessage((w != null ? w + "\n\n" : "") + "It is copied to the system partition now and installed at the next restart. "
                        + "Your apps and data are kept.")
                .setPositiveButton(w != null ? "Copy anyway" : "Copy", (x, y) -> stageFolder(codes.isEmpty() ? null : String.join(",", codes)))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void stageFolder(final String accepted) {
        if (busy) return;
        busy = true;
        final String rel = relPath;
        progressView.setText("Asking the system ...");
        new Thread(() -> {
            String r;
            try {
                Root.Result res = Root.call(this, "stage", Root.extra("path", rel, "where", "shared", "prune_logs", Prefs.pruneLogs(this) ? "1" : null,
                        "accepted_warning", accepted),
                        body -> ui.post(() -> progressView.setText(body)));
                r = (res.ok ? "" : "Failed:\n") + res.body;
            } catch (Exception e) {
                r = "Failed: " + e.getMessage();
            }
            final String s = r;
            ui.post(() -> progressView.setText(s));
            busy = false;
            ui.post(() -> refresh(false));
        }).start();
    }

    // ------------------------------------------------------------------------------------------------ UI helpers
    private int dp(int v) { return (int) (v * getResources().getDisplayMetrics().density + 0.5f); }
    private TextView text(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextIsSelectable(true);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }
    private TextView heading(String s) {
        TextView t = text(s);
        t.setTextSize(20);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(16), 0, dp(4));
        return t;
    }
    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }
    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        return r;
    }
}
