package com.rconnect.android.winrm

import java.time.Instant

/** Reads what the commands in [WinRmClient] print. Pure code, no network. */
object Parsing {
    const val MARKER = "##RCONNECT##"
    const val EVENTS_MARKER = "##RCONNECT-EVENTS##"
    const val LOG_MARKER = "##RCLOG:"
    const val START_MARKER = "##RCSTART##"

    private val space = Regex("\\s+")
    private val dateField = Regex("^\\d{1,4}[./-]\\d{1,2}[./-]\\d{1,4}$")
    private val digits = Regex("^\\d+$")

    /** Splits the output into the events (before [EVENTS_MARKER]) and the sessions (after it). */
    fun snapshot(output: String, withEvents: Boolean): ServerSnapshot {
        var sessionsText = output
        var events = emptyList<AccessEvent>()
        if (withEvents) {
            val split = output.indexOf(EVENTS_MARKER)
            if (split >= 0) {
                events = parseEventLogs(output.substring(0, split))
                sessionsText = output.substring(split + EVENTS_MARKER.length).substringAfter('\n', "")
            }
        }
        var serverNow = 0L
        val sessions = parseSessions(sessionsText) { serverNow = it }
        val sorted = events.sortedByDescending { it.timeUtcMs }
        // The session log only knows the client's address; the security log's sign-ins also know its name.
        val nameOf = HashMap<String, String>()
        for (e in sorted) if (e.client.isNotEmpty() && e.address.isNotEmpty()) nameOf.putIfAbsent(e.address, e.client)
        val named = sorted.filter { it.kind != AccessEvent.Kind.Logon }
            .map { if (it.client.isEmpty()) it.copy(client = nameOf[it.address].orEmpty()) else it }
        val withConnect = sessions.map { s ->
            val last = named.firstOrNull { it.sessionId == s.id && (it.kind == AccessEvent.Kind.SignIn || it.kind == AccessEvent.Kind.Reconnect) }
            if (last == null) s else s.copy(connectedUtcMs = last.timeUtcMs)
        }
        return ServerSnapshot(withConnect, named, serverNow)
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
            val idle = if (dateIndex > idIndex + 1) parseIdle(t[dateIndex - 1]) else 0
            sessions += RemoteSession(
                user = t[0].removePrefix(">"), client = "", id = t[idIndex].toInt(),
                connected = connected, idleMinutes = idle, logonUtcMs = 0,
            )
        }

        // "NOW <time>" is the server clock; then "<session id> <time>" per shell, the first one is the sign-in.
        val logon = HashMap<Int, Long>()
        for (raw in starts.split('\n')) {
            val parts = raw.trim().split(' ').filter { it.isNotEmpty() }
            if (parts.size != 2) continue
            val time = parseTime(parts[1]) ?: continue
            if (parts[0] == "NOW") { onServerNow(time); continue }
            val id = parts[0].toIntOrNull() ?: continue
            val known = logon[id]
            if (known == null || time < known) logon[id] = time
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
        return sessions.map { it.copy(client = clientOf[it.id].orEmpty(), logonUtcMs = logon[it.id] ?: 0) }
    }

    // ".", "none": under a minute; "5"; "1:05"; "2+03:04" (days+hours:minutes)
    fun parseIdle(text: String): Int {
        var days = 0
        var rest = text
        val plus = rest.indexOf('+')
        if (plus >= 0) {
            days = rest.substring(0, plus).toIntOrNull() ?: 0
            rest = rest.substring(plus + 1)
        }
        val colon = rest.indexOf(':')
        val minutes = if (colon >= 0) (rest.substring(0, colon).toIntOrNull() ?: 0) * 60 + (rest.substring(colon + 1).toIntOrNull() ?: 0)
        else rest.toIntOrNull() ?: 0
        return days * 1440 + minutes
    }

