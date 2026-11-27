package com.rconnect.android.winrm

import java.time.Instant
import java.time.OffsetDateTime
import java.util.Locale

/** Reads what the commands in [WinRmClient] print. Pure code, no network. */
object Parsing {
    const val MARKER = "##RCONNECT##"
    const val EVENTS_MARKER = "##RCONNECT-EVENTS##"
    const val CONNECTIONS_MARKER = "##RCONNECT-CONNECTIONS##"
    const val LOG_MARKER = "##RCLOG:"
    const val START_MARKER = "##RCSTART##"

    private val space = Regex("\\s+")
    private val dateField = Regex("^\\d{1,4}[./-]\\d{1,2}[./-]\\d{1,4}$")
    private val digits = Regex("^\\d+$")
    private val idlePattern = Regex("^(?:(\\d{1,6})\\+)?(?:(\\d{1,6}):)?(\\d{1,6})$")

    /** Splits the output into events, recent connection events, and current sessions. */
    fun snapshot(output: String, withEvents: Boolean, fallbackServerNowMs: Long = 0L): ServerSnapshot {
        var rest = output
        var events = emptyList<AccessEvent>()
        if (withEvents) {
            val split = rest.indexOf(EVENTS_MARKER)
            if (split >= 0) {
                events = parseEventLogs(rest.substring(0, split))
                rest = rest.substring(split + EVENTS_MARKER.length).substringAfter('\n', "")
            }
        }

        var connectionEvents = emptyList<AccessEvent>()
        val connSplit = rest.indexOf(CONNECTIONS_MARKER)
        if (connSplit >= 0) {
            connectionEvents = parseEventLogs(rest.substring(0, connSplit))
            rest = rest.substring(connSplit + CONNECTIONS_MARKER.length).substringAfter('\n', "")
        }

        var serverNow = 0L
        val sessions = parseSessions(rest) { serverNow = it }
        if (serverNow <= 0L && fallbackServerNowMs > 0L) serverNow = fallbackServerNowMs

        val sorted = events.sortedByDescending { it.timeUtcMs }
        // The session log only knows the client's address; security reconnect/disconnect events also name the computer.
        val nameOf = HashMap<String, String>()
        for (e in sorted) if (e.client.isNotEmpty() && e.address.isNotEmpty()) nameOf.putIfAbsent(e.address, e.client)
        val named = sorted.filter { it.kind != AccessEvent.Kind.Logon }
            .map { if (it.client.isEmpty()) it.copy(client = nameOf[it.address].orEmpty()) else it }

        // Resolve exact (re)connect time per session from the latest LSM session event by recordId.
        val lsmSource = if (connectionEvents.isNotEmpty()) connectionEvents else named
        val withConnect = applyConnectionTimes(sessions, lsmSource, serverNow)
        return ServerSnapshot(withConnect, named, serverNow)
    }

    private fun applyConnectionTimes(
        sessions: List<RemoteSession>,
        events: List<AccessEvent>,
        serverNow: Long,
    ): List<RemoteSession> {
        val latest = HashMap<Int, AccessEvent>()
        for (event in events) {
            if (event.sessionId <= 0) continue
            if (event.kind != AccessEvent.Kind.SignIn &&
                event.kind != AccessEvent.Kind.SignOut &&
                event.kind != AccessEvent.Kind.Disconnect &&
                event.kind != AccessEvent.Kind.Reconnect
            ) continue
            val prev = latest[event.sessionId]
            if (prev == null || event.recordId > prev.recordId || (event.recordId == prev.recordId && event.timeUtcMs > prev.timeUtcMs)) {
                latest[event.sessionId] = event
            }
        }
        return sessions.map { s ->
            val event = latest[s.id]
            if (!s.connected || event == null ||
                (event.kind != AccessEvent.Kind.SignIn && event.kind != AccessEvent.Kind.Reconnect)
            ) return@map s
            val eventUser = if (s.user.contains('\\')) event.user else event.user.substringAfterLast('\\')
            if (eventUser.isEmpty() || !s.user.equals(eventUser, ignoreCase = true)) return@map s
            if (serverNow > 0 && event.timeUtcMs > serverNow + 1000L) return@map s
            s.copy(
                connectedUtcMs = event.timeUtcMs,
                logonUtcMs = if (event.kind == AccessEvent.Kind.SignIn) event.timeUtcMs else s.logonUtcMs,
            )
        }
    }

