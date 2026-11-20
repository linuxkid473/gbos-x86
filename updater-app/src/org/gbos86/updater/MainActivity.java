package org.gbos86.updater;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * gbOS86 Updater. Not a trust anchor: it never installs anything. It asks the root service (sysupdate.sh request, started by init
 * when this app sets sys.gbos86.sysupdate) to copy a bundle folder from shared storage into GBOSSYS's update.inbox. The next boot's
 * stage0 checks the signature and every file again before anything changes (docs/system-update.md).
 *
 * Protocol: files/request.txt (written here, read and deleted by the service), files/response.txt (written by the service).
 */
public class MainActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final String PROP = "sys.gbos86.sysupdate";
    private static final String ALLOWED = "[A-Za-z0-9._+,@=~/ -]+";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView statusView, bundleView, progressView;
    private Button pickButton, stageButton, cancelButton, refreshButton, rebootButton;
    private CheckBox pruneBox;
    private String relPath;          // folder relative to this user's shared storage, e.g. "Download/gbos86-update-..."
    private volatile boolean busy;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        int pad = dp(16);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(pad, pad, pad, pad);

        col.addView(heading("Installed system"));
        statusView = text("Asking the system ...");
        statusView.setTypeface(Typeface.MONOSPACE);
        col.addView(statusView);
        refreshButton = button("Refresh", v -> request("status", null, false));
        col.addView(refreshButton);

        col.addView(heading("New update"));
        col.addView(text("Pick the gbos86-update-... folder (for example in Downloads). It is copied to the system partition and "
                + "installed at the next restart, after its signature and every file are checked again. If the new system does not "
                + "finish starting, the previous one comes back by itself."));
        pickButton = button("Choose update folder", v -> pickFolder());
        col.addView(pickButton);
        bundleView = text("No folder chosen.");
        col.addView(bundleView);
        pruneBox = new CheckBox(this);
        pruneBox.setText("If space is short, delete old boot logs (the newest 3 boots are kept)");
        col.addView(pruneBox);
        stageButton = button("Copy update to the system", v -> confirmStage());
        stageButton.setEnabled(false);
        col.addView(stageButton);
        progressView = text("");
        progressView.setTypeface(Typeface.MONOSPACE);
        col.addView(progressView);

        col.addView(heading("Other"));
        cancelButton = button("Remove waiting updates", v -> request("cancel", null, false));
        col.addView(cancelButton);
        rebootButton = button("Restart now", v -> confirmReboot());
        col.addView(rebootButton);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(col);
        setContentView(scroll);
        request("status", null, false);
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
        b.setOnClickListener(l);
        return b;
    }
    private void setBusy(boolean b) {
        busy = b;
        ui.post(() -> {
            for (Button x : new Button[] {pickButton, cancelButton, refreshButton, rebootButton}) x.setEnabled(!b);
            stageButton.setEnabled(!b && relPath != null);
        });
    }

    // ------------------------------------------------------------------------------------------------ folder choice
    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(i, PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        relPath = null;
        stageButton.setEnabled(false);
        String docId;
        try {
            docId = DocumentsContract.getTreeDocumentId(tree);
        } catch (Exception e) {
            bundleView.setText("This folder cannot be used: " + e.getMessage());
            return;
        }
        // The root service reads the folder directly from /data/media/<user>/, so only this device's own shared storage works.
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority()) || !docId.startsWith("primary:")) {
            bundleView.setText("Choose a folder in this device's own storage (for example Downloads). USB drives and SD cards "
                    + "are not supported here: copy the folder to Downloads first, or from Windows into update.inbox on GBOSSYS.");
            return;
        }
        String rel = docId.substring("primary:".length());
        if (rel.isEmpty() || !rel.matches(ALLOWED) || ("/" + rel + "/").contains("/../") || ("/" + rel + "/").contains("/./")) {
            bundleView.setText("The folder's name or path has characters the updater does not accept: " + rel);
            return;
        }
        String summary;
        try {
            summary = readManifestSummary(DocumentsContract.buildDocumentUriUsingTree(tree, docId + "/manifest"));
        } catch (Exception e) {
            bundleView.setText("No update manifest in \"" + rel + "\". Choose the gbos86-update-... folder itself.");
            return;
        }
        relPath = rel;
        bundleView.setText(rel + "\n" + summary + "\n(not checked yet: the signature is checked when it is copied, and again at restart)");
        stageButton.setEnabled(!busy);
    }

    private String readManifestSummary(Uri manifest) throws Exception {
        StringBuilder sb = new StringBuilder();
        try (InputStream in = getContentResolver().openInputStream(manifest);
             BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.US_ASCII))) {
            String first = r.readLine();
            if (!"gbos86-system-set 1".equals(first)) throw new Exception("not a gbOS86 system set");
            for (int i = 0; i < 5; i++) {
                String l = r.readLine();
                if (l == null) break;
                if (l.startsWith("seq ")) sb.append("update number ").append(l.substring(4)).append(", ");
                else if (l.startsWith("google_build ")) sb.append("Google build ").append(l.substring(13)).append(", ");
                else if (l.startsWith("commit ")) sb.append("commit ").append(l.substring(7, Math.min(l.length(), 19))).append(", ");
                else if (l.startsWith("created ")) sb.append("made ").append(l.substring(8));
            }
        }
        return sb.toString();
    }

    private void confirmStage() {
        new AlertDialog.Builder(this)
                .setTitle("Copy this update?")
                .setMessage("It is copied to the system partition now and installed at the next restart. "
                        + "Your apps and data are kept.")
                .setPositiveButton("Copy", (d, w) -> request("stage", relPath, pruneBox.isChecked()))
                .setNegativeButton("Cancel", null)
                .show();
    }
    private void confirmReboot() {
        new AlertDialog.Builder(this)
                .setTitle("Restart now?")
                .setMessage("A waiting update is checked and installed while the laptop starts. Save your work first.")
                .setPositiveButton("Restart", (d, w) -> request("reboot", null, false))
                .setNegativeButton("Cancel", null)
                .show();
    }

    // ------------------------------------------------------------------------------------------------ requests
    private void request(String action, String path, boolean prune) {
        if (busy) return;
        setBusy(true);
        final TextView out = "status".equals(action) ? statusView : progressView;
        ui.post(() -> out.setText("Asking the system ..."));
        new Thread(() -> {
            String result;
            try {
                result = exchange(action, path, prune, out);
            } catch (Exception e) {
                result = "Failed: " + e.getMessage();
            }
            final String r = result;
            ui.post(() -> out.setText(r));
            setBusy(false);
            if (!"status".equals(action) && !"reboot".equals(action)) ui.post(() -> request("status", null, false));
        }).start();
    }

    /** Writes the request, sets the property, waits for the matching response. Returns the text to show. */
    private String exchange(String action, String path, boolean prune, TextView out) throws Exception {
        File dir = getFilesDir();
        File rq = new File(dir, "request.txt"), part = new File(dir, "request.txt.part"), rs = new File(dir, "response.txt");
        rs.delete();
        long id = System.currentTimeMillis() % 1000000000L;
        StringBuilder sb = new StringBuilder("gbos86-sysupdate-request 1\nid ").append(id).append("\naction ").append(action).append('\n');
        if (path != null) sb.append("path ").append(path).append('\n');
        if (prune) sb.append("prune_logs 1\n");
        try (FileOutputStream f = new FileOutputStream(part)) {
            f.write(sb.toString().getBytes(StandardCharsets.US_ASCII));
            f.getFD().sync();
        }
        if (!part.renameTo(rq)) throw new Exception("cannot write the request");
        int user = android.os.Process.myUid() / 100000;
        java.lang.Process p = Runtime.getRuntime().exec(new String[] {"/system/bin/setprop", PROP, user + "." + id});
        if (p.waitFor() != 0)
            return "The system did not accept the request (setprop failed). The updater service needs gbOS86's boot hook "
                    + "and permissive SELinux (docs/system-update.md).";
        long start = System.currentTimeMillis(), lastChange = start;
        String last = "";
        while (true) {
            Thread.sleep(700);
            String r = readResponse(rs, id);
            long now = System.currentTimeMillis();
            if (r == null) {
                if (now - start > 20000)
                    return "No answer from the updater service. Is gbos86-updater.apk on GBOSSYS (so hook.sh starts the service), "
                            + "and is this gbOS86 installed on the internal disk?";
                continue;
            }
            String state = r.substring(0, r.indexOf('\n') < 0 ? r.length() : r.indexOf('\n'));
            String body = r.indexOf('\n') < 0 ? "" : r.substring(r.indexOf('\n') + 1);
            if (state.equals("state done")) return body.isEmpty() ? "Done." : body;
            if (state.equals("state failed")) return "Failed:\n" + body;
            if (!body.equals(last)) { last = body; lastChange = now; ui.post(() -> out.setText(body)); }
            if (now - lastChange > 15 * 60 * 1000) return "The updater service stopped answering. Last message:\n" + last;
        }
    }

    /** "state ...\nbody" for the response to request id, or null. */
    private static String readResponse(File rs, long id) {
        if (!rs.exists()) return null;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(rs), StandardCharsets.UTF_8))) {
            if (!"gbos86-sysupdate-response 1".equals(r.readLine())) return null;
            if (!("id " + id).equals(r.readLine())) return null;
            String state = r.readLine();
            if (state == null || !state.startsWith("state ")) return null;
            StringBuilder sb = new StringBuilder(state).append('\n');
            String l;
            int n = 0;
            while ((l = r.readLine()) != null && n++ < 200) sb.append(l).append('\n');
            return sb.toString().trim();
        } catch (Exception e) {
            return null;
        }
    }
}
