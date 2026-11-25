package com.rconnect.android.notify

import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
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
        // Enter foreground before deciding whether to stop, as required after startForegroundService().
        val fgsType = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
        } else {
            0
        }
        try {
            ServiceCompat.startForeground(this, Notifier.WATCH_ID, Notifier.watching(this), fgsType)
        } catch (_: Exception) {
            stopSelf()
            return START_NOT_STICKY
        }

        val prefs = NotificationPrefs(this)
        if (!prefs.enabled || !prefs.watch || !Notifier.canPost(this)) {
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        if (job == null || job?.isActive != true) {
            job = scope.launch {
                while (isActive) {
                    val livePrefs = NotificationPrefs(applicationContext)
                    if (!livePrefs.enabled || !livePrefs.watch || !Notifier.canPost(applicationContext)) {
                        ServiceCompat.stopForeground(this@WatchService, ServiceCompat.STOP_FOREGROUND_REMOVE)
                        stopSelf()
                        break
                    }
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
                runCatching { context.stopService(intent) }
            }
        }
    }
}

/** Restores background checks after the phone restarts. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED) {
            val prefs = NotificationPrefs(context)
            if (prefs.enabled) {
                CheckWorker.schedule(context)
                WatchService.sync(context)
            }
        }
    }
}
