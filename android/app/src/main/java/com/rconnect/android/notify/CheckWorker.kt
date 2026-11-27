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
import com.rconnect.android.data.EventStore
import com.rconnect.android.data.ServerStore
import com.rconnect.android.winrm.WinRmClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import java.util.concurrent.TimeUnit

/**
 * Checks every server while the app is closed and posts a notification for what changed. Android runs this at
 * most every 15 minutes, and later when the phone is saving battery; [WatchService] checks far more often.
 */
class CheckWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        ServerCheck.run(applicationContext)
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

/** One round over every server: who is connected now compared with the last round, and a notification for each change. */
object ServerCheck {
    private val running = Mutex()

    suspend fun run(context: Context) {
        if (!running.tryLock()) return
        try {
            val prefs = NotificationPrefs(context)
            if (!prefs.enabled || AppVisibility.inFront || !Notifier.canPost(context)) return
            val servers = ServerStore(context).load()
            if (servers.isEmpty()) return
            val contacts = ContactStore(context).load()
            val detector = ChangeDetector(context)
            val eventStore = EventStore(context)
            val eventStoreLock = Mutex()
            val parallel = Semaphore(4)
            val now = System.currentTimeMillis()
            coroutineScope {
                servers.map { server ->
                    async(Dispatchers.IO) {
                        val checkSecurity = prefs.onSecurityChange && detector.shouldCheckSecurity(server.id, now)
                        val snapshot = parallel.withPermit {
                            runCatching {
                                WinRmClient(
                                    host = server.host.trim(),
                                    user = server.user.trim(),
                                    password = server.password,
                                    timeoutMs = 15000,
                                    rdpPort = server.port,
                                ).snapshot(
                                    withEvents = false,
                                    includeTimes = false,
                                    securityLookbackMs = if (checkSecurity) ChangeDetector.SECURITY_LOOKBACK_MS else null,
                                )
                            }
                        }
                        val snap = snapshot.getOrNull()
                        val changes = ArrayList<Change>()
                        changes += detector.detect(server.id, server.title, snap != null, snap?.sessions, contacts)
                        if (snap != null && checkSecurity) {
                            detector.markSecurityChecked(server.id, System.currentTimeMillis())
                            val secEvents = snap.events.filter { it.isSecurity }
                            changes += detector.detectSecurityChanges(context, server.id, server.title, secEvents)
                            if (secEvents.isNotEmpty()) {
                                eventStoreLock.withLock {
                                    val merged = EventStore.merge(eventStore.load(), server.id, server.title, secEvents)
                                    eventStore.save(merged)
                                }
                            }
                        }
                        // The app may have been opened meanwhile; it then reports the changes itself.
                        if (!AppVisibility.inFront) for (c in changes) if (prefs.wants(c)) Notifier.post(context, c)
                    }
                }.awaitAll()
            }
        } finally {
            running.unlock()
        }
    }
}
