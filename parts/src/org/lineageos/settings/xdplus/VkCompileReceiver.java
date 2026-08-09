/*
 * GPD XD+ Vulkan shader-compile progress surface.
 *
 * The vkshim Vulkan HAL interposer publishes "<pkg>:<count>" on the momentary
 * sys.xdplus.vkcompile property while a slow pipeline compile is running (the
 * DDK 1.9 USC compiler can take minutes per shader on this GPU). init.xdplus.rc
 * relays each update here as a root explicit-component broadcast; this receiver
 * shows either an indeterminate-progress notification (updated in place, auto-
 * timing out 30 s after the last update — the shim cannot know when the last
 * batch is done) or a rate-limited toast, per persist.sys.xdplus.vknotify.
 */

package org.lineageos.settings.xdplus;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.text.TextUtils;
import android.widget.Toast;


public class VkCompileReceiver extends BroadcastReceiver {

    private static final String CHANNEL_ID = "xdplus_vkcompile";
    private static final int NOTIFICATION_ID = 0x786b;  // "xk"
    private static final long NOTIFICATION_TIMEOUT_MS = 30_000;
    private static final long TOAST_MIN_INTERVAL_MS = 20_000;

    private static long sLastToastAt;

    @Override
    public void onReceive(Context context, Intent intent) {
        final String pkg = intent.getStringExtra("pkg");
        final String count = intent.getStringExtra("count");
        final String mode = intent.getStringExtra("mode");
        if (TextUtils.isEmpty(pkg)) return;

        final String text = context.getString(
                R.string.xdplus_vkcompile_text, pkg, TextUtils.isEmpty(count) ? "0" : count);

        if ("toast".equals(mode)) {
            final long now = SystemClock.elapsedRealtime();
            if (now - sLastToastAt < TOAST_MIN_INTERVAL_MS) return;
            sLastToastAt = now;
            Toast.makeText(context, text, Toast.LENGTH_LONG).show();
            return;
        }

        final NotificationManager nm = context.getSystemService(NotificationManager.class);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL_ID,
                context.getString(R.string.xdplus_vkcompile_channel),
                NotificationManager.IMPORTANCE_LOW));
        nm.notify(NOTIFICATION_ID, new Notification.Builder(context, CHANNEL_ID)
                .setSmallIcon(com.android.internal.R.drawable.stat_sys_download)
                .setContentTitle(context.getString(R.string.xdplus_vkcompile_title))
                .setContentText(text)
                .setProgress(0, 0, true)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setTimeoutAfter(NOTIFICATION_TIMEOUT_MS)
                .build());
    }
}
