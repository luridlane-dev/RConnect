package com.rconnect.android.notify

import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Keeps checking the servers every half minute while the app is closed. Android only allows that with a permanent
 * notification, which is why it is its own option next to the notifications.
 */
class WatchService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val prefs = NotificationPrefs(this)
        if (!prefs.enabled || !prefs.watch || !Notifier.canPost(this)) {
            stopSelf()
            return START_NOT_STICKY
        }
        ServiceCompat.startForeground(this, Notifier.WATCH_ID, Notifier.watching(this), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        if (job == null) {
            job = scope.launch {
                while (isActive) {
                    runCatching { ServerCheck.run(applicationContext) }
                    delay(INTERVAL_MS)
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        private const val INTERVAL_MS = 30_000L

        /** Starts or stops the service to match the settings. */
        fun sync(context: Context) {
            val prefs = NotificationPrefs(context)
            val intent = Intent(context, WatchService::class.java)
            if (prefs.enabled && prefs.watch && Notifier.canPost(context)) {
                runCatching { ContextCompat.startForegroundService(context, intent) }
            } else {
                context.stopService(intent)
            }
        }
    }
}

/** Starts the watching again after the phone has restarted. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED) WatchService.sync(context)
    }
}
