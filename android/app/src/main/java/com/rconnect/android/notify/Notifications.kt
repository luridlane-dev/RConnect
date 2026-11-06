package com.rconnect.android.notify

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.rconnect.android.MainActivity
import com.rconnect.android.R
import com.rconnect.android.data.Contact
import com.rconnect.android.data.ContactBook
import com.rconnect.android.winrm.RemoteSession
import org.json.JSONArray
import org.json.JSONObject

/** What the person wants to be told about. Everything is off until they turn it on in the app. */
class NotificationPrefs(context: Context) {
    private val prefs = context.getSharedPreferences("notifications", Context.MODE_PRIVATE)

    var enabled: Boolean
        get() = prefs.getBoolean("enabled", false)
        set(v) = prefs.edit().putBoolean("enabled", v).apply()
    var onConnect: Boolean
        get() = prefs.getBoolean("connect", true)
        set(v) = prefs.edit().putBoolean("connect", v).apply()
    var onLeave: Boolean
        get() = prefs.getBoolean("leave", true)
        set(v) = prefs.edit().putBoolean("leave", v).apply()
    var onProblem: Boolean
        get() = prefs.getBoolean("problem", true)
        set(v) = prefs.edit().putBoolean("problem", v).apply()

    var watch: Boolean
        get() = prefs.getBoolean("watch", true)
        set(v) = prefs.edit().putBoolean("watch", v).apply()
    var onStranger: Boolean
        get() = prefs.getBoolean("stranger", true)
        set(v) = prefs.edit().putBoolean("stranger", v).apply()

    fun wants(change: Change) = enabled && when (change.kind) {
        Change.Kind.Connected -> onConnect
        Change.Kind.Stranger -> onStranger
        Change.Kind.Left -> onLeave
        Change.Kind.Problem, Change.Kind.Recovered -> onProblem
    }
}

/** Something that changed on a server since the last check. */
data class Change(val kind: Kind, val serverId: String, val server: String, val who: String = "") {
    enum class Kind { Connected, Stranger, Left, Problem, Recovered }
}

/**
 * Compares each answer with the previous one, which is kept on the phone so the app in front and the background
 * check agree on what is new. The first answer of a server never counts as a change.
 */
class ChangeDetector(context: Context) {
    private val prefs = context.getSharedPreferences("observed", Context.MODE_PRIVATE)

    /**
     * A session is told apart by its number and account only. Its sign-in time is read from the shell process and is
     * not steady (the shell restarts, or the server leaves the time out of one answer), which used to make the same
     * person look like one who left and another who connected. A session also has to be missing from [MISSES_BEFORE_LEFT]
     * answers in a row before it counts as gone, so one incomplete answer does not report anybody leaving.
     */
    @Synchronized
    fun detect(serverId: String, title: String, ok: Boolean, sessions: List<RemoteSession>?, contacts: List<Contact>): List<Change> {
        val previous = prefs.getString(serverId, null)?.let { runCatching { JSONObject(it) }.getOrNull() }
        val keys = sessions.orEmpty().filter { it.connected }.associateBy { key(it.id, it.user) }
        // Files written by an earlier version also had the sign-in time in the key.
        val oldRaw = previous?.optJSONArray("s")?.let { a -> (0 until a.length()).map { a.getString(it) } }.orEmpty()
        val oldKeys = oldRaw.map(::normalize).distinct()
        val oldClients = HashMap<String, String>()
        previous?.optJSONObject("c")?.let { o -> o.keys().forEach { oldClients[normalize(it)] = o.optString(it) } }
        val oldMisses = previous?.optJSONObject("m")

        // A failed check does not say who is connected: keep what was known.
        val kept = ArrayList<String>()
        val clients = JSONObject()
        val misses = JSONObject()
        val gone = ArrayList<String>()
        if (ok) {
            for ((k, s) in keys) { kept += k; clients.put(k, s.client) }
            for (k in oldKeys) if (k !in keys) {
                val n = (oldMisses?.optInt(k, 0) ?: 0) + 1
                if (n >= MISSES_BEFORE_LEFT) { gone += k; continue }
                kept += k; misses.put(k, n); clients.put(k, oldClients[k].orEmpty())
            }
        } else {
            for (k in oldKeys) { kept += k; clients.put(k, oldClients[k].orEmpty()); oldMisses?.optInt(k, 0)?.let { misses.put(k, it) } }
        }
        val stored = JSONObject().put("ok", ok).put("s", JSONArray(kept)).put("c", clients).put("m", misses)
        prefs.edit().putString(serverId, stored.toString()).apply()
        if (previous == null) return emptyList()

        val changes = ArrayList<Change>()
        val wasOk = previous.optBoolean("ok", true)
        when {
            wasOk && !ok -> changes += Change(Change.Kind.Problem, serverId, title)
            !wasOk && ok -> changes += Change(Change.Kind.Recovered, serverId, title)
        }
        if (ok && wasOk) {
            for ((k, s) in keys) if (k !in oldKeys) {
                // Someone whose computer is in nobody's address book: worth a louder message. With an empty address
                // book everyone would be a stranger, so it stays quiet then.
                val stranger = contacts.isNotEmpty() && s.client.isNotBlank() && ContactBook.nameFor(contacts, s.client, s.user) == null
                changes += Change(if (stranger) Change.Kind.Stranger else Change.Kind.Connected, serverId, title, who(s, contacts))
            }
            for (k in gone) changes += Change(Change.Kind.Left, serverId, title, whoLeft(k, oldClients[k].orEmpty(), contacts))
        }
        return changes
    }

