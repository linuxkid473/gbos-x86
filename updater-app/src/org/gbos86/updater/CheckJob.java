package org.gbos86.updater;

import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;

import org.gbos86.updater.core.Feed;

/**
 * The automatic update check: once after the laptop starts (BootReceiver) and, if wanted, once a day. Waits for a network, reads
 * the signed feed, and shows a notification when an update newer than the installed one (and not announced before) is published.
 * It never downloads the update itself.
 */
public class CheckJob extends JobService {
    static final int JOB_BOOT = 1, JOB_DAILY = 2, JOB_CLEANUP = 3;
    private volatile Thread worker;

    static void schedule(Context c, boolean atBoot) {
        JobScheduler js = c.getSystemService(JobScheduler.class);
        ComponentName cn = new ComponentName(c, CheckJob.class);
        // After every start, without a network: delete an installed update's bundle folders (Updater.cleanupAfterCommit).
        if (atBoot) js.schedule(new JobInfo.Builder(JOB_CLEANUP, cn).setMinimumLatency(20000).setOverrideDeadline(10 * 60 * 1000L).build());
        if (atBoot && Prefs.checkAtBoot(c)) {
            js.schedule(new JobInfo.Builder(JOB_BOOT, cn).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                    .setMinimumLatency(30000).setOverrideDeadline(6 * 3600 * 1000L).build());
        }
        if (Prefs.checkDaily(c)) {
            if (js.getPendingJob(JOB_DAILY) == null)
                js.schedule(new JobInfo.Builder(JOB_DAILY, cn).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                        .setPeriodic(24 * 3600 * 1000L).setPersisted(true).build());
        } else {
            js.cancel(JOB_DAILY);
        }
        if (!Prefs.checkAtBoot(c)) js.cancel(JOB_BOOT);
    }

    @Override
    public boolean onStartJob(JobParameters params) {
        worker = new Thread(() -> {
            if (params.getJobId() == JOB_CLEANUP) {
                try {
                    String r = Updater.cleanupAfterCommit(this);
                    if (r != null) Prefs.get(this).edit().putString("last_cleanup", r).apply();
                } catch (Exception e) {
                    Prefs.get(this).edit().putString("last_cleanup", "Cleanup failed: " + e.getMessage()).apply();
                }
                jobFinished(params, false);
                return;
            }
            try {
                check(this);
            } catch (Exception e) {
                Prefs.get(this).edit().putString("last_check", "Automatic check failed: " + e.getMessage()).apply();
            }
            jobFinished(params, false);
        });
        worker.start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        Thread w = worker;
        if (w != null) w.interrupt();
        return true;
    }

    static void check(Context c) throws Exception {
        if (UpdateService.State.running) return;
        Updater.Info i = Updater.info(c);
        Feed f = Updater.fetchFeed(c, i);
        Updater.Decision d = Updater.decide(c, i, null, f);
        Prefs.get(c).edit().putString("last_check", d.message).putLong("last_check_time", System.currentTimeMillis()).apply();
        long told = Prefs.get(c).getLong("notified_seq", 0);
        if ((d.kind == Updater.Kind.AVAILABLE || d.kind == Updater.Kind.BLOCKED) && f.seq > told) {
            String text = d.message + (f.notes().isEmpty() ? "" : "\n\nWhat's new: " + String.join("; ", f.notes()));
            Notifier.show(c, Notifier.ID_AVAILABLE, Notifier.CH_UPDATES, "gbOS86 update " + f.seq + " available", text);
            Prefs.get(c).edit().putLong("notified_seq", f.seq).apply();
        }
    }
}