    // `quser` columns: USERNAME [SESSIONNAME] ID STATE IDLE LOGON-DATE LOGON-TIME. Headers and the state are
    // localized, so only the layout is used: disconnected sessions have no session name.
    fun parseSessions(output: String, onServerNow: (Long) -> Unit = {}): List<RemoteSession> {
        val marker = output.indexOf(MARKER)
        var users = if (marker < 0) output else output.substring(0, marker)
        val clients = if (marker < 0) "" else output.substring(marker + MARKER.length)
        var starts = ""
        val startMarker = users.indexOf(START_MARKER)
        if (startMarker >= 0) {
            starts = users.substring(startMarker + START_MARKER.length)
            users = users.substring(0, startMarker)
        }

        val sessions = ArrayList<RemoteSession>()
        val lines = users.split('\n')
        for (i in 1 until lines.size) { // line 0 is the header
            val t = lines[i].trim().split(space).filter { it.isNotEmpty() }
            if (t.size < 4) continue
            val connected = !digits.matches(t[1])
            val idIndex = if (connected) 2 else 1
            if (idIndex >= t.size || !digits.matches(t[idIndex])) continue
            var dateIndex = -1
            var k = idIndex + 2
            while (k < t.size && dateIndex < 0) {
                if (dateField.matches(t[k])) dateIndex = k
                k++
            }
            val idle = if (dateIndex > idIndex + 1) parseIdle(t[dateIndex - 1]) else -1
            sessions += RemoteSession(
                user = t[0].removePrefix(">"),
                client = "",
                id = t[idIndex].toInt(),
                connected = connected,
                idleMinutes = idle,
                logonUtcMs = 0,
            )
        }

        // "NOW <time>" is the server's UTC clock.
        for (raw in starts.split('\n')) {
            val parts = raw.trim().split(' ').filter { it.isNotEmpty() }
            if (parts.size != 2 || parts[0] != "NOW") continue
            val time = parseTime(parts[1]) ?: continue
            onServerNow(time)
        }

        // HKEY_USERS\<sid>\Volatile Environment\<session id>
        //     CLIENTNAME    REG_SZ    PC-NAME
        val clientOf = HashMap<Int, String>()
        var sessionId = -1
        for (raw in clients.split('\n')) {
            val line = raw.trim()
            if (line.startsWith("HKEY_USERS\\", ignoreCase = true)) {
                sessionId = line.substringAfterLast('\\').toIntOrNull() ?: -1
                continue
            }
            val type = line.indexOf("REG_SZ")
            if (sessionId < 0 || !line.startsWith("CLIENTNAME", ignoreCase = true) || type < 0) continue
            clientOf[sessionId] = line.substring(type + 6).trim()
        }
        return sessions.map { it.copy(client = clientOf[it.id].orEmpty()) }
    }

    // ".", "none", "yok": under a minute; "5"; "1:05"; "2+03:04" (days+hours:minutes); anything else -> -1 (unknown).
    fun parseIdle(text: String): Int {
        val clean = text.trim()
        if (clean == "." || clean.equals("none", ignoreCase = true) || clean.equals("yok", ignoreCase = true)) return 0
        val m = idlePattern.matchEntire(clean) ?: return -1
        val days = m.groupValues[1].toLongOrNull() ?: 0L
        val hours = m.groupValues[2].toLongOrNull() ?: 0L
        val mins = m.groupValues[3].toLongOrNull() ?: 0L
        return (days * 1440L + hours * 60L + mins).coerceIn(0L, Int.MAX_VALUE.toLong()).toInt()
    }

