package org.gbos86.updater.core;

import java.io.BufferedInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;

/**
 * Rebuilds gbOS86's super.img from Google's own recovery.zip (experimental; tools/fetch_google_parts.py + usb/make_super.py in one
 * pass, in Java). The zip is never stored: its directory is read with two small range requests, then the raw disk image inside it is
 * inflated as one stream. The GPT and the super partition's liblp metadata come from its first megabytes; the bytes of the wanted
 * logical partitions go straight to their place in super.img, and reading stops after the last one. The result must have the size
 * and sha256 the signed feed names (each partition is checked too, to say which one differs); the laptop checks it again at boot.
 */
public final class GoogleImage {
    private GoogleImage() {}

    /** Random access to the zip: its length and a stream from any offset. */
    public interface Source {
        long length() throws IOException;
        InputStream open(long from) throws IOException;
    }

    public static final class HttpSource implements Source {
        private final String url;
        private final Util.Cancel cancel;
        private long length = -1;
        public HttpSource(String url, Util.Cancel cancel) { this.url = url; this.cancel = cancel; }
        public long length() throws IOException {
            if (length < 0) {
                Http.ResumingStream s = new Http.ResumingStream(url, 0, cancel);
                length = s.length();
                s.close();
                if (length <= 0) throw new IOException("the server did not say how big Google's file is");
            }
            return length;
        }
        public InputStream open(long from) throws IOException { return new Http.ResumingStream(url, from, cancel); }
    }

    public static final class FileSource implements Source {
        private final File f;
        public FileSource(File f) { this.f = f; }
        public long length() { return f.length(); }
        public InputStream open(long from) throws IOException {
            FileInputStream in = new FileInputStream(f);
            if (in.skip(from) != from) { in.close(); throw new EOFException(); }
            return in;
        }
    }

    // ------------------------------------------------------------------------------------------------ zip
    static final class Entry {
        String name;
        int method, flags;
        long comp, size, local;
    }

