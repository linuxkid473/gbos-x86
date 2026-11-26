package org.gbos86.updater.core;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.spec.MGF1ParameterSpec;
import java.security.spec.RSAPublicKeySpec;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.OAEPParameterSpec;
import javax.crypto.spec.PSource;
import javax.crypto.spec.SecretKeySpec;

/**
 * Encrypts a bug report for the maintainer (docs/bug-report-plan.md), so it can be attached to a public GitHub issue.
 * Format, read by tools/open_report.sh with openssl only:
 *   "gbos86-report-encrypted 1\n"
 *   384 bytes  RSA-3072 OAEP (SHA-256, MGF1-SHA-256) of 64 random bytes: 32 AES-256 key, 32 HMAC-SHA256 key
 *   16 bytes   AES-CTR IV
 *   ...        AES-256-CTR of the report (.tar.gz)
 *   32 bytes   HMAC-SHA256 of everything before it
 * The public key is gbos/report.pub (sysupdate.sh info: report_pub=MODULUS, exponent 65537), shipped with the system set.
 */
public final class ReportCrypto {
    private ReportCrypto() {}

    public static final String HEADER = "gbos86-report-encrypted 1\n";

    public static void encrypt(InputStream in, OutputStream out, String modulusHex) throws IOException {
        try {
            PublicKey pub = KeyFactory.getInstance("RSA").generatePublic(new RSAPublicKeySpec(new BigInteger(modulusHex, 16), BigInteger.valueOf(65537)));
            if (new BigInteger(modulusHex, 16).bitLength() != 3072) throw new IOException("the report key is not RSA-3072");
            SecureRandom rnd = new SecureRandom();
            byte[] keys = new byte[64], iv = new byte[16];
            rnd.nextBytes(keys);
            rnd.nextBytes(iv);
            Cipher rsa = Cipher.getInstance("RSA/ECB/OAEPPadding");
            rsa.init(Cipher.ENCRYPT_MODE, pub, new OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA256, PSource.PSpecified.DEFAULT));
            byte[] wrapped = rsa.doFinal(keys);
            Cipher aes = Cipher.getInstance("AES/CTR/NoPadding");
            aes.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(Arrays.copyOfRange(keys, 0, 32), "AES"), new IvParameterSpec(iv));
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(Arrays.copyOfRange(keys, 32, 64), "HmacSHA256"));
            Arrays.fill(keys, (byte) 0);
            byte[] h = HEADER.getBytes(StandardCharsets.US_ASCII);
            for (byte[] b : new byte[][] {h, wrapped, iv}) { out.write(b); mac.update(b); }
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) {
                byte[] c = aes.update(buf, 0, n);
                if (c != null && c.length > 0) { out.write(c); mac.update(c); }
            }
            byte[] c = aes.doFinal();
            if (c != null && c.length > 0) { out.write(c); mac.update(c); }
            out.write(mac.doFinal());
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("encrypting the report failed: " + e, e);
        }
    }

    /**
     * A new-issue link on github.com/REPO with a public title and body that name the report and ask for the attachment. The user's
     * description is NOT in it: it stays inside the encrypted file.
     */
    public static String issueUrl(String repo, String reportId, long seq, String googleName, String fileName) {
        String title = "Bug report " + reportId + (seq > 0 ? " (update " + seq + ")" : "");
        String body = "Encrypted bug report from the gbOS86 Updater app. Only the maintainer can open it (tools/open_report.sh).\n\n"
                + "- Report: " + reportId + "\n"
                + "- gbOS86 update: " + (seq > 0 ? seq : "installed from the USB stick") + "\n"
                + "- Googlebook OS: " + googleName + "\n\n"
                + "**Attach the file `Download/gbOS86/" + fileName + "`** before you submit (drag it into this box, or tap "
                + "\"Paste, drop, or click to add files\").\n";
        return "https://github.com/" + repo + "/issues/new?title=" + enc(title) + "&body=" + enc(body);
    }

    private static String enc(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8").replace("+", "%20");
        } catch (java.io.UnsupportedEncodingException e) {
            throw new IllegalStateException(e);
        }
    }
}