    private fun key(id: Int, user: String) = "$id|${user.lowercase()}"

    // "id|user|signInTime" of an earlier version becomes "id|user".
    private fun normalize(key: String) = key.split('|').let { if (it.size > 2) it[0] + "|" + it[1].lowercase() else key }

    fun forget(serverId: String) = prefs.edit().remove(serverId).apply()

    private companion object {
        const val MISSES_BEFORE_LEFT = 2
    }

    private fun who(s: RemoteSession, contacts: List<Contact>) =
        ContactBook.nameFor(contacts, s.client, s.user) ?: s.client.ifBlank { s.user.substringAfter('\\') }

    // A session that left: the computer it was connected from, as remembered; the account only if that was never known.
    private fun whoLeft(key: String, client: String, contacts: List<Contact>): String {
        val user = key.split('|').getOrElse(1) { "" }
        return ContactBook.nameFor(contacts, client, user) ?: client.ifBlank { user.substringAfter('\\') }
    }
}

/** Process-wide flag: the background check stays quiet while the app is on screen, which shows changes itself. */
object AppVisibility {
    @Volatile
    var inFront = false
}

object Notifier {
    private const val CHANNEL_ACTIVITY = "activity"
    private const val CHANNEL_PROBLEMS = "problems"
    private const val CHANNEL_WATCH = "watch"
    const val WATCH_ID = 7
    const val EXTRA_SERVER = "server"

    fun canPost(context: Context): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED

    fun text(context: Context, c: Change): String = when (c.kind) {
        Change.Kind.Connected -> context.getString(R.string.note_connected, c.who)
        Change.Kind.Stranger -> context.getString(R.string.note_stranger, c.who)
        Change.Kind.Left -> context.getString(R.string.note_left, c.who)
        Change.Kind.Problem -> context.getString(R.string.note_problem)
        Change.Kind.Recovered -> context.getString(R.string.note_recovered)
    }

    fun post(context: Context, c: Change) {
        if (!canPost(context)) return
        createChannels(context)
        val open = PendingIntent.getActivity(
            context, c.serverId.hashCode(),
            Intent(context, MainActivity::class.java).putExtra(EXTRA_SERVER, c.serverId).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val problem = c.kind == Change.Kind.Problem || c.kind == Change.Kind.Stranger
        val n = NotificationCompat.Builder(context, if (problem) CHANNEL_PROBLEMS else CHANNEL_ACTIVITY)
            .setSmallIcon(R.drawable.ic_stat_rconnect)
            .setContentTitle(c.server)
            .setContentText(text(context, c))
            .setContentIntent(open)
            .setAutoCancel(true)
            .setCategory(if (problem) NotificationCompat.CATEGORY_ERROR else NotificationCompat.CATEGORY_STATUS)
            .setPriority(if (problem) NotificationCompat.PRIORITY_HIGH else NotificationCompat.PRIORITY_DEFAULT)
            .setColor(0xFF4A86C8.toInt())
            .build()
        // One notification per server and kind: a flapping server replaces its own message instead of piling up.
        try {
            NotificationManagerCompat.from(context).notify("${c.serverId}|${c.kind}|${c.who}", 1, n)
        } catch (e: SecurityException) {
            // permission withdrawn meanwhile
        }
    }

    /** The permanent notification of [WatchService]. */
    fun watching(context: Context): android.app.Notification {
        createChannels(context)
        val open = PendingIntent.getActivity(
            context, 0, Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        return NotificationCompat.Builder(context, CHANNEL_WATCH)
            .setSmallIcon(R.drawable.ic_stat_rconnect)
            .setContentTitle(context.getString(R.string.watch_title))
            .setContentText(context.getString(R.string.watch_text))
            .setContentIntent(open)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .build()
    }

    private fun createChannels(context: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ACTIVITY, context.getString(R.string.channel_activity), NotificationManager.IMPORTANCE_DEFAULT))
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_PROBLEMS, context.getString(R.string.channel_problems), NotificationManager.IMPORTANCE_HIGH))
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_WATCH, context.getString(R.string.channel_watch), NotificationManager.IMPORTANCE_MIN))
    }
}