    // 2026-10-31T08:15:30.1234567Z, or with fewer fraction digits. Milliseconds are enough.
    fun parseTime(text: String): Long? = try {
        val dot = text.indexOf('.')
        val ms = if (dot < 0) text.trimEnd('Z') + ".000" else text.substring(0, dot) + "." + text.substring(dot + 1).trimEnd('Z').padEnd(3, '0').take(3)
        Instant.parse(ms + "Z").toEpochMilli()
    } catch (e: Exception) {
        null
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

    private fun field(xml: String, name: String): String =
        Regex("<$name>(.*?)</$name>", RegexOption.DOT_MATCHES_ALL).find(xml)?.groupValues?.get(1)
            ?.let(::unescape)?.trim().orEmpty()

    fun parseEvents(text: String): List<AccessEvent> {
        // Characters XML forbids (and bytes that were not text) must not hide the other events.
        val clean = buildString(text.length) {
            for (c in text) append(if ((c.code < 0x20 && c != '\t' && c != '\n' && c != '\r') || c.code == 0xFFFE || c.code == 0xFFFF) ' ' else c)
        }
        val out = ArrayList<AccessEvent>()
        for (m in eventBlock.findAll(clean)) {
            val xml = m.value
            val id = eventId.find(xml)?.groupValues?.get(1)?.toIntOrNull() ?: continue
            val time = timeCreated.find(xml)?.groupValues?.get(1)?.let(::parseTime) ?: continue
            val record = recordId.find(xml)?.groupValues?.get(1)?.toLongOrNull() ?: 0
            val data = HashMap<String, String>()
            for (d in dataItem.findAll(xml)) data[d.groupValues[1]] = unescape(d.groupValues[2]).trim()
            out += when (id) {
                21, 23, 24, 25 -> AccessEvent(
                    kind = when (id) {
                        21 -> AccessEvent.Kind.SignIn
                        23 -> AccessEvent.Kind.SignOut
                        24 -> AccessEvent.Kind.Disconnect
                        else -> AccessEvent.Kind.Reconnect
                    },
                    timeUtcMs = time, user = field(xml, "User"), address = field(xml, "Address"), client = "",
                    detail = "", sessionId = field(xml, "SessionID").toIntOrNull() ?: 0, recordId = record,
                )
                4778, 4779 -> AccessEvent( // session reconnected / disconnected: the only events that name the client computer
                    kind = AccessEvent.Kind.Logon, timeUtcMs = time, user = account(data["AccountDomain"], data["AccountName"]),
                    address = dash(data["ClientAddress"]), client = dash(data["ClientName"]),
                    detail = "", sessionId = 0, recordId = record,
                )
                4625 -> AccessEvent(
                    kind = AccessEvent.Kind.FailedSignIn, timeUtcMs = time,
                    user = account(data["TargetDomainName"], data["TargetUserName"]),
                    address = dash(data["IpAddress"]), client = dash(data["WorkstationName"]),
                    detail = failureReason(data["Status"].orEmpty(), data["SubStatus"].orEmpty()),
                    sessionId = 0, recordId = record,
                )
                else -> continue
            }
        }
        return out
    }

    private fun account(domain: String?, user: String?): String {
        val u = user.orEmpty()
        return if (domain.isNullOrEmpty() || domain == "-") u else "$domain\\$u"
    }

    private fun dash(v: String?): String = if (v == null || v == "-") "" else v

    private fun unescape(s: String): String =
        s.replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&apos;", "'").replace("&amp;", "&")

    private val reasons = mapOf(
        "0xc0000064" to "unknown user name", "0xc000006a" to "wrong password", "0xc000006d" to "logon failed",
        "0xc000006e" to "account restriction", "0xc000006f" to "outside allowed hours",
        "0xc0000070" to "workstation restriction", "0xc0000071" to "password expired",
        "0xc0000072" to "account disabled", "0xc0000133" to "clocks out of sync",
        "0xc000015b" to "logon type not allowed", "0xc0000193" to "account expired",
        "0xc0000234" to "account locked out",
    )

    private fun failureReason(status: String, subStatus: String): String {
        val sub = subStatus.lowercase()
        val key = if (sub.isEmpty() || sub == "0x0") status.lowercase() else sub
        return reasons[key] ?: key
    }
}
