package org.gbos86.updater;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * The laptop started: Android's BOOT_COMPLETED (once the app was opened), or org.gbos86.updater.BOOT_CHECK sent by the gbOS86
 * boot service (sysupdate.sh install-app) on every start, which also reaches an app that was never opened. Schedules the update
 * check (CheckJob), which waits for the network. Sending BOOT_CHECK can only cause a harmless extra check.
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        String a = i.getAction();
        if (Intent.ACTION_BOOT_COMPLETED.equals(a) || "org.gbos86.updater.BOOT_CHECK".equals(a)) CheckJob.schedule(c, true);
        else if (Intent.ACTION_MY_PACKAGE_REPLACED.equals(a)) CheckJob.schedule(c, false);
    }
}
