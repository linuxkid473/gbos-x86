package org.gbos86.updater;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.provider.MediaStore;

import org.gbos86.updater.core.ReportCrypto;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * The Bug report card (docs/bug-report-plan.md): the root service collects and redacts the logs (sysupdate.sh report), this class
 * encrypts the result for the maintainer (gbos/report.pub) and saves it as Download/gbOS86/gbos86-report-....zip, ready to attach to
 * a GitHub issue. The unencrypted .tar.gz never leaves the app's private folder and is deleted afterwards.
 */
final class Report {
    private Report() {}

    static final class Made {
        String id, fileName, issueUrl;
        long size;
    }

    static Made make(Context c, Updater.Info info, String description, Root.Listener l) throws Exception {
        if (!info.hasReportKey()) throw new Exception("This system has no report key (gbos/report.pub), so reports cannot be encrypted. "
                + "Update gbOS86 first.");
        File dir = c.getFilesDir();
        File desc = new File(dir, "report-desc.txt");
        try (FileOutputStream f = new FileOutputStream(desc)) {
            byte[] b = description.getBytes(StandardCharsets.UTF_8);
            f.write(b, 0, Math.min(b.length, 8000));
        }
        Root.Result r;
        try {
            r = Root.call(c, "report", null, l);
        } finally {
            desc.delete();
        }
        if (!r.ok) throw new Exception("Collecting the logs failed: " + r.body);
        Map<String, String> m = Root.keyValues(r.body);
        String file = m.get("file");
        if (file == null || !file.matches("gbos86-report-[0-9a-f]{8}-[0-9]{12}\\.tar\\.gz")) throw new Exception("unexpected answer: " + r.body);
        File tgz = new File(dir, "report/" + file);
        if (!tgz.isFile()) throw new Exception("the report file is missing");
        Made made = new Made();
        made.id = m.getOrDefault("id", "gbos86-unknown");
        String base = file.substring(0, file.length() - ".tar.gz".length());
        made.fileName = base + ".zip";
        if (l != null) l.running("Encrypting the report ...");
        ContentResolver cr = c.getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, made.fileName);
        v.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/gbOS86");
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
        if (uri == null) throw new Exception("cannot create the file in Downloads");
        try (OutputStream o = cr.openOutputStream(uri); ZipOutputStream z = new ZipOutputStream(o); InputStream in = new FileInputStream(tgz)) {
            z.putNextEntry(new ZipEntry("README.txt"));
            z.write(("gbOS86 bug report " + made.id + ", encrypted for the gbOS86 maintainer (only their key opens it).\n"
                    + "Maintainer: tools/open_report.sh open " + made.fileName + " --key report-private.pem\n").getBytes(StandardCharsets.UTF_8));
            z.closeEntry();
            z.putNextEntry(new ZipEntry(base + ".enc"));
            ReportCrypto.encrypt(in, z, info.reportPub);
            z.closeEntry();
        } catch (Exception e) {
            cr.delete(uri, null, null);
            throw e;
        } finally {
            tgz.delete();
        }
        v.clear();
        v.put(MediaStore.MediaColumns.IS_PENDING, 0);
        cr.update(uri, v, null, null);
        try (android.database.Cursor q = cr.query(uri, new String[] {MediaStore.MediaColumns.SIZE, MediaStore.MediaColumns.DISPLAY_NAME}, null, null, null)) {
            if (q != null && q.moveToFirst()) { made.size = q.getLong(0); String dn = q.getString(1); if (dn != null) made.fileName = dn; }
        }
        String repo = Prefs.source(c);
        made.issueUrl = repo == null || repo.isEmpty() ? null : ReportCrypto.issueUrl(repo, made.id, info.seq,
                org.gbos86.updater.core.Feed.googleName(Updater.installedGoogleVersion(info), info.googleBuild), made.fileName);
        return made;
    }
}
