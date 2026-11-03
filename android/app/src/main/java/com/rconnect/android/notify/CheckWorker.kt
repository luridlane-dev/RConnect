package com.rconnect.android.notify

import android.content.Context
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import com.rconnect.android.data.ContactStore
import com.rconnect.android.data.ServerStore
import com.rconnect.android.winrm.WinRmClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.util.concurrent.TimeUnit

/**
 * Checks every server while the app is closed and posts a notification for what changed. Android runs this at
 * most every 15 minutes, and later when the phone is saving battery.
 */
class CheckWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val prefs = NotificationPrefs(applicationContext)
        if (!prefs.enabled || AppVisibility.inFront || !Notifier.canPost(applicationContext)) return Result.success()
        val servers = ServerStore(applicationContext).load()
        if (servers.isEmpty()) return Result.success()
        val contacts = ContactStore(applicationContext).load()
        val detector = ChangeDetector(applicationContext)
        val parallel = Semaphore(4)
        coroutineScope {
            servers.map { server ->
                async(Dispatchers.IO) {
                    val snapshot = parallel.withPermit {
                        runCatching { WinRmClient(server.host.trim(), server.user.trim(), server.password, timeoutMs = 15000).snapshot(false) }
                    }
                    val changes = detector.detect(server.id, server.title, snapshot.isSuccess, snapshot.getOrNull()?.sessions, contacts)
                    // The app may have been opened meanwhile; it then shows the changes itself.
                    if (!AppVisibility.inFront) for (c in changes) if (prefs.wants(c)) Notifier.post(applicationContext, c)
                }
            }.awaitAll()
        }
        return Result.success()
    }

    companion object {
        private const val NAME = "background-check"

        fun schedule(context: Context) {
            val request = PeriodicWorkRequestBuilder<CheckWorker>(15, TimeUnit.MINUTES)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
                .build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(NAME, ExistingPeriodicWorkPolicy.UPDATE, request)
        }

        fun cancel(context: Context) {
            WorkManager.getInstance(context).cancelUniqueWork(NAME)
        }
    }
}
