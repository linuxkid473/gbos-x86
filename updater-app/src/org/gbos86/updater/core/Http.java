package org.gbos86.updater.core;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * HTTPS downloads with resume. GitHub release assets redirect to another host (objects.githubusercontent.com), which
 * HttpURLConnection follows; ranges keep working there and on dl.google.com.
 */
public final class Http {
    private Http() {}

    public static String userAgent = "gbos86-updater/1";
    /** Tests point downloads at a local http server; the app only ever accepts https. */
    public static boolean allowPlainHttpForTests = false;
    static final int RETRIES = 8;

    static HttpURLConnection open(String url, long from, String ifRange) throws IOException {
        if (!url.startsWith("https://") && !(allowPlainHttpForTests && url.startsWith("http://127.0.0.1:")))
            throw new IOException("refusing a non-https address: " + url);
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(20000);
        c.setReadTimeout(60000);
        c.setInstanceFollowRedirects(true);
        c.setRequestProperty("User-Agent", userAgent);
        c.setRequestProperty("Accept-Encoding", "identity");
        if (from > 0) {
            c.setRequestProperty("Range", "bytes=" + from + "-");
            if (ifRange != null) c.setRequestProperty("If-Range", ifRange);
        }
        return c;
    }

    /** A small file (the feed), at most max bytes. */
    public static byte[] get(String url, int max) throws IOException {
        HttpURLConnection c = open(url, 0, null);
        try {
            int code = c.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code + " for " + url);
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    if (out.size() > max) throw new IOException("too big: " + url);
                }
                return out.toByteArray();
            }
        } finally {
            c.disconnect();
        }
    }

    /** HEAD-like status check (GET without reading the body): 200 -> true, 404/422 -> false, otherwise an exception. */
    public static boolean exists(String url) throws IOException {
        HttpURLConnection c = open(url, 0, null);
        try {
            c.setRequestProperty("Accept", "application/vnd.github+json");
            int code = c.getResponseCode();
            if (code == 200) return true;
            if (code == 404 || code == 422) return false;
            throw new IOException("HTTP " + code);
        } finally {
            c.disconnect();
        }
    }

    /**
     * Downloads url to dest (via dest.part, resumed from where an earlier attempt stopped), checks size and sha256, then renames.
     * A file of the right name, size and hash is kept as it is.
     */
    public static void download(String url, File dest, long size, String sha256, Util.Progress p, Util.Cancel cancel) throws IOException {
        if (dest.isFile() && dest.length() == size && Util.sha256Hex(dest, cancel).equals(sha256)) return;
        File part = new File(dest.getPath() + ".part");
        if (part.length() > size) part.delete();
        try (InputStream in = new ResumingStream(url, part.length(), cancel);
             FileOutputStream out = new FileOutputStream(part, true)) {
            long done = part.length();
            byte[] buf = new byte[1 << 16];
            int n;
            long lastReport = 0;
            while (done < size && (n = in.read(buf, 0, (int) Math.min(buf.length, size - done))) > 0) {
                out.write(buf, 0, n);
                done += n;
                if (p != null && done - lastReport > (1 << 20)) { p.update("downloading", done, size); lastReport = done; }
            }
            out.getFD().sync();
        }
        if (part.length() != size) throw new IOException("the download ended early (" + part.length() + " of " + size + " bytes)");
        if (p != null) p.update("checking the download", size, size);
        String got = Util.sha256Hex(part, cancel);
        if (!got.equals(sha256)) {
            part.delete();
            throw new IOException("the download is damaged or not the published file (sha256 " + got.substring(0, 16) + "...): try again");
        }
        dest.delete();
        if (!part.renameTo(dest)) throw new IOException("cannot rename " + part);
    }

    /**
     * An InputStream over url starting at byte 'from' that reconnects by itself (Range + If-Range on the first answer's ETag or
     * Last-Modified) when the connection breaks, up to RETRIES times in a row. A server that answers a resumed request with the whole
     * file (the file changed) is an error. Callers that keep their own state over the bytes (an Inflater) depend on exactly this.
     */
    public static final class ResumingStream extends InputStream {
        private final String url;
        private final Util.Cancel cancel;
        private long pos;
        private String validator;
        private HttpURLConnection conn;
        private InputStream in;
        private long length = -1;

        public ResumingStream(String url, long from, Util.Cancel cancel) throws IOException {
            this.url = url;
            this.pos = from;
            this.cancel = cancel;
            connect(false);
        }

        /** Total length of the resource, when the server said. */
        public long length() { return length; }
        public long position() { return pos; }

        private void connect(boolean resume) throws IOException {
            close();
            conn = open(url, pos, resume ? validator : null);
            int code = conn.getResponseCode();
            if (pos > 0 && code == 200) {
                if (resume) throw new IOException("the file on the server changed during the download");
                // no range support: skip to the position
                in = conn.getInputStream();
                long left = pos;
                while (left > 0) {
                    long k = in.skip(left);
                    if (k <= 0) { if (in.read() < 0) throw new IOException("the file on the server is shorter than expected"); k = 1; }
                    left -= k;
                }
            } else if ((pos > 0 && code == 206) || (pos == 0 && code == 200)) {
                in = conn.getInputStream();
            } else if (code == 416) {
                throw new IOException("the server has no bytes from " + pos + " on (HTTP 416)");
            } else {
                throw new IOException("HTTP " + code + " for " + url);
            }
            if (!resume) {
                String et = conn.getHeaderField("ETag");
                validator = et != null && !et.startsWith("W/") ? et : conn.getHeaderField("Last-Modified");
                if (code == 200) length = conn.getContentLengthLong();
                else {
                    String cr = conn.getHeaderField("Content-Range");
                    if (cr != null && cr.matches("bytes [0-9]+-[0-9]+/[0-9]+")) length = Long.parseLong(cr.substring(cr.indexOf('/') + 1));
                }
            }
        }

        @Override
        public int read() throws IOException {
            byte[] b = new byte[1];
            int n = read(b, 0, 1);
            return n < 0 ? -1 : (b[0] & 255);
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            int failures = 0;
            while (true) {
                if (cancel != null) cancel.check();
                try {
                    int n = in.read(b, off, len);
                    if (n > 0) pos += n;
                    if (n < 0 && length >= 0 && pos < length) throw new IOException("connection closed early");
                    return n;
                } catch (Util.Cancelled e) {
                    throw e;
                } catch (IOException e) {
                    if (++failures > RETRIES || validator == null) throw e;
                    try {
                        Thread.sleep(Math.min(30000, 1000L << failures));
                    } catch (InterruptedException ie) {
                        throw new Util.Cancelled();
                    }
                    try {
                        connect(true);
                    } catch (Util.Cancelled c) {
                        throw c;
                    } catch (IOException again) {
                        if (failures >= RETRIES) throw again;
                    }
                }
            }
        }

        @Override
        public void close() {
            try {
                if (in != null) in.close();
            } catch (IOException ignored) {
            }
            if (conn != null) conn.disconnect();
            in = null;
            conn = null;
        }
    }
}
