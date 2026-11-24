package org.gbos86.updater.core;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.math.BigInteger;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.RSAPublicKeySpec;

/** Small helpers shared by the core classes. Plain Java (no Android classes), so tools/test_updater_core.sh runs them on a PC. */
public final class Util {
    private Util() {}

    /** Progress callback: phase text, bytes done, bytes in all (or -1). */
    public interface Progress { void update(String phase, long done, long total); }

    /** Set from another thread to stop a long operation; it then throws {@link Cancelled}. */
    public static final class Cancel {
        private volatile boolean cancelled;
        public void cancel() { cancelled = true; }
        public boolean isCancelled() { return cancelled; }
        public void check() throws Cancelled { if (cancelled) throw new Cancelled(); }
    }

    public static final class Cancelled extends IOException {
        public Cancelled() { super("stopped"); }
    }

    public static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    public static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) sb.append(Character.forDigit((x >> 4) & 15, 16)).append(Character.forDigit(x & 15, 16));
        return sb.toString();
    }

    public static byte[] unhex(String s) {
        if (s.length() % 2 != 0) throw new IllegalArgumentException("odd hex length");
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) {
            int hi = Character.digit(s.charAt(2 * i), 16), lo = Character.digit(s.charAt(2 * i + 1), 16);
            if (hi < 0 || lo < 0) throw new IllegalArgumentException("not hex");
            b[i] = (byte) ((hi << 4) | lo);
        }
        return b;
    }

    public static String sha256Hex(File f, Cancel cancel) throws IOException {
        MessageDigest md = sha256();
        byte[] buf = new byte[1 << 20];
        try (InputStream in = new FileInputStream(f)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                md.update(buf, 0, n);
                if (cancel != null) cancel.check();
            }
        }
        return hex(md.digest());
    }

    /**
     * RSA PKCS#1 v1.5 with SHA-256, the scheme of usb/sysupdate.sh and tools/make_system_update.sh. modulusHex is the 768 hex digits
     * of gbos/sysupdate.pub (e = 65537), sigHex the 768 hex digits of the signature.
     */
    public static boolean rsaVerify(byte[] data, String sigHex, String modulusHex) {
        try {
            if (!modulusHex.matches("[0-9A-Fa-f]{768}") || !sigHex.matches("[0-9A-Fa-f]{768}")) return false;
            BigInteger n = new BigInteger(modulusHex, 16);
            if (n.bitLength() != 3072) return false;
            PublicKey key = KeyFactory.getInstance("RSA").generatePublic(new RSAPublicKeySpec(n, BigInteger.valueOf(65537)));
            Signature s = Signature.getInstance("SHA256withRSA");
            s.initVerify(key);
            s.update(data);
            return s.verify(unhex(sigHex));
        } catch (Exception e) {
            return false;
        }
    }

    public static String size(long bytes) {
        if (bytes >= 1L << 30) return String.format(java.util.Locale.ROOT, "%.1f GB", bytes / (double) (1L << 30));
        if (bytes >= 1L << 20) return String.format(java.util.Locale.ROOT, "%.0f MB", bytes / (double) (1L << 20));
        return Math.max(1, (bytes + 1023) / 1024) + " KB";
    }
}