    // 2026-10-31T08:15:30.1234567Z, with or without fraction digits or timezone offset.
    fun parseTime(text: String): Long? {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) return null
        return try {
            Instant.parse(trimmed).toEpochMilli()
        } catch (_: Exception) {
            try {
                OffsetDateTime.parse(trimmed).toInstant().toEpochMilli()
            } catch (_: Exception) {
                null
            }
        }
    }

    /** "##RCLOG:<source>##" followed by that log's wevtutil XML; each event is read on its own. */
    fun parseEventLogs(output: String): List<AccessEvent> {
        val events = ArrayList<AccessEvent>()
        var pos = output.indexOf(LOG_MARKER)
        while (pos >= 0) {
            val next = output.indexOf(LOG_MARKER, pos + LOG_MARKER.length)
            val start = output.indexOf('\n', pos)
            if (start >= 0 && (next < 0 || start < next))
                events += parseEvents(output.substring(start, if (next < 0) output.length else next))
            pos = next
        }
        return events
    }

    private val eventBlock = Regex("<Event[ >].*?</Event>", RegexOption.DOT_MATCHES_ALL)
    private val eventId = Regex("<EventID[^>]*>(\\d+)</EventID>")
    private val timeCreated = Regex("SystemTime=['\"]([^'\"]+)['\"]")
    private val recordId = Regex("<EventRecordID>(\\d+)</EventRecordID>")
    private val dataItem = Regex("<Data Name=['\"]([^'\"]*)['\"]>(.*?)</Data>", RegexOption.DOT_MATCHES_ALL)
    private val wsCollapse = Regex("\\s+")

    private fun taskCommand(taskXml: String): String {
        if (taskXml.isEmpty()) return ""
        val cmd = field(taskXml, "Command")
        val args = field(taskXml, "Arguments")
        return listOf(cmd, args).filter { it.isNotEmpty() }.joinToString(" ").replace(wsCollapse, " ").trim().take(240)
    }

    private fun field(xml: String, name: String): String {
        val open = "<$name>"
        val start = xml.indexOf(open)
        if (start < 0) return ""
        val contentStart = start + open.length
        val end = xml.indexOf("</$name>", contentStart)
        if (end < 0) return ""
        return unescape(xml.substring(contentStart, end)).trim()
    }

    private fun sanitizeXml(text: String): String {
        var needsCopy = false
        for (i in text.indices) {
            val c = text[i]
            if ((c.code < 0x20 && c != '\t' && c != '\n' && c != '\r') || c.code == 0xFFFE || c.code == 0xFFFF) {
                needsCopy = true
                break
            }
        }
        if (!needsCopy) return text
        return buildString(text.length) {
            for (c in text) append(if ((c.code < 0x20 && c != '\t' && c != '\n' && c != '\r') || c.code == 0xFFFE || c.code == 0xFFFF) ' ' else c)
        }
    }

    fun parseEvents(text: String): List<AccessEvent> {
        // Characters XML forbids (and bytes that were not text) must not hide the other events.
        val clean = sanitizeXml(text)
        val out = ArrayList<AccessEvent>()
        for (m in eventBlock.findAll(clean)) {
            val xml = m.value
            val id = eventId.find(xml)?.groupValues?.get(1)?.toIntOrNull() ?: continue
            val time = timeCreated.find(xml)?.groupValues?.get(1)?.let(::parseTime) ?: continue
            val record = recordId.find(xml)?.groupValues?.get(1)?.toLongOrNull() ?: 0
            if (id == 21 || id == 23 || id == 24 || id == 25) {
                out += AccessEvent(
                    kind = when (id) {
                        21 -> AccessEvent.Kind.SignIn
                        23 -> AccessEvent.Kind.SignOut
                        24 -> AccessEvent.Kind.Disconnect
                        else -> AccessEvent.Kind.Reconnect
                    },
                    timeUtcMs = time,
                    user = field(xml, "User"),
                    address = field(xml, "Address"),
                    client = "",
                    detail = "",
                    sessionId = field(xml, "SessionID").toIntOrNull() ?: 0,
                    recordId = record,
                )
                continue
            }
            val data = HashMap<String, String>()
            for (d in dataItem.findAll(xml)) data[d.groupValues[1]] = unescape(d.groupValues[2]).trim()
            out += when (id) {
                4778, 4779 -> AccessEvent(
                    kind = AccessEvent.Kind.Logon,
                    timeUtcMs = time,
                    user = account(data["AccountDomain"], data["AccountName"]),
                    address = dash(data["ClientAddress"]),
                    client = dash(data["ClientName"]),
                    detail = "",
                    sessionId = 0,
                    recordId = record,
                )
                4625 -> AccessEvent(
                    kind = AccessEvent.Kind.FailedSignIn,
                    timeUtcMs = time,
                    user = account(data["TargetDomainName"], data["TargetUserName"]),
                    address = dash(data["IpAddress"]),
                    client = dash(data["WorkstationName"]),
                    detail = failureKey(data["Status"].orEmpty(), data["SubStatus"].orEmpty()),
                    sessionId = 0,
                    recordId = record,
                )
                4720, 4722, 4723, 4724, 4725, 4726, 4740 -> AccessEvent(
                    kind = when (id) {
                        4720 -> AccessEvent.Kind.UserCreated
                        4722 -> AccessEvent.Kind.UserEnabled
                        4723 -> AccessEvent.Kind.PasswordChangeAttempt
                        4724 -> AccessEvent.Kind.PasswordResetAttempt
                        4725 -> AccessEvent.Kind.UserDisabled
                        4726 -> AccessEvent.Kind.UserDeleted
                        else -> AccessEvent.Kind.AccountLocked
                    },
                    timeUtcMs = time,
                    user = account(data["TargetDomainName"], data["TargetUserName"]),
                    address = "",
                    client = if (id == 4740) dash(data["CallerComputerName"]) else "",
                    detail = actor(data),
                    sessionId = 0,
                    recordId = record,
                )
                4732, 4733 -> AccessEvent(
                    kind = if (id == 4732) AccessEvent.Kind.GroupMemberAdded else AccessEvent.Kind.GroupMemberRemoved,
                    timeUtcMs = time,
                    user = dash(data["MemberName"]).ifEmpty { dash(data["MemberSid"]) },
                    address = "",
                    client = "",
                    detail = listOf(dash(data["TargetUserName"]), actor(data)).filter { it.isNotEmpty() }.joinToString(" · "),
                    sessionId = 0,
                    recordId = record,
                )
                4698, 4699, 4700, 4701, 4702 -> {
                    val taskName = dash(data["TaskName"])
                    val cmd = taskCommand(data["TaskContentNew"] ?: data["TaskContent"].orEmpty())
                    val detail = if (cmd.isNotEmpty() && taskName.isNotEmpty()) "$taskName: $cmd" else taskName.ifEmpty { cmd }
                    AccessEvent(
                        kind = when (id) {
                            4698 -> AccessEvent.Kind.TaskCreated
                            4699 -> AccessEvent.Kind.TaskRemoved
                            4700 -> AccessEvent.Kind.TaskEnabled
                            4701 -> AccessEvent.Kind.TaskDisabled
                            else -> AccessEvent.Kind.TaskUpdated
                        },
                        timeUtcMs = time,
                        user = actor(data),
                        address = "",
                        client = "",
                        detail = detail,
                        sessionId = 0,
                        recordId = record,
                    )
                }
                4719 -> {
                    val guid = dash(data["SubcategoryGuid"])
                    val changes = dash(data["AuditPolicyChanges"]).replace(wsCollapse, " ").trim()
                    val detail = listOf(guid, changes).filter { it.isNotEmpty() }.joinToString(" · ")
                    AccessEvent(
                        kind = AccessEvent.Kind.AuditPolicyChanged,
                        timeUtcMs = time,
                        user = actor(data),
                        address = "",
                        client = "",
                        detail = detail,
                        sessionId = 0,
                        recordId = record,
                    )
                }
                7045 -> AccessEvent(
                    kind = AccessEvent.Kind.ServiceInstalled,
                    timeUtcMs = time,
                    user = dash(data["AccountName"]),
                    address = "",
                    client = "",
                    detail = listOf(dash(data["ServiceName"]), dash(data["ImagePath"])).filter { it.isNotEmpty() }.joinToString(": "),
                    sessionId = 0,
                    recordId = record,
                )
                1102, 104 -> AccessEvent(
                    kind = AccessEvent.Kind.LogCleared,
                    timeUtcMs = time,
                    user = actor(data).ifEmpty { account(field(xml, "SubjectDomainName"), field(xml, "SubjectUserName")) },
                    address = "",
                    client = "",
                    detail = if (id == 1102) "Security" else field(xml, "Channel").ifEmpty { "System" },
                    sessionId = 0,
                    recordId = record,
                )
                else -> continue
            }
        }
        return out
    }

    private fun actor(data: Map<String, String>): String =
        account(data["SubjectDomainName"], data["SubjectUserName"])

    private fun account(domain: String?, user: String?): String {
        val u = user.orEmpty()
        return if (domain.isNullOrEmpty() || domain == "-") u else "$domain\\$u"
    }

    private fun dash(v: String?): String = if (v == null || v == "-") "" else v

    private fun unescape(s: String): String =
        s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'").replace("&amp;", "&")

    private fun failureKey(status: String, subStatus: String): String {
        val sub = subStatus.lowercase(Locale.ROOT)
        return if (sub.isEmpty() || sub == "0x0") status.lowercase(Locale.ROOT) else sub
    }
}
