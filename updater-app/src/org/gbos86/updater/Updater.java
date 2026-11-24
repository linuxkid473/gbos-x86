package org.gbos86.updater;

import android.content.Context;
import android.content.SharedPreferences;

import org.gbos86.updater.core.Feed;
import org.gbos86.updater.core.GoogleImage;
import org.gbos86.updater.core.Http;
import org.gbos86.updater.core.Tar;
import org.gbos86.updater.core.Util;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * What the app does, without the screens: ask the laptop what is installed (Root "info"), whether it was changed (Root "audit"),
 * read the signed feed from GitHub, decide what to download, download it, and hand it to the root service, which copies it to
 * GBOSSYS's update.inbox. The next boot checks everything again before it installs anything (docs/system-update.md).
 */
final class Updater {
    private Updater() {}

    // ------------------------------------------------------------------------------------------------ the laptop
    static final class Info {
        String set = "none", commit = "unknown", superSha = "", pub = "none", pending = "none", inbox = "", hook = "", last = "";
        long seq, googleBuild, freeKb = -1, logsKb;
        boolean hasKey() { return pub.matches("[0-9A-F]{768}"); }
    }

    static Info info(Context c) throws Exception {
        Root.Result r = Root.call(c, "info", null, null);
        if (!r.ok) throw new Exception("The laptop could not answer: " + r.body);
        Map<String, String> m = Root.keyValues(r.body);
        Info i = new Info();
        i.set = m.getOrDefault("set", "none");
        i.commit = m.getOrDefault("commit", "unknown");
        i.superSha = m.getOrDefault("super", "");
        i.pub = m.getOrDefault("pub", "none");
        i.pending = m.getOrDefault("pending", "none");
        i.inbox = m.getOrDefault("inbox", "").trim();
        i.hook = m.getOrDefault("hook", "");
        i.last = m.getOrDefault("last", "");
        i.seq = num(m.get("seq"));
        i.googleBuild = num(m.get("google_build"));
        i.freeKb = m.containsKey("free_kb") ? num(m.get("free_kb")) : -1;
        i.logsKb = num(m.get("logs_kb"));
        return i;
    }

    static long num(String s) {
        try { return s == null ? 0 : Long.parseLong(s.trim()); } catch (NumberFormatException e) { return 0; }
    }

    /** The modification check: the laptop's files against its system.manifest, and the set's commit against the project's sources. */
    static final class Audit {
        String manifest = "none", source = "unknown", modified = "unknown", commit = "", set = "";
        int changed, missing, added;
        final List<String> files = new ArrayList<>();
        /** official | unpublished | unknown (not checked: no network, no commit) */
        String published = "unknown";
        long time;

        /** The user's question: are there modifications? (local file changes, or a set built from changed sources) */
        boolean modified() { return "yes".equals(modified) || "dirty".equals(source); }
        boolean known() { return "ok".equals(manifest); }
    }

    static Audit audit(Context c, boolean deep) throws Exception {
        Root.Result r = Root.call(c, "audit", deep ? Root.extra("deep", "1") : null, null);
        if (!r.ok) throw new Exception("The check failed: " + r.body);
        Map<String, String> m = Root.keyValues(r.body);
        Audit a = new Audit();
        a.manifest = m.getOrDefault("manifest", "none");
        a.source = m.getOrDefault("source", "unknown");
        a.modified = m.getOrDefault("modified", "unknown");
        a.commit = m.getOrDefault("commit", "");
        a.set = m.getOrDefault("set", "");
        a.changed = (int) num(m.get("changed"));
        a.missing = (int) num(m.get("missing"));
        a.added = (int) num(m.get("added"));
        for (String l : r.body.split("\n")) if (l.matches("(changed|missing|added) .+")) a.files.add(l);
        a.time = System.currentTimeMillis();
        String sha = a.commit.replace("-dirty", "");
        if ("git".equals(a.source) && sha.matches("[0-9a-f]{40}") && Prefs.validSource(Prefs.source(c))) {
            SharedPreferences p = Prefs.get(c);
            String key = "published_" + Prefs.source(c) + "_" + sha;
            String cached = p.getString(key, "");
            if (!cached.isEmpty()) a.published = cached;
            else {
                try {
                    a.published = Http.exists("https://api.github.com/repos/" + Prefs.source(c) + "/commits/" + sha) ? "official" : "unpublished";
                    p.edit().putString(key, a.published).apply();
                } catch (Exception e) {
                    a.published = "unknown";
                }
            }
        }
        return a;
    }

