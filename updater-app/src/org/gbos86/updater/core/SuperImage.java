package org.gbos86.updater.core;

import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/**
 * Java port of usb/make_super.py (minimal lpmake, liblp metadata v10.0, 1 MiB aligned, 2 slots). The image is fully determined by
 * the partition names and sizes, so the app rebuilds byte for byte the super.img the maintainer published (tools/super_parts.py
 * checks that before a feed offers it; tools/test_updater_core.sh compares this class with make_super.py).
 */
public final class SuperImage {
    static final int SECTOR = 512, ALIGN = 1 << 20, BLOCK = 4096, RESERVED = 4096, GEOM_SIZE = 4096, MAX_META = 65536, SLOTS = 2;
    static final int GEOM_MAGIC = 0x616c4467, HDR_MAGIC = 0x414C5030;
    static final String GROUP = "google_dynamic_partitions_a";

    public static final class Placed {
        public final String name;
        public final long size, offset, alignedSize;
        Placed(String name, long size, long offset, long alignedSize) { this.name = name; this.size = size; this.offset = offset; this.alignedSize = alignedSize; }
    }

    public final List<Placed> parts = new ArrayList<>();
    public long total;
    byte[] geom, meta;

    static long up(long n, long a) { return (n + a - 1) / a * a; }

    private static byte[] name36(String s) {
        byte[] b = s.getBytes(StandardCharsets.US_ASCII);
        if (b.length >= 36) throw new IllegalArgumentException("partition name too long: " + s);
        byte[] out = new byte[36];
        System.arraycopy(b, 0, out, 0, b.length);
        return out;
    }

    private static ByteBuffer le(int n) { return ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN); }

    public static SuperImage plan(String[] names, long[] sizes) {
        SuperImage s = new SuperImage();
        long metaEnd = RESERVED + (long) (GEOM_SIZE + MAX_META * SLOTS) * 2;
        long firstLogical = up(metaEnd, ALIGN), cur = firstLogical;
        for (int i = 0; i < names.length; i++) {
            long al = up(sizes[i], BLOCK);
            s.parts.add(new Placed(names[i], sizes[i], cur, al));
            cur = up(cur + al, ALIGN);
        }
        s.total = up(cur + ALIGN, ALIGN);

        int n = s.parts.size();
        ByteBuffer ptab = le(52 * n), etab = le(24 * n), gtab = le(96), btab = le(64);
        for (int i = 0; i < n; i++) {
            ptab.put(name36(s.parts.get(i).name)).putInt(1).putInt(i).putInt(1).putInt(1);
            Placed p = s.parts.get(i);
            etab.putLong(p.alignedSize / SECTOR).putInt(0).putLong(p.offset / SECTOR).putInt(0);
        }
        gtab.put(name36("default")).putInt(0).putLong(0).put(name36(GROUP)).putInt(0).putLong(0);
        btab.putLong(firstLogical / SECTOR).putInt(ALIGN).putInt(0).putLong(s.total).put(name36("super")).putInt(0);
        byte[] tables = new byte[ptab.capacity() + etab.capacity() + 96 + 64];
        ByteBuffer.wrap(tables).put(ptab.array()).put(etab.array()).put(gtab.array()).put(btab.array());

        int[] descs = new int[12];
        int o = 0, k = 0;
        int[][] t = {{ptab.capacity(), 52}, {etab.capacity(), 24}, {96, 48}, {64, 64}};
        for (int[] x : t) { descs[k++] = o; descs[k++] = x[0] / x[1]; descs[k++] = x[1]; o += x[0]; }
        MessageDigest md = Util.sha256();
        byte[] tsum = md.digest(tables);
        byte[] h = header(new byte[32], tables.length, tsum, descs);
        h = header(Util.sha256().digest(h), tables.length, tsum, descs);
        s.meta = new byte[h.length + tables.length];
        System.arraycopy(h, 0, s.meta, 0, h.length);
        System.arraycopy(tables, 0, s.meta, h.length, tables.length);
        if (s.meta.length > MAX_META) throw new IllegalArgumentException("metadata too big");

        byte[] g = geom(new byte[32]);
        s.geom = geom(Util.sha256().digest(g));
        return s;
    }

    private static byte[] header(byte[] ck, int tablesLen, byte[] tsum, int[] descs) {
        ByteBuffer b = le(128);
        b.putInt(HDR_MAGIC).putShort((short) 10).putShort((short) 0).putInt(128).put(ck).putInt(tablesLen).put(tsum);
        for (int d : descs) b.putInt(d);
        return b.array();
    }

    private static byte[] geom(byte[] ck) {
        ByteBuffer b = le(52);
        b.putInt(GEOM_MAGIC).putInt(52).put(ck).putInt(MAX_META).putInt(SLOTS).putInt(BLOCK);
        return b.array();
    }

    /** Writes the geometry (twice) and the metadata (primary and backup slots). The rest of the image is left as it is (zeros). */
    public void writeMetadata(RandomAccessFile f) throws IOException {
        for (long g : new long[] {RESERVED, RESERVED + GEOM_SIZE}) { f.seek(g); f.write(geom); }
        long base = RESERVED + GEOM_SIZE * 2L;
        for (int i = 0; i < SLOTS * 2; i++) { f.seek(base + (long) i * MAX_META); f.write(meta); }
    }

    public Placed part(String name) {
        for (Placed p : parts) if (p.name.equals(name)) return p;
        return null;
    }
}
