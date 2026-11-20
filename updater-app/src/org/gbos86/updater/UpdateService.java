package org.gbos86.updater;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

import org.gbos86.updater.core.Util;

/**
 * Downloads and stages an update in the foreground (with a progress notification), so it carries on when the app's window is
 * closed. The screen reads State. Started only by a tap on "Download and install".
 */
public class UpdateService extends Service {
    /** What the screen shows. Written by the worker thread only. */
    static final class State {
        static volatile boolean running, done, failed;
        static volatile String phase = "", message = "";
        static volatile long progress, total;
        static volatile Updater.Decision decision;
        static volatile Util.Cancel cancel;
    }

    static final String STOP = "org.gbos86.updater.STOP";

    static void start(Context c, Updater.Decision d) {
        if (State.running) return;
        State.decision = d;
        State.done = false;
        State.failed = false;
        State.message = "";
        State.phase = "Starting ...";
        State.progress = 0;
        State.total = -1;
        State.cancel = new Util.Cancel();
        State.running = true;
        c.startForegroundService(new Intent(c, UpdateService.class));
    }

    static void stop() {
        Util.Cancel c = State.cancel;
        if (c != null) c.cancel();
    }

    private long lastNote;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && STOP.equals(intent.getAction())) { stop(); return START_NOT_STICKY; }
        Notifier.channels(this);
        startForeground(Notifier.ID_PROGRESS, progressNote("Starting ...", 0, -1), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        final Updater.Decision d = State.decision;
        if (d == null) { State.running = false; stopSelf(); return START_NOT_STICKY; }
        new Thread(() -> {
            try {
                String r = Updater.install(this, d, (ph, done, total) -> {
                    State.phase = ph;
                    State.progress = done;
                    State.total = total;
                    long now = System.currentTimeMillis();
                    if (now - lastNote > 2000) {
                        lastNote = now;
                        getSystemService(NotificationManager.class).notify(Notifier.ID_PROGRESS, progressNote(ph, done, total));
                    }
                }, State.cancel);
                State.message = r;
                State.done = true;
                Notifier.show(this, Notifier.ID_DONE, Notifier.CH_UPDATES, "gbOS86 update " + d.feed.seq + " is ready",
                        "Restart the laptop to install it. It is checked again before anything changes.");
            } catch (Util.Cancelled e) {
                State.message = "Stopped. Tap \"Download and install\" again to continue (the download resumes"
                        + (d.needsSuper ? "; Google's file starts over" : "") + ").";
                State.failed = true;
            } catch (Exception e) {
                State.message = "Failed: " + e.getMessage();
                State.failed = true;
                Notifier.show(this, Notifier.ID_DONE, Notifier.CH_UPDATES, "gbOS86 update " + d.feed.seq + " failed", String.valueOf(e.getMessage()));
            } finally {
                State.running = false;
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf();
            }
        }).start();
        return START_NOT_STICKY;
    }

    /** Android 15 stops dataSync services after 6 hours a day. */
    @Override
    public void onTimeout(int startId, int fgsType) {
        stop();
    }

    private Notification progressNote(String text, long done, long total) {
        Intent stop = new Intent(this, UpdateService.class).setAction(STOP);
        PendingIntent ps = PendingIntent.getService(this, 1, stop, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification.Builder b = new Notification.Builder(this, Notifier.CH_PROGRESS)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("gbOS86 update")
                .setContentText(text + (total > 0 ? "  " + Util.size(done) + " of " + Util.size(total) : ""))
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(Notifier.openApp(this))
                .addAction(new Notification.Action.Builder(null, "Stop", ps).build());
        if (total > 0) b.setProgress(1000, (int) (done * 1000 / total), false);
        else b.setProgress(0, 0, true);
        return b.build();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
