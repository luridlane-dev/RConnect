package com.rconnect.android.notify

import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
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
            isRunning = false
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        isRunning = true
        if (job == null || job?.isActive != true) {
            job = scope.launch {
                while (isActive) {
                    val livePrefs = NotificationPrefs(applicationContext)
                    if (!livePrefs.enabled || !livePrefs.watch || !Notifier.canPost(applicationContext)) {
                        isRunning = false
                        ServiceCompat.stopForeground(this@WatchService, ServiceCompat.STOP_FOREGROUND_REMOVE)
                        stopSelf()
                        break
                    }
                    if (!AppVisibility.inFront && hasNetwork(applicationContext)) {
                        runCatching { ServerCheck.run(applicationContext) }
                    }
                    delay(INTERVAL_MS)
                }
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        isRunning = false
        scope.cancel()
        super.onDestroy()
    }

    companion object {
        private const val INTERVAL_MS = 30_000L

        @Volatile
        private var isRunning = false

        private fun hasNetwork(context: Context): Boolean = runCatching {
            val cm = context.getSystemService(ConnectivityManager::class.java) ?: return true
            val net = cm.activeNetwork ?: return false
            val caps = cm.getNetworkCapabilities(net) ?: return false
            caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
        }.getOrDefault(true)

        /** Starts or stops the service to match the settings. */
        fun sync(context: Context) {
            val prefs = NotificationPrefs(context)
            val intent = Intent(context, WatchService::class.java)
            if (prefs.enabled && prefs.watch && Notifier.canPost(context)) {
                if (!isRunning) {
                    runCatching { ContextCompat.startForegroundService(context, intent) }
                }
            } else if (isRunning) {
                isRunning = false
                runCatching { context.stopService(intent) }
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
                if (prefs.watch) {
                    CheckWorker.cancel(context)
                    WatchService.sync(context)
                } else {
                    CheckWorker.schedule(context)
                }
            }
        }
    }
}
