package org.gbos86.updater.core;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.file.Files;

/**
 * Plain-Java tests of the app's core (tools/test_updater_core.sh prepares the files: Google-like zips, the reference super.img made
 * by tools/fetch_google_parts.py + usb/make_super.py, signed feeds, tars, a local Range server). Prints one line per check.
 */
public class CoreTest {
    static int fails = 0;
    static File T;
    static String port;

    static void ok(String m) { System.out.println("ok - " + m); }
    static void bad(String m) { System.out.println("FAIL: " + m); fails++; }
    static void check(String m, boolean c) { if (c) ok(m); else bad(m); }
    interface Body { void run() throws Exception; }
    static void throwsWith(String m, String needle, Body b) {
        try {
            b.run();
            bad(m + " (no error)");
        } catch (Exception e) {
            if (e.getMessage() != null && e.getMessage().contains(needle)) ok(m);
            else bad(m + " (wrong error: " + e + ")");
        }
    }
    static byte[] read(String name) throws IOException { return Files.readAllBytes(new File(T, name).toPath()); }
    static String text(String name) throws IOException { return new String(read(name), "US-ASCII").trim(); }
    static String sha(File f) throws IOException { return Util.sha256Hex(f, null); }

    public static void main(String[] a) throws Exception {
        T = new File(a[0]);
        port = a[1];
        Http.allowPlainHttpForTests = true;
        String mod = text("key.mod"), other = text("other.mod");

        // ---- feed
        Feed f = Feed.parseSigned(read("feed.good"), mod);
        check("feed: parses and verifies", f.seq == 4242 && f.googleBuild == 16000001 && f.asset("full") != null && f.googleParts.size() == 2
                && f.canRebuildSuper() && f.bases.size() == 1 && f.notes.size() == 1);
        check("feed: google_version and its short name", "CL3B.260622.276.R5".equals(f.googleVersion)
                && "R5".equals(Feed.googleName(f.googleVersion, f.googleBuild)));
        check("googleName: no version -> build number", "build 16658739".equals(Feed.googleName(null, 16658739))
                && "R4".equals(Feed.googleName("CL3B.260622.276.R4", 16658739)) && "ABC1".equals(Feed.googleName("ABC1", 1)));
        throwsWith("feed: a malformed google_version is refused", "google_version", () -> Feed.parseSigned(read("feed.badversion"), mod));
        throwsWith("feed: another key is refused", "signature", () -> Feed.parseSigned(read("feed.good"), other));
        byte[] t = read("feed.good");
        t[30] ^= 1;
        final byte[] tampered = t;
        throwsWith("feed: a changed byte is refused", "signature", () -> Feed.parseSigned(tampered, mod));
        throwsWith("feed: an unknown line is refused even when signed", "unknown line", () -> Feed.parseSigned(read("feed.unknown"), mod));
        throwsWith("feed: a wrong end count is refused", "end count", () -> Feed.parseSigned(read("feed.badend"), mod));
        throwsWith("feed: a non-Google zip address is refused", "bad google_zip", () -> Feed.parseSigned(read("feed.badurl"), mod));
        throwsWith("feed: no key installed", "signature", () -> Feed.parseSigned(read("feed.good"), null));

        // ---- make_super.py port
        File p1 = new File(T, "parts/system_a.img"), p2 = new File(T, "parts/vendor_a.img");
        SuperImage plan = SuperImage.plan(new String[] {"system_a", "vendor_a"}, new long[] {p1.length(), p2.length()});
        File mine = new File(T, "mine-super.img");
        try (RandomAccessFile raf = new RandomAccessFile(mine, "rw")) {
            raf.setLength(plan.total);
            plan.writeMetadata(raf);
            for (File p : new File[] {p1, p2}) {
                raf.seek(plan.part(p == p1 ? "system_a" : "vendor_a").offset);
                raf.write(Files.readAllBytes(p.toPath()));
            }
        }
        check("SuperImage: byte for byte make_super.py's super.img", sha(mine).equals(sha(new File(T, "exp/super.img"))));

        // ---- Google zip -> super.img
        File out = new File(T, "out/super.img");
        out.getParentFile().mkdirs();
        GoogleImage.buildSuper(new GoogleImage.FileSource(new File(T, "google.zip")), f, out, null, null);
        check("GoogleImage: deflated zip64 entry -> the published super.img", sha(out).equals(f.googleSuper.sha256));
        out.delete();
        GoogleImage.buildSuper(new GoogleImage.FileSource(new File(T, "google-stored.zip")), f, out, null, null);
        check("GoogleImage: stored entry -> the same super.img", sha(out).equals(f.googleSuper.sha256));
        out.delete();
        final long[] last = {0};
        GoogleImage.buildSuper(new GoogleImage.HttpSource("http://127.0.0.1:" + port + "/drop/google.zip", null), f, out,
                (ph, d, tot) -> last[0] = d, null);
        check("GoogleImage: over HTTP with a dropped connection (resumed, same Inflater) -> the same super.img",
                sha(out).equals(f.googleSuper.sha256) && last[0] > 0);
        Feed wrong = Feed.parseSigned(read("feed.wrongsize"), mod);
        throwsWith("GoogleImage: a partition of another size = another Google build, refused", "not the Google build",
                () -> GoogleImage.buildSuper(new GoogleImage.FileSource(new File(T, "google.zip")), wrong, out, null, null));
        check("GoogleImage: no half-built super.img is left after a refusal", !out.exists());
        Feed wrongData = Feed.parseSigned(read("feed.wrongsha"), mod);
        throwsWith("GoogleImage: same sizes but other data is refused, naming the partition", "vendor_a",
                () -> GoogleImage.buildSuper(new GoogleImage.FileSource(new File(T, "google.zip")), wrongData, out, null, null));
        final Util.Cancel c = new Util.Cancel();
        c.cancel();
        throwsWith("GoogleImage: stop", "stopped", () -> GoogleImage.buildSuper(new GoogleImage.FileSource(new File(T, "google.zip")), f, out, null, c));

        // ---- downloads with resume
        File dl = new File(T, "dl/bundle.tar");
        dl.getParentFile().mkdirs();
        String want = text("payload.sha");
        long size = new File(T, "payload.bin").length();
        try (FileOutputStream o = new FileOutputStream(dl.getPath() + ".part")) {
            o.write(read("payload.bin"), 0, 100000);
        }
        Http.download("http://127.0.0.1:" + port + "/payload.bin", dl, size, want, null, null);
        check("download: resumes a .part file and checks the hash", dl.isFile() && sha(dl).equals(want) && !new File(dl.getPath() + ".part").exists());
        dl.delete();
        Http.download("http://127.0.0.1:" + port + "/drop/payload.bin", dl, size, want, null, null);
        check("download: survives a dropped connection", sha(dl).equals(want));
        File dl2 = new File(T, "dl/other.tar");
        throwsWith("download: a file with another hash is refused and dropped", "damaged",
                () -> Http.download("http://127.0.0.1:" + port + "/payload.bin", dl2, size, "00" + want.substring(2), null, null));
        check("download: nothing kept after a hash mismatch", !dl2.exists() && !new File(dl2.getPath() + ".part").exists());
        throwsWith("download: https only", "non-https", () -> Http.download("http://example.com/x", dl2, 1, want, null, null));

        // ---- tar
        File ex = new File(T, "untar");
        ex.mkdirs();
        File top = Tar.extract(new File(T, "good.tar"), ex, null, null);
        check("tar: a ustar bundle (long paths) unpacks to its one top folder", top.getName().equals(text("good.top")));
        for (String evil : new String[] {"evil-symlink.tar", "evil-dotdot.tar", "evil-twotops.tar", "evil-abs.tar", "evil-hardlink.tar"}) {
            File d = new File(T, "untar-" + evil);
            d.mkdirs();
            throwsWith("tar: refuses " + evil, "tar", () -> Tar.extract(new File(T, evil), d, null, null));
        }

        // ---- RSA against openssl
        check("rsaVerify: openssl's signature checks out", Util.rsaVerify(read("msg.bin"), text("msg.sig"), mod));
        check("rsaVerify: not with another key", !Util.rsaVerify(read("msg.bin"), text("msg.sig"), other));

        if (fails == 0) System.out.println("CORE_JAVA_PASS");
        else System.out.println(fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