    /** One paragraph for the screen, and whether it is a warning. */
    static String describe(Audit a, Info i) {
        StringBuilder sb = new StringBuilder();
        if (!a.known()) {
            if ("none".equals(a.manifest))
                return "Cannot check for modifications: the laptop has no record of its files (it was installed or updated from the USB "
                        + "stick without one). The first update from this app installs every file and makes that record.";
            return "Cannot check for modifications: the laptop's record of its files is unreadable.";
        }
        String c7 = a.commit.length() >= 7 ? a.commit.substring(0, 7) : a.commit;
        if ("dirty".equals(a.source)) sb.append("Built from modified sources (commit ").append(c7).append(" with local changes).");
        else if ("official".equals(a.published)) sb.append("Built from the project's published sources (commit ").append(c7).append(").");
        else if ("unpublished".equals(a.published)) sb.append("Built from commit ").append(c7).append(", which is not in the project's published sources (a local or forked build).");
        else if ("git".equals(a.source)) sb.append("Built from commit ").append(c7).append(" (not checked against GitHub: no connection).");
        else sb.append("Unknown origin (the record names no commit).");
        if ("no".equals(a.modified)) sb.append(" No local changes to its files.");
        else if ("yes".equals(a.modified)) {
            sb.append(" Local changes: ");
            List<String> parts = new ArrayList<>();
            if (a.changed > 0) parts.add(a.changed + " changed");
            if (a.missing > 0) parts.add(a.missing + " missing");
            if (a.added > 0) parts.add(a.added + " added");
            sb.append(String.join(", ", parts)).append(" file(s)");
            int n = 0;
            for (String f : a.files) { if (n++ == 8) { sb.append("\n  ..."); break; } sb.append("\n  ").append(f); }
        }
        return sb.toString();
    }

    /** Why an install needs the "Install anyway" confirmation, as short codes (also written to the laptop's update history when the
     *  user accepts): unchecked (no check result), norecord / unreadable (no usable record of the files), dirty (built from changed
     *  sources), changed (files differ from the record), unpublished (its commit is not in the project's sources), noorigin (the record
     *  names no commit). Empty: the system is exactly a published set. */
    static List<String> warningCodes(Audit a) {
        List<String> w = new ArrayList<>();
        if (a == null) { w.add("unchecked"); return w; }
        if (!a.known()) { w.add("none".equals(a.manifest) ? "norecord" : "unreadable"); return w; }
        if ("dirty".equals(a.source)) w.add("dirty");
        if ("yes".equals(a.modified)) w.add("changed");
        if ("unpublished".equals(a.published)) w.add("unpublished");
        if ("unknown".equals(a.source)) w.add("noorigin");
        return w;
    }

    static String warning(Audit a) {
        List<String> codes = warningCodes(a);
        if (codes.isEmpty()) return null;
        List<String> why = new ArrayList<>();
        for (String k : codes) {
            switch (k) {
                case "unchecked": why.add("its files could not be checked"); break;
                case "norecord": why.add("the laptop has no record of its files (it was installed from a USB stick with changes), so changes cannot be ruled out"); break;
                case "unreadable": why.add("the laptop's record of its files is unreadable"); break;
                case "dirty": why.add("it was built from changed sources"); break;
                case "changed": why.add("some of its files were changed, removed or added"); break;
                case "unpublished": why.add("it was built from a version that is not in gbOS86's published sources (a local or forked build)"); break;
                case "noorigin": why.add("its origin is unknown"); break;
                default: why.add(k);
            }
        }
        return "This system may not be the one gbOS86 published: " + String.join("; ", why) + ".\n\n"
                + "The update replaces every system file with the published version, so any changes on this laptop are lost, and files added "
                + "to its folders are deleted. It was tested only with the published version and could break things here. "
                + "If you continue, the laptop's update history records that you were warned.";
    }

    // ------------------------------------------------------------------------------------------------ the feed
    static Feed fetchFeed(Context c, Info i) throws Exception {
        String url = Prefs.feedUrl(c);
        if (url == null) throw new Exception("No update source is set (Settings > General).");
        if (!i.hasKey()) throw new Exception("This laptop has no update key (gbos/sysupdate.pub), so it accepts no updates. Update once with the USB stick.");
        byte[] b;
        try {
            b = Http.get(url, 65536);
        } catch (java.io.IOException e) {
            if (String.valueOf(e.getMessage()).startsWith("HTTP 404")) throw new Exception("No updates have been published at github.com/" + Prefs.source(c) + " yet.");
            throw new Exception("Could not reach GitHub: " + e.getMessage());
        }
        return Feed.parseSigned(b, i.pub);
    }

