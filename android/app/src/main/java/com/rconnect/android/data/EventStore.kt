package com.rconnect.android.data

import android.content.Context
import com.rconnect.android.winrm.AccessEvent
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

data class StoredEvent(
    val serverId: String,
    val serverTitle: String,
    val event: AccessEvent,
)

/** Keeps recent server events on disk so the activity log loads immediately and grows across checks. */
class EventStore(context: Context) {
    private val file = File(context.filesDir, "activity_log.json")

    fun load(): List<StoredEvent> {
        if (!file.exists()) return emptyList()
        return runCatching {
            val arr = JSONArray(file.readText(Charsets.UTF_8))
            (0 until arr.length()).mapNotNull { i ->
                val o = arr.optJSONObject(i) ?: return@mapNotNull null
                val kind = runCatching { AccessEvent.Kind.valueOf(o.getString("kind")) }.getOrNull() ?: return@mapNotNull null
                StoredEvent(
                    serverId = o.optString("serverId"),
                    serverTitle = o.optString("serverTitle"),
                    event = AccessEvent(
                        kind = kind,
                        timeUtcMs = o.optLong("time"),
                        user = o.optString("user"),
                        address = o.optString("address"),
                        client = o.optString("client"),
                        detail = o.optString("detail"),
                        sessionId = o.optInt("sessionId"),
                        recordId = o.optLong("recordId"),
                    ),
                )
            }.sortedByDescending { it.event.timeUtcMs }
        }.getOrDefault(emptyList())
    }

    fun save(events: List<StoredEvent>) {
        runCatching {
            val sorted = if (isSortedDesc(events)) events else events.sortedByDescending { it.event.timeUtcMs }
            val trimmed = if (sorted.size > MAX_STORED) sorted.subList(0, MAX_STORED) else sorted
            val arr = JSONArray()
            for (item in trimmed) {
                val e = item.event
                arr.put(
                    JSONObject()
                        .put("serverId", item.serverId)
                        .put("serverTitle", item.serverTitle)
                        .put("kind", e.kind.name)
                        .put("time", e.timeUtcMs)
                        .put("user", e.user)
                        .put("address", e.address)
                        .put("client", e.client)
                        .put("detail", e.detail)
                        .put("sessionId", e.sessionId)
                        .put("recordId", e.recordId),
                )
            }
            val tmp = File(file.parentFile, "activity_log.json.tmp")
            tmp.writeText(arr.toString(), Charsets.UTF_8)
            tmp.renameTo(file)
        }
    }

    fun clear() {
        runCatching { file.delete() }
    }

    companion object {
        private const val MAX_STORED = 4000

        fun dedupeKey(serverId: String, e: AccessEvent): String =
            if (e.recordId > 0) "$serverId|${e.kind.name}|${e.recordId}"
            else "$serverId|${e.kind.name}|${e.timeUtcMs}|${e.user.lowercase(Locale.ROOT)}|${e.address}"

        fun merge(existing: List<StoredEvent>, serverId: String, serverTitle: String, incoming: List<AccessEvent>): List<StoredEvent> {
            val byKey = LinkedHashMap<String, StoredEvent>(existing.size + incoming.size)
            for (item in existing) {
                val updatedTitle = if (item.serverId == serverId) item.copy(serverTitle = serverTitle) else item
                byKey[dedupeKey(item.serverId, item.event)] = updatedTitle
            }
            for (e in incoming) {
                val k = dedupeKey(serverId, e)
                val prev = byKey[k]
                val mergedEvent = if (prev != null && e.client.isEmpty() && prev.event.client.isNotEmpty()) {
                    e.copy(client = prev.event.client)
                } else e
                byKey[k] = StoredEvent(serverId, serverTitle, mergedEvent)
            }
            return byKey.values.sortedByDescending { it.event.timeUtcMs }.take(MAX_STORED)
        }

        fun toCsv(
            items: List<StoredEvent>,
            contacts: List<Contact>,
            eventName: (AccessEvent.Kind) -> String,
            detailLabel: (AccessEvent) -> String,
        ): String {
            val fmt = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss", Locale.US)
            val zone = ZoneId.systemDefault()
            val contactIndex = ContactIndex(contacts)
            val sb = StringBuilder(items.size * 96 + 64)
            sb.append("Time,Server,Event,Person,Computer,User,IP Address,Details\r\n")
            for (item in items) {
                val e = item.event
                val time = fmt.format(Instant.ofEpochMilli(e.timeUtcMs).atZone(zone))
                val nick = contactIndex.nameFor(e.client, e.user).orEmpty()
                sb.append(csvCell(time)).append(',')
                    .append(csvCell(item.serverTitle)).append(',')
                    .append(csvCell(eventName(e.kind))).append(',')
                    .append(csvCell(nick)).append(',')
                    .append(csvCell(e.client)).append(',')
                    .append(csvCell(e.user)).append(',')
                    .append(csvCell(e.address)).append(',')
                    .append(csvCell(detailLabel(e))).append("\r\n")
            }
            return sb.toString()
        }

        private fun isSortedDesc(events: List<StoredEvent>): Boolean {
            for (i in 1 until events.size) {
                if (events[i].event.timeUtcMs > events[i - 1].event.timeUtcMs) return false
            }
            return true
        }

        private fun csvCell(raw: String): String {
            // Prevent spreadsheet formula execution when values come from remote logs.
            val needsPrefix = raw.isNotEmpty() && when (raw[0]) {
                '=', '+', '-', '@', '\t' -> true
                else -> false
            }
            val safe = if (needsPrefix) "'$raw" else raw
            return "\"" + safe.replace("\"", "\"\"") + "\""
        }
    }
}
