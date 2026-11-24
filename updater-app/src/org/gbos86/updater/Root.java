package org.gbos86.updater;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * Talks to the root service (sysupdate.sh request, started by init when this app sets sys.gbos86.sysupdate). The request goes in
 * files/request.txt (only this app and root can write there), the answer comes back in files/response.txt. One request at a time.
 * Actions: info, audit, status, stage, cancel, cleanup, reboot (docs/system-update.md, "The app").
 */
final class Root {
    private Root() {}

    static final String PROP = "sys.gbos86.sysupdate";
    static final String ALLOWED = "[A-Za-z0-9._+,@=~/ -]+";

    interface Listener { void running(String body); }

    static final class Result {
        final boolean ok;
        final String body;
        Result(boolean ok, String body) { this.ok = ok; this.body = body; }
    }

    static final class NoService extends Exception {
        NoService(String m) { super(m); }
    }

    /** extra: path, where (app|shared), prune_logs (1), deep (1). */
    static synchronized Result call(Context ctx, String action, Map<String, String> extra, Listener l) throws Exception {
        File dir = ctx.getFilesDir();
        File rq = new File(dir, "request.txt"), part = new File(dir, "request.txt.part"), rs = new File(dir, "response.txt");
        rs.delete();
        long id = System.currentTimeMillis() % 1000000000L;
        StringBuilder sb = new StringBuilder("gbos86-sysupdate-request 1\nid ").append(id).append("\naction ").append(action).append('\n');
        if (extra != null) for (Map.Entry<String, String> e : extra.entrySet()) {
            if (!e.getValue().matches(ALLOWED) || !e.getKey().matches("[a-z_]+")) throw new IllegalArgumentException("bad request value");
            sb.append(e.getKey()).append(' ').append(e.getValue()).append('\n');
        }
        try (FileOutputStream f = new FileOutputStream(part)) {
            f.write(sb.toString().getBytes(StandardCharsets.US_ASCII));
            f.getFD().sync();
        }
        if (!part.renameTo(rq)) throw new Exception("cannot write the request");
        int user = android.os.Process.myUid() / 100000;
        java.lang.Process p = Runtime.getRuntime().exec(new String[] {"/system/bin/setprop", PROP, user + "." + id});
        if (p.waitFor() != 0)
            throw new NoService("The system did not accept the request (setprop failed). The updater service needs gbOS86's boot hook "
                    + "and permissive SELinux (docs/system-update.md).");
        long start = System.currentTimeMillis(), lastChange = start;
        String last = "";
        while (true) {
            Thread.sleep(500);
            String r = readResponse(rs, id);
            long now = System.currentTimeMillis();
            if (r == null) {
                if (now - start > 20000)
                    throw new NoService("No answer from the updater service. It runs only on gbOS86 installed on the internal disk, "
                            + "with gbos86-updater.apk on GBOSSYS (then hook.sh starts it).");
                continue;
            }
            int nl = r.indexOf('\n');
            String state = nl < 0 ? r : r.substring(0, nl), body = nl < 0 ? "" : r.substring(nl + 1);
            if (state.equals("state done")) return new Result(true, body);
            if (state.equals("state failed")) return new Result(false, body);
            if (!body.equals(last)) { last = body; lastChange = now; if (l != null) l.running(body); }
            if (now - lastChange > 30 * 60 * 1000) throw new Exception("The updater service stopped answering. Last message:\n" + last);
        }
    }

    static Map<String, String> extra(String... kv) {
        Map<String, String> m = new HashMap<>();
        for (int i = 0; i + 1 < kv.length; i += 2) if (kv[i + 1] != null) m.put(kv[i], kv[i + 1]);
        return m;
    }

    /** "state ...\nbody" for the response to request id, or null. */
    private static String readResponse(File rs, long id) {
        if (!rs.exists()) return null;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(rs), StandardCharsets.UTF_8))) {
            if (!"gbos86-sysupdate-response 1".equals(r.readLine())) return null;
            if (!("id " + id).equals(r.readLine())) return null;
            String state = r.readLine();
            if (state == null || !state.startsWith("state ")) return null;
            StringBuilder sb = new StringBuilder(state).append('\n');
            String l;
            int n = 0;
            while ((l = r.readLine()) != null && n++ < 300) sb.append(l).append('\n');
            return sb.toString().trim();
        } catch (Exception e) {
            return null;
        }
    }

    /** key=value lines (later keys win); other lines are ignored. */
    static Map<String, String> keyValues(String body) {
        Map<String, String> m = new HashMap<>();
        for (String line : body.split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0 && line.substring(0, eq).matches("[a-z_]+")) m.put(line.substring(0, eq), line.substring(eq + 1).trim());
        }
        return m;
    }
}
