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
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import org.gbos86.updater.core.Feed;
import org.gbos86.updater.core.Util;
import org.gbos86.updater.ui.M3;
import org.gbos86.updater.ui.M3Theme;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * gbOS86 Updater (Material 3 Edition).
 * Not a trust anchor: it never installs anything itself. It finds updates (the signed feed on the project's GitHub
 * releases), downloads them, and asks the root service (sysupdate.sh request) to copy them into GBOSSYS's update.inbox.
 * The next boot's stage0 checks the signature and every file again before anything changes, and rolls back an update
 * that does not start. It also shows whether the installed system matches the published sources (the modification check).
 */
public class MainActivity extends Activity {
    private static final int PICK_FOLDER = 1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView updateStatusTitle, updateStatusSubtitle, notesView, progressView;
    private TextView laptopView, integrityView, integrityBadge, folderView, folderSummaryView;
    private TextView googleTitle, googleDetail, googleStatus;
    private Button googleCheckButton, googleInstallButton;
    private long finishingSince;
    private Button checkButton, installButton, stopButton, rebootButton;
    private Button auditButton, deepButton, pickButton, stageFolderButton, cancelButton;
    private ProgressBar bar;
    private LinearLayout notesContainer, folderContainer, actionCard;

    private String relPath;
    private volatile boolean busy;
    private static volatile Updater.Info info;
    private static volatile Updater.Audit audit;
    private static volatile Updater.Decision decision;
    private boolean visible;
    private boolean wasRunning;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = M3.dp(this, 16);
        root.setPadding(pad, pad, pad, pad * 2);

        // ---------------------------------------------------------------- Top App Header: banner, then title and Settings
        boolean wide = M3.wide(this);
        ImageView banner = M3.banner(this, R.drawable.gbos_header, wide ? 4.2f : 2.6f, 20);
        root.addView(banner, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(M3.dp(this, 4), M3.dp(this, 16), 0, M3.dp(this, 16));
        header.addView(M3.headline(this, "System Update"), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        Button settingsBtn = M3.tonalButton(this, "Settings", v -> startActivity(new Intent(this, SettingsActivity.class)));
        header.addView(settingsBtn);
        root.addView(header);

        // ---------------------------------------------------------------- Hero Card: Updates
        LinearLayout updateCard = M3.card(this, 20);
        updateCard.addView(M3.eyebrow(this, "UPDATE STATUS"));

        updateStatusTitle = M3.titleLarge(this, "Checking for updates ...");
        updateStatusTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 2));
        updateCard.addView(updateStatusTitle);

        updateStatusSubtitle = M3.body(this, "");
        updateStatusSubtitle.setTextIsSelectable(true);
        updateCard.addView(updateStatusSubtitle);

        // Notes container
        notesContainer = new LinearLayout(this);
        notesContainer.setOrientation(LinearLayout.VERTICAL);
        notesContainer.setPadding(M3.dp(this, 12), M3.dp(this, 10), M3.dp(this, 12), M3.dp(this, 10));
        notesContainer.setBackground(M3.roundRect(M3Theme.surfaceContainerHigh(this), M3.dp(this, 12)));
        LinearLayout.LayoutParams nclp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        nclp.setMargins(0, M3.dp(this, 10), 0, M3.dp(this, 10));
        notesContainer.setLayoutParams(nclp);

        TextView notesHeader = M3.titleMedium(this, "What's new");
        notesHeader.setTextSize(14);
        notesContainer.addView(notesHeader);

        notesView = M3.body(this, "");
        notesView.setTextIsSelectable(true);
        notesContainer.addView(notesView);
        notesContainer.setVisibility(View.GONE);
        updateCard.addView(notesContainer);

