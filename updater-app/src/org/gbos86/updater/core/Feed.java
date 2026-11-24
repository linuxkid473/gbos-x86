package org.gbos86.updater.core;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The update feed tools/publish_system_update.sh writes (gbos86-update.feed on the project's GitHub release "sysupdate-feed"):
 * signed text up to "end N", then one line "rsa3072-sha256 <signature>" made with the same key as the bundles. The app trusts nothing
 * in it before the signature checks out against the installed gbos/sysupdate.pub; the laptop checks the bundle itself again anyway.
 * The format is strict: an unknown or malformed line rejects the whole feed (a newer format gets a new first line).
 */
public final class Feed {
    public static final String HEADER = "gbos86-update-feed 1";

    public static final class Asset {
        public final String kind, file, sha256;
        public final long size;
        Asset(String kind, String file, long size, String sha256) { this.kind = kind; this.file = file; this.size = size; this.sha256 = sha256; }
    }

    public static final class Part {
        public final String name, sha256;
        public final long bytes;
        Part(String name, long bytes, String sha256) { this.name = name; this.bytes = bytes; this.sha256 = sha256; }
    }

    public long seq, googleBuild;
    public String set, name, commit, created, url, googleZip, googleVersion;
    public final List<String> bases = new ArrayList<>();
    public final Map<String, Asset> assets = new HashMap<>();
    public final List<Part> googleParts = new ArrayList<>();
    public Part googleSuper;
    public final List<String> notes = new ArrayList<>();

    public static final class FeedException extends Exception {
        FeedException(String m) { super(m); }
    }

    public Asset asset(String kind) { return assets.get(kind); }

    /**
     * A Google build as people name it: "R5" from a version like CL3B.260622.276.R5, else the version, else "build N".
     * Display only (the version comes from the signed feed or from the running system's ro.build.id).
     */
    public static String googleName(String version, long build) {
        if (version != null) {
            Matcher m = Pattern.compile(".*\\.(R[0-9]{1,3})").matcher(version);
            if (m.matches()) return m.group(1);
            if (!version.isEmpty()) return version;
        }
        return build > 0 ? "build " + build : "unknown";
    }
    public String assetUrl(Asset a) { return url + a.file; }
    public boolean canRebuildSuper() { return googleZip != null && googleSuper != null && !googleParts.isEmpty(); }
    public List<String> notes() { return Collections.unmodifiableList(notes); }

    /** Checks the signature with the installed key (768 hex digits), then parses. */
    public static Feed parseSigned(byte[] file, String modulusHex) throws FeedException {
        if (file.length < 100 || file.length > 65536) throw new FeedException("the update feed has a wrong size");
        if (file[file.length - 1] != '\n') throw new FeedException("the update feed is cut off");
        int last = file.length - 2;
        while (last >= 0 && file[last] != '\n') last--;
        if (last < 0) throw new FeedException("the update feed is not signed");
        String sigLine = new String(file, last + 1, file.length - last - 2, StandardCharsets.US_ASCII);
        Matcher m = Pattern.compile("rsa3072-sha256 ([0-9a-f]{768})").matcher(sigLine);
        if (!m.matches()) throw new FeedException("the update feed has no signature line");
        byte[] body = new byte[last + 1];
        System.arraycopy(file, 0, body, 0, body.length);
        if (modulusHex == null || !Util.rsaVerify(body, m.group(1), modulusHex))
            throw new FeedException("the update feed's signature is wrong (not signed with this laptop's update key)");
        return parse(new String(body, StandardCharsets.US_ASCII));
    }

    private static final Pattern SEQ = Pattern.compile("[1-9][0-9]{0,11}");
    private static final Pattern HEX64 = Pattern.compile("[0-9a-f]{64}");
    private static final Pattern NAME = Pattern.compile("gbos86-update-[0-9]{12}-[0-9a-f]{12}");
    private static final Pattern FILE = Pattern.compile("[A-Za-z0-9][A-Za-z0-9._-]{0,200}");
    private static final Pattern URL = Pattern.compile("https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~%+-]+)*/");
    private static final Pattern GURL = Pattern.compile("https://dl\\.google\\.com(/[A-Za-z0-9._~%+-]+)+");
    private static final Pattern PART = Pattern.compile("[a-z][a-z0-9_]{0,30}");

    static Feed parse(String text) throws FeedException {
        String[] lines = text.split("\n", -1);
        if (lines.length < 3 || !lines[lines.length - 1].isEmpty()) throw new FeedException("the update feed is cut off");
        if (!HEADER.equals(lines[0])) throw new FeedException("not a gbOS86 update feed (or a newer format: update the app)");
        Feed f = new Feed();
        int count = 1;          // "end N": N counts every line before it, the header included
        boolean end = false;
        for (int i = 1; i < lines.length - 1; i++) {
            String l = lines[i];
            if (end) throw new FeedException("text after the end line");
            for (int k = 0; k < l.length(); k++) {
                char c = l.charAt(k);
                if (c < 0x20 || c > 0x7e) throw new FeedException("line " + (i + 1) + ": character not allowed");
            }
            int sp = l.indexOf(' ');
            if (sp <= 0) throw new FeedException("line " + (i + 1) + ": no value");
            String key = l.substring(0, sp), v = l.substring(sp + 1);
            String[] w = v.split(" ", -1);
            switch (key) {
                case "seq": f.seq = num(SEQ, v, "seq"); break;
                case "set": f.set = re(HEX64, v, "set"); break;
                case "name": f.name = re(NAME, v, "name"); break;
                case "commit": f.commit = re(Pattern.compile("[0-9a-f]{40}"), v, "commit"); break;
                case "google_build": f.googleBuild = num(SEQ, v, "google_build"); break;
                case "google_version": f.googleVersion = re(Pattern.compile("[A-Z0-9][A-Z0-9.]{0,39}"), v, "google_version"); break;
                case "created": f.created = re(Pattern.compile("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"), v, "created"); break;
                case "base": f.bases.add(re(HEX64, v, "base")); break;
                case "url": f.url = re(URL, v, "url"); break;
                case "asset": {
                    if (w.length != 4 || !w[0].matches("full|delta|manifest")) throw new FeedException("bad asset line");
                    if (f.assets.containsKey(w[0])) throw new FeedException("asset " + w[0] + " twice");
                    f.assets.put(w[0], new Asset(w[0], re(FILE, w[1], "asset file"), num(Pattern.compile("[0-9]{1,13}"), w[2], "asset size"),
                            re(HEX64, w[3], "asset sha256")));
                    break;
                }
                case "google_zip": f.googleZip = re(GURL, v, "google_zip"); break;
                case "google_part": {
                    if (w.length != 3) throw new FeedException("bad google_part line");
                    long b = num(Pattern.compile("[1-9][0-9]{0,12}"), w[1], "google_part size");
                    if (b % 4096 != 0) throw new FeedException("google_part size not 4 KiB aligned");
                    for (Part p : f.googleParts) if (p.name.equals(w[0])) throw new FeedException("google_part " + w[0] + " twice");
                    f.googleParts.add(new Part(re(PART, w[0], "google_part name"), b, re(HEX64, w[2], "google_part sha256")));
                    break;
                }
                case "google_super": {
                    if (w.length != 2) throw new FeedException("bad google_super line");
                    f.googleSuper = new Part("super", num(Pattern.compile("[1-9][0-9]{0,12}"), w[0], "google_super size"), re(HEX64, w[1], "google_super sha256"));
                    break;
                }
                case "note": if (f.notes.size() < 20) f.notes.add(v.length() > 200 ? v.substring(0, 200) : v); break;
                case "end": {
                    if (num(SEQ, v, "end") != count) throw new FeedException("end count does not match");
                    end = true;
                    break;
                }
                default: throw new FeedException("unknown line: " + key);
            }
            if (!end) count++;
        }
        if (!end) throw new FeedException("the update feed has no end line");
        if (f.seq == 0 || f.set == null || f.name == null || f.commit == null || f.googleBuild == 0 || f.created == null || f.url == null)
            throw new FeedException("the update feed lacks a required line");
        if (f.asset("full") == null || f.asset("manifest") == null) throw new FeedException("the update feed lacks the full bundle");
        if (!f.name.startsWith(String.format(java.util.Locale.ROOT, "gbos86-update-%012d-", f.seq)) || !f.name.endsWith(f.set.substring(0, 12)))
            throw new FeedException("the bundle name does not match seq/set");
        boolean anyGoogle = f.googleZip != null || f.googleSuper != null || !f.googleParts.isEmpty();
        if (anyGoogle && !f.canRebuildSuper()) throw new FeedException("incomplete google_ lines");
        if (f.googleParts.size() > 6) throw new FeedException("more than 6 google_part lines");
        return f;
    }

    private static String re(Pattern p, String v, String what) throws FeedException {
        if (!p.matcher(v).matches()) throw new FeedException("bad " + what + ": " + (v.length() > 80 ? v.substring(0, 80) : v));
        return v;
    }

    private static long num(Pattern p, String v, String what) throws FeedException {
        return Long.parseLong(re(p, v, what));
    }
}