    private static ByteBuffer le(byte[] b) { return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN); }

    private static byte[] read(Source s, long from, int n) throws IOException {
        byte[] b = new byte[n];
        try (InputStream in = s.open(from)) { readFully(in, b, 0, n); }
        return b;
    }

    static void readFully(InputStream in, byte[] b, int off, int n) throws IOException {
        while (n > 0) {
            int k = in.read(b, off, n);
            if (k < 0) throw new EOFException("Google's file ended early");
            off += k;
            n -= k;
        }
    }

    /** The biggest entry of the zip (the disk image), from the central directory (zip64 aware). */
    static Entry biggestEntry(Source s) throws IOException {
        long len = s.length();
        int tailLen = (int) Math.min(len, 65536 + 22 + 20 + 56);
        byte[] tail = read(s, len - tailLen, tailLen);
        int e = -1;
        for (int i = tailLen - 22; i >= 0; i--) if (le(tail).getInt(i) == 0x06054b50) { e = i; break; }
        if (e < 0) throw new IOException("Google's file is not a zip (no end record)");
        ByteBuffer eb = le(tail);
        long cdSize = eb.getInt(e + 12) & 0xffffffffL, cdOff = eb.getInt(e + 16) & 0xffffffffL;
        long entries = eb.getShort(e + 10) & 0xffff;
        if (cdOff == 0xffffffffL || cdSize == 0xffffffffL || entries == 0xffff) {
            int loc = e - 20;
            if (loc < 0 || eb.getInt(loc) != 0x07064b50) throw new IOException("zip64 locator missing");
            long recOff = eb.getLong(loc + 8);
            ByteBuffer r = le(read(s, recOff, 56));
            if (r.getInt(0) != 0x06064b50) throw new IOException("zip64 end record missing");
            entries = r.getLong(32);
            cdSize = r.getLong(40);
            cdOff = r.getLong(48);
        }
        if (cdSize > (16 << 20) || entries > 100000) throw new IOException("zip directory too big");
        ByteBuffer cd = le(read(s, cdOff, (int) cdSize));
        Entry best = null;
        int p = 0;
        for (long i = 0; i < entries; i++) {
            if (cd.getInt(p) != 0x02014b50) throw new IOException("bad zip directory");
            Entry x = new Entry();
            x.flags = cd.getShort(p + 8) & 0xffff;
            x.method = cd.getShort(p + 10) & 0xffff;
            x.comp = cd.getInt(p + 20) & 0xffffffffL;
            x.size = cd.getInt(p + 24) & 0xffffffffL;
            int nl = cd.getShort(p + 28) & 0xffff, xl = cd.getShort(p + 30) & 0xffff, cl = cd.getShort(p + 32) & 0xffff;
            x.local = cd.getInt(p + 42) & 0xffffffffL;
            byte[] nb = new byte[nl];
            cd.position(p + 46);
            cd.get(nb);
            x.name = new String(nb, StandardCharsets.UTF_8);
            int q = p + 46 + nl, end = q + xl;
            while (q + 4 <= end) {
                int id = cd.getShort(q) & 0xffff, sz = cd.getShort(q + 2) & 0xffff;
                if (id == 1) {
                    int r = q + 4;
                    if (x.size == 0xffffffffL) { x.size = cd.getLong(r); r += 8; }
                    if (x.comp == 0xffffffffL) { x.comp = cd.getLong(r); r += 8; }
                    if (x.local == 0xffffffffL) { x.local = cd.getLong(r); }
                }
                q += 4 + sz;
            }
            if (best == null || x.size > best.size) best = x;
            p += 46 + nl + xl + cl;
        }
        if (best == null) throw new IOException("the zip is empty");
        if ((best.flags & 1) != 0) throw new IOException("the zip entry is encrypted");
        if (best.method != 0 && best.method != 8) throw new IOException("unsupported zip compression " + best.method);
        return best;
    }

    /** Stream of the entry's uncompressed bytes. */
    static InputStream entryStream(Source s, Entry e) throws IOException {
        InputStream raw = s.open(e.local);
        byte[] h = new byte[30];
        readFully(raw, h, 0, 30);
        ByteBuffer b = le(h);
        if (b.getInt(0) != 0x04034b50) { raw.close(); throw new IOException("bad zip entry header"); }
        long skip = (b.getShort(26) & 0xffff) + (b.getShort(28) & 0xffff);
        byte[] junk = new byte[(int) skip];
        readFully(raw, junk, 0, junk.length);
        InputStream bounded = new Bounded(raw, e.comp);
        if (e.method == 0) return bounded;
        return new InflaterInputStream(new BufferedInputStream(bounded, 1 << 16), new Inflater(true), 1 << 16);
    }

    static final class Bounded extends InputStream {
        private final InputStream in;
        private long left;
        Bounded(InputStream in, long n) { this.in = in; this.left = n; }
        public int read() throws IOException {
            if (left <= 0) return -1;
            int c = in.read();
            if (c >= 0) left--;
            return c;
        }
        public int read(byte[] b, int off, int len) throws IOException {
            if (left <= 0) return -1;
            int n = in.read(b, off, (int) Math.min(len, left));
            if (n > 0) left -= n;
            return n;
        }
        public void close() throws IOException { in.close(); }
    }

    /** Forward-only reader over the disk image: skipping is reading and dropping (which is all GPT/liblp parsing needs). */
    static final class Forward {
        final InputStream in;
        long pos;
        final byte[] scratch = new byte[1 << 20];
        final Util.Cancel cancel;
        Forward(InputStream in, Util.Cancel cancel) { this.in = in; this.cancel = cancel; }
        void skipTo(long off) throws IOException {
            if (off < pos) throw new IOException("the disk image is not in the order gbOS86 expects (backward seek)");
            while (pos < off) {
                int n = in.read(scratch, 0, (int) Math.min(scratch.length, off - pos));
                if (n < 0) throw new EOFException("the disk image ended early");
                pos += n;
                if (cancel != null) cancel.check();
            }
        }
        byte[] read(int n) throws IOException {
            byte[] b = new byte[n];
            readFully(in, b, 0, n);
            pos += n;
            return b;
        }
    }

    // ------------------------------------------------------------------------------------------------ GPT and liblp
    static final class Extent {
        final long disk, length, superOffset;
        Extent(long disk, long length, long superOffset) { this.disk = disk; this.length = length; this.superOffset = superOffset; }
    }

    /** One logical partition: its size and its linear extents (disk offset, length, offset inside the partition); zero extents dropped. */
    static final class Logical {
        long size;
        final List<long[]> linear = new ArrayList<>();
    }

    static long[] findSuper(Forward f) throws IOException {
        f.skipTo(512);
        ByteBuffer h = le(f.read(92));
        if (!new String(h.array(), 0, 8, StandardCharsets.US_ASCII).equals("EFI PART")) throw new IOException("no GPT in Google's disk image");
        long entriesLba = h.getLong(72);
        int count = h.getInt(80), size = h.getInt(84);
        if (count <= 0 || count > 1024 || size < 128 || size > 4096) throw new IOException("unexpected GPT table");
        f.skipTo(entriesLba * 512);
        ByteBuffer t = le(f.read(count * size));
        for (int i = 0; i < count; i++) {
            int o = i * size;
            long first = t.getLong(o + 32), last = t.getLong(o + 40);
            byte[] nb = new byte[72];
            t.position(o + 56);
            t.get(nb);
            String name = new String(nb, StandardCharsets.UTF_16LE);
            int z = name.indexOf('\0');
            if (z >= 0) name = name.substring(0, z);
            if (name.equals("super") || name.equals("super_a")) return new long[] {first * 512, (last + 1) * 512};
        }
        throw new IOException("no super partition in Google's disk image");
    }

    static java.util.Map<String, Logical> liblp(Forward f, long base) throws IOException {
        f.skipTo(base + 4096);
        ByteBuffer g = le(f.read(4096));
        if (g.getInt(0) != 0x616C4467) throw new IOException("bad liblp geometry");
        int metaMax = g.getInt(40);
        if (metaMax <= 0 || metaMax > (1 << 20)) throw new IOException("unexpected liblp metadata size");
        f.skipTo(base + 4096 + 2 * 4096);
        ByteBuffer m = le(f.read(metaMax));
        if (m.getInt(0) != 0x414C5030) throw new IOException("bad liblp metadata");
        int major = m.getShort(4) & 0xffff;
        if (major != 10) throw new IOException("liblp metadata version " + major + " (gbOS86 knows 10)");
        int hsize = m.getInt(8), tsize = m.getInt(44);
        int[] d = new int[12];
        for (int i = 0; i < 12; i++) d[i] = m.getInt(80 + 4 * i);
        if (hsize + (long) tsize > metaMax) throw new IOException("liblp tables too big");
        int pOff = hsize + d[0], pN = d[1], pSz = d[2], eOff = hsize + d[3], eN = d[4], eSz = d[5];
        java.util.Map<String, Logical> out = new java.util.HashMap<>();
        for (int i = 0; i < pN; i++) {
            int o = pOff + i * pSz;
            byte[] nb = new byte[36];
            m.position(o);
            m.get(nb);
            int z = 0;
            while (z < 36 && nb[z] != 0) z++;
            String name = new String(nb, 0, z, StandardCharsets.US_ASCII);
            int first = m.getInt(o + 40), num = m.getInt(o + 44);
            Logical l = new Logical();
            for (int k = first; k < first + num; k++) {
                int eo = eOff + k * eSz;
                if (k >= eN) throw new IOException("liblp extent index out of range");
                long n = m.getLong(eo) * 512;
                int type = m.getInt(eo + 8);
                long data = m.getLong(eo + 12);
                int src = m.getInt(eo + 20);
                if (type == 0) {
                    if (src != 0) throw new IOException("super spans several disks: not supported");
                    l.linear.add(new long[] {base + data * 512, n, l.size});
                } else if (type != 1) {
                    throw new IOException("unknown liblp extent type " + type);
                }
                l.size += n;
            }
            out.put(name, l);
        }
        return out;
    }

    // ------------------------------------------------------------------------------------------------ the whole job
    /**
     * Builds out (super.img) for the feed's google_part list from the zip. Throws with a plain message when anything differs from
     * what the signed feed says. out is deleted on failure.
     */
    public static void buildSuper(Source src, Feed feed, File out, Util.Progress p, Util.Cancel cancel) throws IOException {
        if (!feed.canRebuildSuper()) throw new IOException("this update does not say how to rebuild super.img from Google's download");
        int n = feed.googleParts.size();
        String[] names = new String[n];
        long[] sizes = new long[n];
        for (int i = 0; i < n; i++) { names[i] = feed.googleParts.get(i).name; sizes[i] = feed.googleParts.get(i).bytes; }
        SuperImage plan = SuperImage.plan(names, sizes);
        if (plan.total != feed.googleSuper.bytes) throw new IOException("internal: the planned super.img size differs from the feed's");
        File dir = out.getAbsoluteFile().getParentFile();
        if (dir.getUsableSpace() < plan.total + (256L << 20))
            throw new IOException("not enough free space for super.img: needs " + Util.size(plan.total + (256L << 20)) + ", "
                    + Util.size(dir.getUsableSpace()) + " free");
        if (p != null) p.update("asking Google for the file's directory", 0, -1);
        Entry e = biggestEntry(src);
        boolean ok = false;
        out.delete();
        try (RandomAccessFile raf = new RandomAccessFile(out, "rw");
             InputStream img = entryStream(src, e)) {
            raf.setLength(plan.total);
            plan.writeMetadata(raf);
            Forward f = new Forward(img, cancel);
            long[] sup = findSuper(f);
            java.util.Map<String, Logical> lps = liblp(f, sup[0]);
            List<Extent> jobs = new ArrayList<>();
            for (SuperImage.Placed pl : plan.parts) {
                Logical l = lps.get(pl.name);
                if (l == null) throw new IOException("Google's image has no partition " + pl.name);
                if (SuperImage.up(l.size, 4096) != pl.alignedSize)
                    throw new IOException("Google's " + pl.name + " is " + l.size + " bytes, the update expects " + pl.alignedSize
                            + ": this is not the Google build the update was made for");
                for (long[] x : l.linear) jobs.add(new Extent(x[0], x[1], pl.offset + x[2]));
            }
            Collections.sort(jobs, (a, b) -> Long.compare(a.disk, b.disk));
            for (int i = 1; i < jobs.size(); i++)
                if (jobs.get(i - 1).disk + jobs.get(i - 1).length > jobs.get(i).disk) throw new IOException("overlapping extents in Google's image");
            long total = 0, done = 0;
            for (Extent x : jobs) total += x.length;
            byte[] buf = new byte[1 << 20];
            long last = 0;
            for (Extent x : jobs) {
                f.skipTo(x.disk);
                raf.seek(x.superOffset);
                long left = x.length;
                while (left > 0) {
                    int k = f.in.read(buf, 0, (int) Math.min(buf.length, left));
                    if (k < 0) throw new EOFException("Google's disk image ended inside a partition");
                    f.pos += k;
                    raf.write(buf, 0, k);
                    left -= k;
                    done += k;
                    if (cancel != null) cancel.check();
                    if (p != null && done - last > (8 << 20)) { p.update("downloading Google's system image", done, total); last = done; }
                }
            }
            raf.getFD().sync();
            ok = true;
        } finally {
            if (!ok) out.delete();
        }
        verify(out, plan, feed, p, cancel);
    }

    /** Whole-file and per-partition sha256 against the feed. */
    static void verify(File out, SuperImage plan, Feed feed, Util.Progress p, Util.Cancel cancel) throws IOException {
        MessageDigest whole = Util.sha256();
        byte[] buf = new byte[1 << 20];
        String bad = null;
        try (InputStream in = new BufferedInputStream(new FileInputStream(out), 1 << 20)) {
            long pos = 0;
            for (int i = 0; i <= plan.parts.size(); i++) {
                long start = i < plan.parts.size() ? plan.parts.get(i).offset : plan.total;
                pos = feed(in, whole, null, start - pos, buf, cancel) + pos;
                if (i == plan.parts.size()) break;
                SuperImage.Placed pl = plan.parts.get(i);
                MessageDigest md = Util.sha256();
                pos += feed(in, whole, md, pl.alignedSize, buf, cancel);
                if (bad == null && !Util.hex(md.digest()).equals(feed.googleParts.get(i).sha256)) bad = pl.name;
                if (p != null) p.update("checking super.img", pos, plan.total);
            }
        }
        if (bad != null || !Util.hex(whole.digest()).equals(feed.googleSuper.sha256)) {
            out.delete();
            throw new IOException(bad != null ? "Google's " + bad + " is not the one the update was made for (different data)"
                    : "the rebuilt super.img does not match the update");
        }
    }

    private static long feed(InputStream in, MessageDigest a, MessageDigest b, long n, byte[] buf, Util.Cancel cancel) throws IOException {
        long left = n;
        while (left > 0) {
            int k = in.read(buf, 0, (int) Math.min(buf.length, left));
            if (k < 0) throw new EOFException("super.img is shorter than planned");
            a.update(buf, 0, k);
            if (b != null) b.update(buf, 0, k);
            left -= k;
            if (cancel != null) cancel.check();
        }
        return n;
    }
}
