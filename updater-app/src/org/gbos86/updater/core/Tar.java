package org.gbos86.updater.core;

import java.io.BufferedInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * Unpacks a bundle tar (tools/publish_system_update.sh writes ustar) into a folder: plain files and folders only, names in the
 * charset sysupdate.sh accepts, no "." / ".." parts, no absolute paths, one top folder. Anything else rejects the whole tar.
 * (The tar's sha256 was already checked against the signed feed; this is defence in depth, and the root service copies only the
 * files the signed manifest lists, refusing links.)
 */
public final class Tar {
    private Tar() {}

    /** Extracts into parent (which must exist); returns the bundle folder. */
    public static File extract(File tar, File parent, Util.Progress p, Util.Cancel cancel) throws IOException {
        String top = null;
        long total = tar.length(), done = 0;
        try (InputStream in = new BufferedInputStream(new FileInputStream(tar), 1 << 16)) {
            byte[] h = new byte[512];
            byte[] buf = new byte[1 << 16];
            String longName = null;
            while (true) {
                if (cancel != null) cancel.check();
                if (!readBlock(in, h)) throw new IOException("the tar ends without its end marker");
                done += 512;
                if (allZero(h)) break;
                checksum(h);
                char type = (char) h[156];
                long size = octal(h, 124, 12);
                String name = longName != null ? longName : name(h);
                longName = null;
                if (type == 'L') { // GNU long name: the next entry's name is this entry's data
                    if (size > 4096) throw new IOException("tar: long name too long");
                    byte[] nb = new byte[(int) size];
                    readFully(in, nb, (int) size);
                    skip(in, pad(size) - size);
                    done += pad(size);
                    int z = 0;
                    while (z < nb.length && nb[z] != 0) z++;
                    longName = new String(nb, 0, z, StandardCharsets.US_ASCII);
                    continue;
                }
                while (name.endsWith("/")) name = name.substring(0, name.length() - 1);
                if (name.startsWith("./")) name = name.substring(2);
                checkName(name);
                String first = name.contains("/") ? name.substring(0, name.indexOf('/')) : name;
                if (top == null) top = first;
                else if (!top.equals(first)) throw new IOException("tar: more than one top folder");
                File out = new File(parent, name);
                if (type == '5') {
                    if (size != 0) throw new IOException("tar: folder with data");
                    if (!out.isDirectory() && !out.mkdirs()) throw new IOException("cannot make " + out);
                } else if (type == '0' || type == 0) {
                    if (!name.contains("/")) throw new IOException("tar: file outside the bundle folder");
                    File d = out.getParentFile();
                    if (!d.isDirectory() && !d.mkdirs()) throw new IOException("cannot make " + d);
                    if (out.exists() && !out.delete()) throw new IOException("cannot replace " + out);
                    try (FileOutputStream o = new FileOutputStream(out)) {
                        long left = size;
                        while (left > 0) {
                            int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                            if (n < 0) throw new EOFException("the tar is cut off");
                            o.write(buf, 0, n);
                            left -= n;
                            done += n;
                            if (cancel != null) cancel.check();
                        }
                    }
                    skip(in, pad(size) - size);
                    done += pad(size) - size;
                    if (p != null) p.update("unpacking", done, total);
                } else {
                    throw new IOException("tar: entry type '" + type + "' (a link or special file) in " + name);
                }
            }
        }
        if (top == null) throw new IOException("the tar is empty");
        return new File(parent, top);
    }

    static void checkName(String name) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || !name.matches("[A-Za-z0-9._+,@=~/-]+") || name.length() > 1024)
            throw new IOException("tar: name not allowed: " + name);
        for (String part : name.split("/", -1))
            if (part.isEmpty() || part.equals(".") || part.equals("..")) throw new IOException("tar: name not allowed: " + name);
    }

    private static String name(byte[] h) {
        String n = cstr(h, 0, 100);
        if (new String(h, 257, 5, StandardCharsets.US_ASCII).equals("ustar")) {
            String prefix = cstr(h, 345, 155);
            if (!prefix.isEmpty()) n = prefix + "/" + n;
        }
        return n;
    }

    private static String cstr(byte[] b, int off, int len) {
        int e = off;
        while (e < off + len && b[e] != 0) e++;
        return new String(b, off, e - off, StandardCharsets.US_ASCII);
    }

    private static long octal(byte[] h, int off, int len) throws IOException {
        if ((h[off] & 0x80) != 0) throw new IOException("tar: binary size field");
        String s = cstr(h, off, len).trim();
        if (!s.matches("[0-7]{1,12}")) throw new IOException("tar: bad number field");
        return Long.parseLong(s, 8);
    }

    private static void checksum(byte[] h) throws IOException {
        long want = octal(h, 148, 8), sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? 32 : (h[i] & 255);
        if (sum != want) throw new IOException("tar: header checksum wrong (damaged file)");
    }

    private static long pad(long n) { return (n + 511) / 512 * 512; }

    private static boolean allZero(byte[] b) {
        for (byte x : b) if (x != 0) return false;
        return true;
    }

    private static boolean readBlock(InputStream in, byte[] b) throws IOException {
        int got = 0;
        while (got < b.length) {
            int n = in.read(b, got, b.length - got);
            if (n < 0) return false;
            got += n;
        }
        return true;
    }

    private static void readFully(InputStream in, byte[] b, int len) throws IOException {
        int got = 0;
        while (got < len) {
            int n = in.read(b, got, len - got);
            if (n < 0) throw new EOFException("the tar is cut off");
            got += n;
        }
    }

    private static void skip(InputStream in, long n) throws IOException {
        while (n > 0) {
            long k = in.skip(n);
            if (k <= 0) {
                if (in.read() < 0) throw new EOFException("the tar is cut off");
                k = 1;
            }
            n -= k;
        }
    }
}
