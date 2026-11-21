package org.gbos86.updater;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;

/** The two notification channels: "Updates available" (the check at startup / daily) and "Download progress" (the download service). */
final class Notifier {
    private Notifier() {}

    static final String CH_UPDATES = "updates", CH_PROGRESS = "progress";
    static final int ID_AVAILABLE = 1, ID_PROGRESS = 2, ID_DONE = 3;

    static void channels(Context c) {
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        NotificationChannel a = new NotificationChannel(CH_UPDATES, "Updates available", NotificationManager.IMPORTANCE_DEFAULT);
        a.setDescription("A new gbOS86 system update was published");
        NotificationChannel b = new NotificationChannel(CH_PROGRESS, "Download progress", NotificationManager.IMPORTANCE_LOW);
        b.setDescription("While an update downloads and is copied to the system partition");
        nm.createNotificationChannel(a);
        nm.createNotificationChannel(b);
    }

    static PendingIntent openApp(Context c) {
        Intent i = new Intent(c, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        return PendingIntent.getActivity(c, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    static void cancelAvailable(Context c) {
        c.getSystemService(NotificationManager.class).cancel(ID_AVAILABLE);
    }

    static void show(Context c, int id, String channel, String title, String text) {
        channels(c);
        Notification n = new Notification.Builder(c, channel)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setContentIntent(openApp(c))
                .setAutoCancel(true)
                .build();
        try {
            c.getSystemService(NotificationManager.class).notify(id, n);
        } catch (SecurityException e) {
            // notifications not allowed: the app shows the same on its screen
        }
    }
}