        // Progress bar and details
        bar = M3.progressBar(this);
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, M3.dp(this, 8));
        blp.setMargins(0, M3.dp(this, 12), 0, M3.dp(this, 6));
        bar.setLayoutParams(blp);
        bar.setVisibility(View.GONE);
        updateCard.addView(bar);

        progressView = M3.bodySmall(this, "");
        progressView.setTypeface(Typeface.MONOSPACE);
        progressView.setTextIsSelectable(true);
        updateCard.addView(progressView);

        // Action Buttons Row inside Hero Card
        LinearLayout updateActions = new LinearLayout(this);
        updateActions.setOrientation(LinearLayout.HORIZONTAL);
        updateActions.setPadding(0, M3.dp(this, 14), 0, 0);

        checkButton = M3.tonalButton(this, "Check for updates", v -> refresh(true));
        installButton = M3.filledButton(this, "Download and install", v -> confirmInstall());
        stopButton = M3.outlinedButton(this, "Stop download", v -> UpdateService.stop());
        rebootButton = M3.filledButton(this, "Restart now", v -> confirmReboot());

        installButton.setVisibility(View.GONE);
        stopButton.setVisibility(View.GONE);
        rebootButton.setVisibility(View.GONE);

        updateActions.addView(checkButton);
        addMarginEnd(checkButton, 10);
        updateActions.addView(installButton);
        addMarginEnd(installButton, 10);
        updateActions.addView(rebootButton);
        addMarginEnd(rebootButton, 10);
        updateActions.addView(stopButton);

        updateCard.addView(updateActions);

        // ---------------------------------------------------------------- Card 2: This Laptop & Integrity
        LinearLayout deviceCard = M3.card(this, 20);
        deviceCard.addView(M3.eyebrow(this, "THIS LAPTOP"));

        TextView deviceTitle = M3.titleLarge(this, "Installed System");
        deviceTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 4));
        deviceCard.addView(deviceTitle);

        laptopView = M3.body(this, "Asking the system ...");
        laptopView.setTextIsSelectable(true);
        deviceCard.addView(laptopView);

        deviceCard.addView(M3.divider(this));

        // Integrity status header & badge
        LinearLayout intHeader = new LinearLayout(this);
        intHeader.setOrientation(LinearLayout.HORIZONTAL);
        intHeader.setGravity(Gravity.CENTER_VERTICAL);
        intHeader.addView(M3.titleMedium(this, "System Integrity"), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));

        integrityBadge = M3.badge(this, "Not checked", M3.BADGE_NEUTRAL);
        intHeader.addView(integrityBadge);
        deviceCard.addView(intHeader);

        integrityView = M3.bodySmall(this, "");
        integrityView.setTextIsSelectable(true);
        integrityView.setPadding(0, M3.dp(this, 6), 0, M3.dp(this, 10));
        deviceCard.addView(integrityView);

        LinearLayout auditRow = new LinearLayout(this);
        auditRow.setOrientation(LinearLayout.HORIZONTAL);
        auditButton = M3.outlinedButton(this, "Check for changes", v -> runAudit(false));
        deepButton = M3.outlinedButton(this, "Full check (slower)", v -> runAudit(true));
        auditRow.addView(auditButton);
        addMarginEnd(auditButton, 10);
        auditRow.addView(deepButton);
        deviceCard.addView(auditRow);



        // ---------------------------------------------------------------- Card: Googlebook OS (Google's system image)
        LinearLayout googleCard = M3.card(this, 20);
        googleCard.addView(M3.eyebrow(this, "GOOGLEBOOK OS"));
        googleTitle = M3.titleLarge(this, "Googlebook OS");
        googleTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 2));
        googleCard.addView(googleTitle);
        googleDetail = M3.bodySmall(this, "Asking the system ...");
        googleDetail.setTextIsSelectable(true);
        googleCard.addView(googleDetail);
        googleStatus = M3.body(this, "");
        googleStatus.setTextIsSelectable(true);
        googleStatus.setPadding(0, M3.dp(this, 8), 0, 0);
        googleCard.addView(googleStatus);
        LinearLayout googleActions = new LinearLayout(this);
        googleActions.setOrientation(LinearLayout.HORIZONTAL);
        googleActions.setPadding(0, M3.dp(this, 14), 0, 0);
        googleCheckButton = M3.tonalButton(this, "Check for updates", v -> refresh(true));
        googleInstallButton = M3.filledButton(this, "Download and install", v -> confirmInstall());
        googleInstallButton.setVisibility(View.GONE);
        googleActions.addView(googleCheckButton);
        addMarginEnd(googleCheckButton, 10);
        googleActions.addView(googleInstallButton);
        googleCard.addView(googleActions);

        // ---------------------------------------------------------------- Card 3: Install from Folder
        LinearLayout localCard = M3.card(this, 20);
        localCard.addView(M3.eyebrow(this, "LOCAL BUNDLE"));

        TextView localTitle = M3.titleLarge(this, "Install from folder");
        localTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 4));
        localCard.addView(localTitle);

        TextView localDesc = M3.body(this, "Deploy an update folder (gbos86-update-...) copied to this laptop (for example in Downloads).");
        localCard.addView(localDesc);

        folderContainer = new LinearLayout(this);
        folderContainer.setOrientation(LinearLayout.VERTICAL);
        folderContainer.setPadding(M3.dp(this, 12), M3.dp(this, 10), M3.dp(this, 12), M3.dp(this, 10));
        folderContainer.setBackground(M3.roundRect(M3Theme.surfaceContainerHigh(this), M3.dp(this, 12)));
        LinearLayout.LayoutParams fclp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        fclp.setMargins(0, M3.dp(this, 10), 0, M3.dp(this, 10));
        folderContainer.setLayoutParams(fclp);

        folderView = M3.titleMedium(this, "No folder selected");
        folderView.setTextSize(14);
        folderContainer.addView(folderView);

        folderSummaryView = M3.bodySmall(this, "");
        folderSummaryView.setTextIsSelectable(true);
        folderContainer.addView(folderSummaryView);
        folderContainer.setVisibility(View.GONE);
        localCard.addView(folderContainer);

        LinearLayout folderActions = new LinearLayout(this);
        folderActions.setOrientation(LinearLayout.HORIZONTAL);
        folderActions.setPadding(0, M3.dp(this, 10), 0, 0);

        pickButton = M3.tonalButton(this, "Choose update folder", v ->
                startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER));
        stageFolderButton = M3.filledButton(this, "Copy to system", v -> confirmStageFolder());
        stageFolderButton.setEnabled(false);

        folderActions.addView(pickButton);
        addMarginEnd(pickButton, 10);
        folderActions.addView(stageFolderButton);
        localCard.addView(folderActions);



        // ---------------------------------------------------------------- Card 4: Maintenance
        actionCard = M3.card(this, 20);
        actionCard.addView(M3.eyebrow(this, "MAINTENANCE"));

        TextView actionTitle = M3.titleLarge(this, "Pending Updates");
        actionTitle.setPadding(0, M3.dp(this, 4), 0, M3.dp(this, 4));
        actionCard.addView(actionTitle);

        TextView actionDesc = M3.body(this, "Discard staged or waiting update files from GBOSSYS's update.inbox.");
        actionCard.addView(actionDesc);

        LinearLayout cancelRow = new LinearLayout(this);
        cancelRow.setOrientation(LinearLayout.HORIZONTAL);
        cancelRow.setPadding(0, M3.dp(this, 10), 0, 0);
        cancelButton = M3.dangerButton(this, "Remove waiting updates", v -> simple("cancel"));
        cancelRow.addView(cancelButton);
        actionCard.addView(cancelRow);

        // ---------------------------------------------------------------- Layout: two columns on a laptop screen, one on a narrow window
        // Left: what changes the system (gbOS86 update, Googlebook OS version, local folder). Right: what is installed now (laptop,
        // integrity, waiting files).
        if (wide) {
            LinearLayout cols = new LinearLayout(this);
            cols.setOrientation(LinearLayout.HORIZONTAL);
            cols.setBaselineAligned(false);
            LinearLayout left = column(updateCard, googleCard, localCard), right = column(deviceCard, actionCard);
            LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
            llp.setMarginEnd(M3.dp(this, 16));
            cols.addView(left, llp);
            cols.addView(right, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
            root.addView(cols);
        } else {
            for (View card : new View[] {updateCard, googleCard, deviceCard, localCard, actionCard}) {
                root.addView(card);
                addMarginBottom(card, 16);
            }
        }

        // Wrap root in responsive scaffold (max width 1200dp on a wide screen, 720dp otherwise) inside ScrollView
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(M3Theme.surface(this));
        View scaffold = M3.responsiveScaffold(this, root, wide ? 1200 : 720);
        scroll.addView(scaffold);
        setContentView(scroll);

        Notifier.channels(this);
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 2);
        CheckJob.schedule(this, false);
        Notifier.cancelAvailable(this);
        refresh(false);
    }

    private LinearLayout column(View... cards) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        for (View card : cards) {
            col.addView(card);
            addMarginBottom(card, 16);
        }
        return col;
    }

    private void addMarginEnd(View v, int dp) {
        LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) v.getLayoutParams();
        if (lp == null) lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMarginEnd(M3.dp(this, dp));
        v.setLayoutParams(lp);
    }

    private void addMarginBottom(View v, int dp) {
        LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) v.getLayoutParams();
        if (lp == null) lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = M3.dp(this, dp);
        v.setLayoutParams(lp);
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
        googleInstallButton.setVisibility(!run && decision != null && decision.kind == Updater.Kind.AVAILABLE && decision.needsSuper
                && !UpdateService.State.done ? View.VISIBLE : View.GONE);
        for (Button b : new Button[] {checkButton, googleCheckButton, auditButton, deepButton, pickButton, cancelButton}) b.setEnabled(!busy && !run);
        // the update is being confirmed in this start (sysupdate.sh boot-ok, a minute after boot_completed): look again until it is
        if (decision != null && decision.kind == Updater.Kind.FINISHING && !busy && System.currentTimeMillis() - finishingSince > 15000) {
            finishingSince = System.currentTimeMillis();
            refresh(false);
        }
        stageFolderButton.setEnabled(!busy && !run && relPath != null);

        if (info != null && (info.pending.equals("none") || info.pending.isEmpty()) && !waiting) {
            actionCard.setVisibility(View.GONE);
        } else {
            actionCard.setVisibility(View.VISIBLE);
        }

        if (UpdateService.State.done && run != wasRunning) refresh(false);
        wasRunning = run;
        ui.postDelayed(this::tick, 500);
    }

    // -------------------------------------------------------------------------------- The Checks
    private void refresh(boolean userAsked) {
        if (busy) return;
        busy = true;
        if (userAsked) {
            updateStatusTitle.setText("Checking for updates ...");
            updateStatusSubtitle.setText("");
            googleStatus.setText("Checking for a newer version ...");
        }
        new Thread(() -> {
            try {
                Updater.Info i = Updater.info(this);
                info = i;
                ui.post(this::showLaptop);
                // The modification check reads every system file through root: on a USB stick that took seconds of disk time on each
                // first open (boots 16-19, 2026-11-24). It runs when asked ("Check for changes") and before an update is copied.
                ui.post(auditFresh(i) ? this::showIntegrity : this::showUnchecked);
                try {
                    Feed f = Updater.fetchFeed(this, i);
                    Updater.Decision d = Updater.decide(this, i, audit, f);
                    decision = d;
                    Prefs.get(this).edit().putString("last_check", d.message).putLong("last_check_time", System.currentTimeMillis()).apply();
                    ui.post(() -> {
                        if (d.kind == Updater.Kind.UP_TO_DATE) {
                            updateStatusTitle.setText("System is up to date");
                        } else if (d.kind == Updater.Kind.AVAILABLE) {
                            updateStatusTitle.setText("Update available: seq " + f.seq);
                        } else if (d.kind == Updater.Kind.WAITING) {
                            updateStatusTitle.setText("Update ready to install");
                        } else if (d.kind == Updater.Kind.FINISHING) {
                            updateStatusTitle.setText("Finishing the update ...");
                        } else {
                            updateStatusTitle.setText("System Status");
                        }
                        updateStatusSubtitle.setText(d.message);
                        showGoogle(i, f, d);
                        if (!f.notes().isEmpty() && d.kind != Updater.Kind.UP_TO_DATE) {
                            notesView.setText(String.join("\n• ", f.notes()));
                            notesContainer.setVisibility(View.VISIBLE);
                        } else {
                            notesContainer.setVisibility(View.GONE);
                        }
                    });
                } catch (Exception e) {
                    decision = null;
                    ui.post(() -> {
                        googleStatus.setText("Could not check for a newer version: " + e.getMessage());
                        updateStatusTitle.setText("Unable to check updates");
                        updateStatusSubtitle.setText(e.getMessage());
                        notesContainer.setVisibility(View.GONE);
                    });
                }
            } catch (Root.NoService e) {
                ui.post(() -> {
                    laptopView.setText(e.getMessage());
                    updateStatusTitle.setText("Service unavailable");
                });
            } catch (Exception e) {
                ui.post(() -> {
                    laptopView.setText("Failed: " + e.getMessage());
                    updateStatusTitle.setText("Error");
                });
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
        sb.append(" • Google build ").append(i.googleBuild > 0 ? i.googleBuild : "unknown");
        if (i.freeKb >= 0) sb.append("\nFree space on system partition: ").append(Util.size(i.freeKb * 1024));
        if (!i.hasKey()) sb.append("\n⚠ No update key installed: updates are refused. Update once with the USB stick.");
        if (!"ok".equals(i.hook)) sb.append("\n⚠ Boot hook missing: update once with the USB stick (menu 1).");
        if (!i.last.isEmpty()) sb.append("\nLast result: ").append(i.last);
        laptopView.setText(sb.toString());
        String v = Updater.installedGoogleVersion(i);
        googleTitle.setText("Googlebook OS " + Feed.googleName(v, i.googleBuild));
        googleDetail.setText((v != null ? v + " • " : "") + "Google build " + (i.googleBuild > 0 ? i.googleBuild : "unknown"));
    }

    /** The Googlebook OS card: is a newer Google build published (inside a gbOS86 update), and can it be downloaded from Google? */
    private void showGoogle(Updater.Info i, Feed f, Updater.Decision d) {
        String have = Feed.googleName(Updater.installedGoogleVersion(i), i.googleBuild);
        if (f.googleBuild <= i.googleBuild || i.googleBuild <= 0) {
            googleStatus.setText(d.kind == Updater.Kind.FINISHING || d.kind == Updater.Kind.WAITING ? d.message
                    : "Googlebook OS " + have + " is the newest version gbOS86 supports.");
            return;
        }
        String name = Feed.googleName(f.googleVersion, f.googleBuild);
        if (d.kind == Updater.Kind.AVAILABLE && d.needsSuper) {
            long git = d.asset == null ? 0 : d.asset.size, google = f.googleSuper.bytes * 2;
            googleStatus.setText("Googlebook OS " + name + " is available (" + (f.googleVersion != null ? f.googleVersion + ", " : "")
                    + "Google build " + f.googleBuild + "), with gbOS86 update " + f.seq + ". Download: about " + Util.size(google)
                    + " from Google and " + Util.size(git) + " from GitHub. It is installed at the next restart; apps and data are kept.");
            googleInstallButton.setText("Download " + name + " and install");
        } else {
            googleStatus.setText("Googlebook OS " + name + " (Google build " + f.googleBuild + ") is published. " + d.message);
        }
    }

    private void showIntegrity() {
        Updater.Audit a = audit;
        if (a == null || info == null) return;
        String w = Updater.warning(a);
        String s = Updater.describe(a, info) + (w != null ? "\n\n⚠ " + w : "");
        integrityView.setText(s);

        if (w != null) {
            integrityBadge.setText("⚠ Modified");
            integrityBadge.setBackground(M3.roundRect(M3Theme.warningContainer(this), M3.dp(this, 12)));
            integrityBadge.setTextColor(M3Theme.onWarningContainer(this));
        } else if ("ok".equals(a.manifest)) {
            integrityBadge.setText("✓ Verified");
            integrityBadge.setBackground(M3.roundRect(M3Theme.successContainer(this), M3.dp(this, 12)));
            integrityBadge.setTextColor(M3Theme.onSuccessContainer(this));
        } else {
            integrityBadge.setText("Unknown");
            integrityBadge.setBackground(M3.roundRect(M3Theme.surfaceContainerHighest(this), M3.dp(this, 12)));
            integrityBadge.setTextColor(M3Theme.onSurfaceVariant(this));
        }
    }

    private boolean auditFresh(Updater.Info i) {
        Updater.Audit a = audit;
        return a != null && i != null && a.set.equals(i.set) && System.currentTimeMillis() - a.time <= 6 * 3600 * 1000L;
    }

    private void showUnchecked() {
        integrityView.setText("Not checked since the app started. \"Check for changes\" reads every system file; it also runs before an "
                + "update is installed.");
        integrityBadge.setText("Not checked");
        integrityBadge.setBackground(M3.roundRect(M3Theme.surfaceContainerHighest(this), M3.dp(this, 12)));
        integrityBadge.setTextColor(M3Theme.onSurfaceVariant(this));
    }

    /** Runs the modification check first when there is no recent one, then next (on the UI thread), so the install warning is real. */
    private void afterAudit(Runnable next) {
        if (auditFresh(info)) { next.run(); return; }
        if (busy) return;
        busy = true;
        integrityBadge.setText("Checking");
        integrityView.setText("Checking for modifications before the update ...");
        progressView.setText("Checking this laptop's files before the update ...");
        new Thread(() -> {
            try {
                audit = Updater.audit(this, false);
            } catch (Exception e) {
                audit = null;   // warning(null) says the files could not be checked, and asks before going on
                ui.post(() -> integrityView.setText("The modification check failed: " + e.getMessage()));
            } finally {
                busy = false;
            }
            ui.post(() -> {
                if (audit != null) showIntegrity();
                progressView.setText("");
                next.run();
            });
        }).start();
    }

    private void runAudit(boolean deep) {
        if (busy) return;
        busy = true;
        integrityBadge.setText("Auditing ...");
        integrityBadge.setBackground(M3.roundRect(M3Theme.surfaceContainerHighest(this), M3.dp(this, 12)));
        integrityBadge.setTextColor(M3Theme.onSurfaceVariant(this));
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

    // -------------------------------------------------------------------------------- Install Actions
    private void confirmInstall() {
        final Updater.Decision d = decision;
        if (d == null || d.kind != Updater.Kind.AVAILABLE) return;
        afterAudit(() -> confirmInstallChecked(d));
    }

    private void confirmInstallChecked(final Updater.Decision d) {
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
        if (d.needsSuper) { confirmGoogleDownload(d); return; }
        confirmDownloadNow(d);
    }

    /** A new Google build comes from Google itself: say how big, how long and what it needs, before anything starts. */
    private void confirmGoogleDownload(Updater.Decision d) {
        Feed f = d.feed;
        long sup = f.googleSuper.bytes, git = d.asset == null ? 0 : d.asset.size;
        android.net.ConnectivityManager cm = getSystemService(android.net.ConnectivityManager.class);
        boolean metered = cm != null && cm.isActiveNetworkMetered();
        String msg = "Update " + f.seq + " brings a new Google build (" + f.googleBuild + "). Its system image is Google's, so the app downloads "
                + "it from Google (dl.google.com) and rebuilds super.img on this laptop.\n\n"
                + "• Download: " + Util.size(git) + " from GitHub, plus about " + Util.size(sup * 2) + " from Google (at most Google's "
                + "whole file, about 7.7 GB).\n"
                + "• Free space needed: about " + Util.size(sup + git * 2 + (64L << 20)) + " on this laptop's data partition while it works, "
                + "and " + Util.size(sup + (128L << 20)) + " on GBOSSYS.\n"
                + "• Time: often an hour or more. Keep the laptop plugged in and awake. If the app is closed or the laptop sleeps, Google's "
                + "part starts over.\n"
                + (metered ? "\n⚠ This network is metered (mobile data or a data limit). Several GB will count against it: use Wi-Fi without a limit.\n" : "")
                + "\nNothing from Google is trusted: every partition and the rebuilt image must match the signed update before it is installed.";
        new AlertDialog.Builder(this)
                .setTitle("Large download: about " + Util.size(sup * 2 + git))
                .setMessage(msg)
                .setPositiveButton(metered ? "Download anyway" : "Continue", (x, y) -> confirmDownloadNow(d))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void confirmDownloadNow(Updater.Decision d) {
        String msg = d.message + "\n\nIt is downloaded now and installed at the next restart, after its signature and every file are checked "
                + "again. Your apps and data are kept. If the new system does not finish starting, the previous one comes back by itself.";
        if (d.needsSuper)
            msg += "\n\nThe new Google build is downloaded from Google and super.img is rebuilt here. Use Wi-Fi; this can take "
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

    // -------------------------------------------------------------------------------- Folder Picker
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
            showFolderError("This folder cannot be used: " + e.getMessage());
            return;
        }
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority()) || !docId.startsWith("primary:")) {
            showFolderError("Choose a folder in this device's own storage (for example Downloads). USB drives and SD cards "
                    + "are not supported here: copy the folder to Downloads first, or from Windows into update.inbox on GBOSSYS.");
            return;
        }
        String rel = docId.substring("primary:".length());
        if (rel.isEmpty() || !rel.matches(Root.ALLOWED) || ("/" + rel + "/").contains("/../") || ("/" + rel + "/").contains("/./")) {
            showFolderError("The folder's name or path has characters the updater does not accept: " + rel);
            return;
        }
        String summary;
        try {
            summary = readManifestSummary(DocumentsContract.buildDocumentUriUsingTree(tree, docId + "/manifest"));
        } catch (Exception e) {
            showFolderError("No update manifest in \"" + rel + "\". Choose the gbos86-update-... folder itself.");
            return;
        }
        relPath = rel;
        folderContainer.setVisibility(View.VISIBLE);
        folderView.setText(rel);
        folderSummaryView.setText(summary + "\n(Signature and files will be verified before staging and switch)");
        stageFolderButton.setEnabled(true);
    }

    private void showFolderError(String error) {
        folderContainer.setVisibility(View.VISIBLE);
        folderView.setText("Invalid update folder");
        folderSummaryView.setText(error);
        stageFolderButton.setEnabled(false);
    }

    private String readManifestSummary(Uri manifest) throws Exception {
        StringBuilder sb = new StringBuilder();
        try (InputStream in = getContentResolver().openInputStream(manifest);
             BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.US_ASCII))) {
            if (!"gbos86-system-set 1".equals(r.readLine())) throw new Exception("not a gbOS86 system set");
            for (int i = 0; i < 5; i++) {
                String l = r.readLine();
                if (l == null) break;
                if (l.startsWith("seq ")) sb.append("Update ").append(l.substring(4)).append(" • ");
                else if (l.startsWith("google_build ")) sb.append("Google build ").append(l.substring(13)).append(" • ");
                else if (l.startsWith("commit ")) sb.append("Commit ").append(l.substring(7, Math.min(l.length(), 19))).append(" • ");
                else if (l.startsWith("created ")) sb.append("Made ").append(l.substring(8));
            }
        }
        return sb.toString();
    }

    private void confirmStageFolder() {
        if (relPath == null) return;
        afterAudit(this::confirmStageFolderChecked);
    }

    private void confirmStageFolderChecked() {
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
                if (res.ok) {   // CheckJob deletes the folder once this set is installed (Updater.cleanupAfterCommit)
                    java.util.regex.Matcher m = java.util.regex.Pattern.compile("gbos86-update-([0-9]{1,14})-[0-9a-f]+$").matcher(rel);
                    if (m.find()) Prefs.get(this).edit().putLong("staged_seq", Long.parseLong(m.group(1))).apply();
                }
            } catch (Exception e) {
                r = "Failed: " + e.getMessage();
            }
            final String s = r;
            ui.post(() -> progressView.setText(s));
            busy = false;
            ui.post(() -> refresh(false));
        }).start();
    }
}