    enum Kind { UP_TO_DATE, AVAILABLE, BLOCKED, WAITING, FINISHING }

    static final class Decision {
        Kind kind;
        String message;
        Feed feed;
        boolean delta, needsSuper;
        Feed.Asset asset;
    }

    /** The running Googlebook OS version (ro.build.id, e.g. CL3B.260622.276.R4) when it is the Google build the set records. */
    static String installedGoogleVersion(Info i) {
        String id = android.os.Build.ID, inc = android.os.Build.VERSION.INCREMENTAL;
        if (id == null || !id.matches("[A-Z0-9][A-Z0-9.]{0,39}")) return null;
        return i.googleBuild <= 0 || String.valueOf(i.googleBuild).equals(inc) ? id : null;
    }

    static Decision decide(Context c, Info i, Audit a, Feed f) {
        Decision d = new Decision();
        d.feed = f;
        if ("trying".equals(i.pending)) {   // this start runs the new set: sysupdate.sh boot-ok commits it a minute after boot_completed
            d.kind = Kind.FINISHING;
            d.message = "Finishing the update: it is confirmed about a minute after the laptop has started. If this stays for more "
                    + "than a few minutes, restart the laptop.";
            return d;
        }
        if (!"none".equals(i.pending)) {
            d.kind = Kind.WAITING;
            d.message = "An update is being installed (" + i.pending + "). Restart the laptop to finish it.";
            return d;
        }
        if (!i.inbox.isEmpty()) {
            d.kind = Kind.WAITING;
            d.message = "An update is ready. Restart the laptop to install it.";
            return d;
        }
        if (f.seq <= i.seq) {
            d.kind = Kind.UP_TO_DATE;
            d.message = "gbOS86 is up to date (update " + i.seq + ").";
            return d;
        }
        d.needsSuper = f.googleBuild != i.googleBuild
                || (f.googleSuper != null && !i.superSha.isEmpty() && !i.superSha.equals(f.googleSuper.sha256));
        if (d.needsSuper && f.googleBuild < i.googleBuild) {
            d.kind = Kind.BLOCKED;
            d.message = "Update " + f.seq + " has an older Google build (" + f.googleBuild + ") than this laptop (" + i.googleBuild
                    + "). Going back needs the USB stick (Reinstall).";
            return d;
        }
        if (d.needsSuper && !f.canRebuildSuper()) {
            d.kind = Kind.BLOCKED;
            d.message = "Update " + f.seq + " brings a new Google build (" + f.googleBuild + "). It cannot be downloaded from Google for "
                    + "this build: update with the USB stick (menu 1).";
            return d;
        }
        if (d.needsSuper && !Prefs.googleDirect(c)) {
            d.kind = Kind.BLOCKED;
            d.message = "Update " + f.seq + " brings a new Google build (" + f.googleBuild + ", you have " + i.googleBuild + "). Update with "
                    + "the USB stick (menu 1), or turn on Settings > General > Google builds > \"Download new Google builds from "
                    + "Google\" (several GB).";
            return d;
        }
        if (d.needsSuper && i.freeKb >= 0) {
            // super.img goes to GBOSSYS next to the current one: check before downloading GBs from Google
            long need = f.googleSuper.bytes + (128L << 20), have = i.freeKb * 1024 + (Prefs.pruneLogs(c) ? Math.max(0, i.logsKb - 50 * 1024) * 1024 : 0);
            if (have < need) {
                d.kind = Kind.BLOCKED;
                d.message = "Update " + f.seq + " brings a new Google build, which needs " + Util.size(need) + " free on the system partition "
                        + "(GBOSSYS); it has " + Util.size(i.freeKb * 1024) + ". Old boot logs use " + Util.size(i.logsKb * 1024)
                        + (Prefs.pruneLogs(c) ? "." : ": turn on Settings > General > \"delete old boot logs\", or") + " update with the USB stick.";
                return d;
            }
        }
        d.delta = f.asset("delta") != null && f.bases.contains(i.set) && (a == null || !a.known() || !a.set.equals(i.set) || !a.modified());
        d.asset = d.delta ? f.asset("delta") : f.asset("full");
        d.kind = Kind.AVAILABLE;
        d.message = "Update " + f.seq + " is available (" + Util.size(d.asset.size) + " from GitHub"
                + (d.needsSuper ? ", plus Google build " + f.googleBuild + " from Google: about " + Util.size(f.googleSuper.bytes * 2)
                        + " of data" : "") + ").";
        return d;
    }

    // ------------------------------------------------------------------------------------------------ download and stage
    /** Downloads, (experimental) rebuilds super.img, unpacks, stages. Returns the root service's message. */
    static String install(Context c, Decision d, Util.Progress p, Util.Cancel cancel) throws Exception {
        Feed f = d.feed;
        File work = new File(c.getFilesDir(), "updates/" + f.seq);
        File parent = work.getParentFile();
        // earlier updates' leftovers
        File[] old = parent.listFiles();
        if (old != null) for (File o : old) if (!o.getName().equals(String.valueOf(f.seq))) deleteTree(o);
        if (!work.isDirectory() && !work.mkdirs()) throw new Exception("cannot make " + work);
        String result;
        boolean delta = d.delta;
        while (true) {
            Feed.Asset asset = delta ? f.asset("delta") : f.asset("full");
            long need = asset.size * 2 + (d.needsSuper ? f.googleSuper.bytes : 0) + (64L << 20);
            if (work.getUsableSpace() < need)
                throw new Exception("Not enough free space on this laptop's data partition: the update needs " + Util.size(need) + ", "
                        + Util.size(work.getUsableSpace()) + " are free.");
            File tar = new File(work, asset.file);
            Http.download(f.assetUrl(asset), tar, asset.size, asset.sha256, prefixed(p, "Downloading the update: "), cancel);
            File superImg = new File(work, "super.img");
            if (d.needsSuper && !(superImg.isFile() && superImg.length() == f.googleSuper.bytes)) {
                GoogleImage.buildSuper(new GoogleImage.HttpSource(f.googleZip, cancel), f, superImg, prefixed(p, "Google build " + f.googleBuild + ": "), cancel);
            }
            File unpack = new File(work, "bundle");
            deleteTree(unpack);
            unpack.mkdirs();
            File bundle = Tar.extract(tar, unpack, prefixed(p, "Unpacking: "), cancel);
            if (!bundle.getName().equals(f.name)) throw new Exception("the download holds " + bundle.getName() + ", not " + f.name);
            if (d.needsSuper) {
                File dst = new File(bundle, "files/super.img");
                if (!superImg.renameTo(dst)) throw new Exception("cannot move super.img into the bundle");
            }
            if (p != null) p.update("Copying the update to the system partition ...", 0, -1);
            Root.Result r = Root.call(c, "stage", Root.extra("where", "app", "path", "updates/" + f.seq + "/bundle/" + f.name,
                    "prune_logs", Prefs.pruneLogs(c) ? "1" : null,
                    "accepted_warning", Prefs.acceptedWarning(c)), body -> { if (p != null) p.update(body, 0, -1); });
            if (d.needsSuper) new File(bundle, "files/super.img").renameTo(superImg);   // keep it for a retry until staged
            if (r.ok) { result = r.body; break; }
            if (delta && r.body.contains("NEEDS-FILES")) {   // the laptop lacks files the delta does not carry: take the full bundle
                delta = false;
                tar.delete();
                continue;
            }
            throw new Exception(r.body);
        }
        deleteTree(work);   // the copy is on GBOSSYS now
        Prefs.get(c).edit().putLong("staged_seq", f.seq).apply();
        return result;
    }

    /**
     * After an update this app staged is committed (the laptop runs its set and no update is being tried): deletes this app's
     * download folder and asks the root service to delete the bundle folder it was staged from (a folder picked in shared storage,
     * e.g. Downloads). Runs after the user unlocks (CheckJob), because both live in encrypted storage. Returns what happened, or null.
     */
    static String cleanupAfterCommit(Context c) throws Exception {
        long staged = Prefs.get(c).getLong("staged_seq", 0);
        if (staged == 0) return null;
        Info i = info(c);
        if (!"none".equals(i.pending) && !i.pending.isEmpty()) return null;   // still being tried: wait for the next start
        if (!i.inbox.isEmpty()) return null;                                     // staged, not installed yet (no restart since)
        Prefs.get(c).edit().remove("staged_seq").apply();
        if (i.seq != staged) return null;                                       // rolled back or replaced: keep the user's files
        deleteTree(new File(c.getFilesDir(), "updates"));
        Root.Result r = Root.call(c, "cleanup", null, null);
        return r.body;
    }

    static Util.Progress prefixed(Util.Progress p, String prefix) {
        if (p == null) return null;
        return (ph, d, t) -> p.update(prefix + ph, d, t);
    }

    static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] k = f.isDirectory() ? f.listFiles() : null;
        if (k != null) for (File x : k) deleteTree(x);
        f.delete();
    }
}
