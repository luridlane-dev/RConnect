package com.rconnect.android

import android.app.Application
import android.content.Context
import androidx.core.content.edit
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rconnect.android.data.ClientNames
import com.rconnect.android.data.Contact
import com.rconnect.android.data.ContactBook
import com.rconnect.android.data.ContactJson
import com.rconnect.android.data.ContactStore
import com.rconnect.android.data.EventStore
import com.rconnect.android.data.ServerEntry
import com.rconnect.android.data.ServerFile
import com.rconnect.android.data.ServerImport
import com.rconnect.android.data.ServerStore
import com.rconnect.android.data.StoredEvent
import com.rconnect.android.notify.ChangeDetector
import com.rconnect.android.notify.CheckWorker
import com.rconnect.android.notify.NotificationPrefs
import com.rconnect.android.notify.Notifier
import com.rconnect.android.notify.WatchService
import com.rconnect.android.winrm.AccessEvent
import com.rconnect.android.winrm.RemoteSession
import com.rconnect.android.winrm.ServerSnapshot
import com.rconnect.android.winrm.WinRmClient
import com.rconnect.android.winrm.WinRmException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.util.Locale

enum class Health { Pending, Free, InUse, Problem }

enum class ThemeMode { Dark, Light }

enum class ServerFilter { All, Favorites, Available, InUse, Offline }

/** What the last check of one server found. */
data class ServerState(
    val sessions: List<RemoteSession>? = null,   // null: never answered
    val events: List<AccessEvent>? = null,
    val checkedAt: Long = 0,                     // phone clock, ms
    val serverNowUtcMs: Long = 0,                // server clock, ms
    val latencyMs: Int = -1,                     // TCP connect time, ms
    val checking: Boolean = false,
    val error: String? = null,
    val errorKind: ErrorKind = ErrorKind.None,
) {
    enum class ErrorKind { None, Unreachable, NoWinRm, AuthFailed, Other }

    val connected: List<RemoteSession> = sessions?.filter { it.connected }.orEmpty()

    val health: Health = when {
        error != null -> Health.Problem
        sessions == null -> Health.Pending
        connected.isNotEmpty() -> Health.InUse
        else -> Health.Free
    }
}

enum class Tab { Servers, People, Logs, Contacts }

class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val store = ServerStore(app)
    private val names = ClientNames(app)
    private val contactStore = ContactStore(app)
    private val eventStore = EventStore(app)
    private val detector = ChangeDetector(app)
    private val uiPrefs = app.getSharedPreferences("ui", Context.MODE_PRIVATE)
    val notificationPrefs = NotificationPrefs(app)

    private val parallel = Semaphore(MAX_PARALLEL)
    private val saveMutex = Mutex()
    private val contactSaveMutex = Mutex()
    private val eventMergeMutex = Mutex()
    private val eventSaveMutex = Mutex()
    private val logSaveDirty = java.util.concurrent.atomic.AtomicBoolean(false)
    private val logSaveRunning = java.util.concurrent.atomic.AtomicBoolean(false)
    private val pendingEvents = java.util.Collections.synchronizedSet(HashSet<String>())
    private val failureStreak = java.util.concurrent.ConcurrentHashMap<String, Int>()

    private val _servers = MutableStateFlow<List<ServerEntry>>(emptyList())
    val servers: StateFlow<List<ServerEntry>> = _servers

    private val _contacts = MutableStateFlow<List<Contact>>(emptyList())
    val contacts: StateFlow<List<Contact>> = _contacts

    private val _states = MutableStateFlow<Map<String, ServerState>>(emptyMap())
    val states: StateFlow<Map<String, ServerState>> = _states

    private val _logs = MutableStateFlow<List<StoredEvent>>(emptyList())
    val logs: StateFlow<List<StoredEvent>> = _logs

    private val _fetchingLogs = MutableStateFlow(false)
    val fetchingLogs: StateFlow<Boolean> = _fetchingLogs

    private val _themeMode = MutableStateFlow(
        if (uiPrefs.getString("theme", "dark") == "light") ThemeMode.Light else ThemeMode.Dark,
    )
    val themeMode: StateFlow<ThemeMode> = _themeMode

    private val _listView = MutableStateFlow(uiPrefs.getBoolean("list_view", false))
    val listView: StateFlow<Boolean> = _listView

    /** The server whose page is open, or null for the lists. */
    private val _selected = MutableStateFlow<String?>(null)
    val selected: StateFlow<String?> = _selected

    val tab = MutableStateFlow(Tab.Servers)
    val filter = MutableStateFlow(ServerFilter.All)
    val serverQuery = MutableStateFlow("")
    val peopleQuery = MutableStateFlow("")
    val logQuery = MutableStateFlow("")

    init {
        viewModelScope.launch(Dispatchers.IO) {
            val loadedServers = store.load()
            val loadedContacts = contactStore.load()
            _servers.value = loadedServers
            _contacts.value = loadedContacts
            withContext(Dispatchers.Main.immediate) {
                val sel = _selected.value
                if (sel != null) refresh(sel, force = true, forceEvents = true)
                else refreshAll()
            }
            val loadedLogs = eventStore.load()
            if (loadedLogs.isNotEmpty()) _logs.value = loadedLogs
        }
    }

    fun setThemeMode(mode: ThemeMode) {
        _themeMode.value = mode
        uiPrefs.edit { putString("theme", if (mode == ThemeMode.Light) "light" else "dark") }
    }

    fun setListView(list: Boolean) {
        _listView.value = list
        uiPrefs.edit { putBoolean("list_view", list) }
    }

    fun select(id: String?) {
        _selected.value = id
        if (id != null) refresh(id, force = true, forceEvents = true)
    }

    private fun persistServers(snapshot: List<ServerEntry>) {
        viewModelScope.launch(Dispatchers.IO) {
            saveMutex.withLock { store.save(snapshot) }
        }
    }

    private fun persistContacts(snapshot: List<Contact>) {
        viewModelScope.launch(Dispatchers.IO) {
            contactSaveMutex.withLock { contactStore.save(snapshot) }
        }
    }

    private fun schedulePersistLogs() {
        logSaveDirty.set(true)
        if (!logSaveRunning.compareAndSet(false, true)) return
        viewModelScope.launch(Dispatchers.IO) {
            try {
                while (logSaveDirty.getAndSet(false)) {
                    val snapshot = _logs.value
                    eventSaveMutex.withLock { eventStore.save(snapshot) }
                }
            } finally {
                logSaveRunning.set(false)
                if (logSaveDirty.get()) schedulePersistLogs()
            }
        }
    }

    fun save(entry: ServerEntry) {
        failureStreak.remove(entry.id)
        val updated = _servers.value.let { list ->
            if (list.any { it.id == entry.id }) list.map { if (it.id == entry.id) entry else it } else list + entry
        }
        _servers.value = updated
        persistServers(updated)
        refresh(entry.id, force = true)
    }

    fun toggleFavorite(id: String) {
        val updated = _servers.value.map { if (it.id == id) it.copy(favorite = !it.favorite) else it }
        _servers.value = updated
        persistServers(updated)
    }

    fun renameGroup(from: String, to: String) {
        val target = to.trim()
        if (from.isBlank() || from == target) return
        val updated = _servers.value.map { if (it.group == from) it.copy(group = target) else it }
        _servers.value = updated
        persistServers(updated)
    }

    fun delete(id: String) {
        val updated = _servers.value.filter { it.id != id }
        _servers.value = updated
        _states.update { it - id }
        _logs.update { it.filter { e -> e.serverId != id } }
        schedulePersistLogs()
        pendingEvents.remove(id)
        failureStreak.remove(id)
        viewModelScope.launch(Dispatchers.IO) { detector.forget(id) }
        persistServers(updated)
        if (_selected.value == id) _selected.value = null
    }

    fun clearLogs() {
        _logs.value = emptyList()
        logSaveDirty.set(false)
        viewModelScope.launch(Dispatchers.IO) {
            eventSaveMutex.withLock { eventStore.clear() }
        }
    }

    /** Adds the servers of a pasted list; returns how many were added and how many lines were skipped. */
    fun import(text: String): Pair<Int, Int> {
        val result = ServerImport.parse(text, _servers.value)
        if (result.servers.isNotEmpty()) {
            val updated = _servers.value + result.servers
            _servers.value = updated
            persistServers(updated)
            for (s in result.servers) refresh(s.id)
        }
        return result.servers.size to result.skipped
    }

    fun saveContact(contact: Contact) {
        if (!contact.valid) return
        _contacts.update { list -> if (list.any { it.id == contact.id }) list.map { if (it.id == contact.id) contact else it } else list + contact }
        persistContacts(_contacts.value)
    }

    fun deleteContact(id: String) {
        _contacts.update { list -> list.filter { it.id != id } }
        persistContacts(_contacts.value)
    }

    /** Adds the entries of an address book file; returns how many were added or changed, or null if the text is not an address book. */
    fun importContacts(text: String): Int? {
        val incoming = ContactJson.read(text) ?: return null
        var changed = 0
        _contacts.update { current ->
            val list = current.toMutableList()
            val indexByKey = HashMap<String, Int>(list.size + incoming.size)
            for (i in list.indices) indexByKey.putIfAbsent(ContactBook.matchKey(list[i]), i)
            for (c in incoming) {
                val key = ContactBook.matchKey(c)
                val same = indexByKey[key]
                when {
                    same == null -> {
                        indexByKey[key] = list.size
                        list += c
                        changed++
                    }
                    list[same].name != c.name -> {
                        list[same] = c.copy(id = list[same].id)
                        changed++
                    }
                }
            }
            list
        }
        if (changed > 0) persistContacts(_contacts.value)
        return changed
    }

    fun exportContacts(): String = ContactJson.write(_contacts.value)

    /**
     * Adds what is not in the list yet. Returns how many were added and how many were left out: already there, or
     * [unusable] in the file. A server that is already there but has no saved password takes the file's password.
     */
    fun addServers(list: List<ServerEntry>, unusable: Int = 0): Pair<Int, Int> {
        fun key(s: ServerEntry) = s.host.trim().lowercase(Locale.ROOT) + "|" + s.user.trim().lowercase(Locale.ROOT)
        val known = _servers.value.map(::key).toMutableSet()
        val fresh = ArrayList<ServerEntry>()
        val filled = HashMap<String, String>() // key -> password
        for (s in list) {
            if (known.add(key(s))) fresh += s
            else if (s.password.isNotEmpty()) filled.putIfAbsent(key(s), s.password)
        }
        var changed = fresh.isNotEmpty()
        val current = _servers.value.map {
            val pw = filled[key(it)]
            if (it.password.isEmpty() && pw != null) { changed = true; it.copy(password = pw) } else it
        }
        if (changed) {
            val updated = current + fresh
            _servers.value = updated
            persistServers(updated)
            for (s in fresh) refresh(s.id)
            for (s in current) if (filled.containsKey(key(s))) refresh(s.id, true)
        }
        return fresh.size to (list.size - fresh.size + unusable)
    }

    suspend fun exportServersAsync(passphrase: String): String = withContext(Dispatchers.Default) {
        ServerFile.write(_servers.value, passphrase)
    }

    suspend fun readServerFileAsync(text: String, passphrase: String = ""): ServerFile.Read = withContext(Dispatchers.Default) {
        ServerFile.read(text, passphrase)
    }

    suspend fun sendMessage(serverId: String, sessionId: Int, text: String): Result<Unit> {
        val server = _servers.value.firstOrNull { it.id == serverId } ?: return Result.failure(IllegalStateException("Server not found"))
        return withContext(Dispatchers.IO) {
            runCatching {
                WinRmClient(
                    host = server.host.trim(),
                    user = server.user.trim(),
                    password = server.password,
                    timeoutMs = 15000,
                    rdpPort = server.port,
                ).sendMessage(sessionId, text)
            }
        }
    }

    /** Turns the background check on or off together with the notifications. */
    fun setNotificationsEnabled(on: Boolean) {
        notificationPrefs.enabled = on
        if (on && !notificationPrefs.watch) CheckWorker.schedule(getApplication())
        else CheckWorker.cancel(getApplication())
        WatchService.sync(getApplication())
    }

    fun setWatching(on: Boolean) {
        notificationPrefs.watch = on
        if (notificationPrefs.enabled && !on) CheckWorker.schedule(getApplication())
        else CheckWorker.cancel(getApplication())
        WatchService.sync(getApplication())
    }

    fun groups(): List<String> = _servers.value.map { it.group }.filter { it.isNotBlank() }.distinct().sorted()

    /** Checks every server whose last answer is older than [STALE_MS], or all of them when [force]. */
    fun refreshAll(force: Boolean = false) {
        for (s in _servers.value) refresh(s.id, force)
    }

    /** Fetches full 7-day event logs from every server into the shared activity log. */
    fun refreshAllLogs() {
        val list = _servers.value
        if (list.isEmpty() || _fetchingLogs.value) return
        _fetchingLogs.value = true
        viewModelScope.launch {
            try {
                val jobs = list.map { server ->
                    launch {
                        if (_states.value[server.id]?.checking == true) return@launch
                        setState(server.id) { it.copy(checking = true) }
                        val result = withContext(Dispatchers.IO) {
                            parallel.withPermit {
                                runCatching {
                                    val raw = WinRmClient(
                                        host = server.host.trim(),
                                        user = server.user.trim(),
                                        password = server.password,
                                        timeoutMs = 30000,
                                        rdpPort = server.port,
                                    ).snapshot(withEvents = true, includeTimes = false)
                                    withNames(server.id, raw)
                                }
                            }
                        }
                        if (_servers.value.none { it.id == server.id }) return@launch
                        val snap = result.getOrNull()
                        if (snap != null) {
                            failureStreak.remove(server.id)
                            recordEventsAsync(server.id, server.title, snap.events)
                            viewModelScope.launch(Dispatchers.IO) {
                                detector.markSecurityChecked(server.id)
                                for (c in detector.detectSecurityChanges(getApplication(), server.id, server.title, snap.events)) {
                                    if (notificationPrefs.wants(c)) Notifier.post(getApplication(), c)
                                }
                            }
                            setState(server.id) {
                                ServerState(
                                    sessions = snap.sessions,
                                    events = snap.events,
                                    checkedAt = System.currentTimeMillis(),
                                    serverNowUtcMs = snap.serverNowUtcMs,
                                    latencyMs = if (snap.latencyMs >= 0) snap.latencyMs else it.latencyMs,
                                )
                            }
                        } else {
                            setState(server.id) { it.copy(checking = false) }
                        }
                    }
                }
                jobs.forEach { it.join() }
            } finally {
                _fetchingLogs.value = false
            }
        }
    }

    /** Asks one server who is connected; the open server page also reads the sign-in and security history. */
    fun refresh(id: String, force: Boolean = false, forceEvents: Boolean = false) {
        val server = _servers.value.firstOrNull { it.id == id } ?: return
        val current = _states.value[id]
        val withEvents = forceEvents || _selected.value == id
        if (current?.checking == true) {
            if (withEvents) pendingEvents.add(id)
            return
        }
        val now = System.currentTimeMillis()
        if (!force && !withEvents && current != null) {
            // Never auto-retry refused credentials (prevents AD account lockouts); wait for edit or manual refresh.
            if (current.errorKind == ServerState.ErrorKind.AuthFailed) return
            val streak = failureStreak[id] ?: 0
            val minAge = if (streak > 0) {
                minOf(MAX_BACKOFF_MS, STALE_MS * (1L shl minOf(streak, 4)))
            } else STALE_MS
            if (now - current.checkedAt < minAge) return
        }
        if (withEvents) pendingEvents.remove(id)
        // Skip the extra LSM wevtutil query when all connected sessions already have a known start time.
        val needTimes = !withEvents && (
            current == null || current.sessions == null ||
                current.connected.any { it.connectedUtcMs <= 0L }
            )
        val checkSecurity = !withEvents && notificationPrefs.enabled && notificationPrefs.onSecurityChange &&
            detector.shouldCheckSecurity(id, now)
        setState(id) { it.copy(checking = true) }
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                parallel.withPermit {
                    try {
                        val client = WinRmClient(
                            host = server.host.trim(),
                            user = server.user.trim(),
                            password = server.password,
                            timeoutMs = if (withEvents) 30000 else 18000,
                            rdpPort = server.port,
                        )
                        val raw = client.snapshot(
                            withEvents = withEvents,
                            includeTimes = needTimes,
                            securityLookbackMs = if (checkSecurity) ChangeDetector.SECURITY_LOOKBACK_MS else null,
                        )
                        Result.success(withNames(id, raw))
                    } catch (e: Exception) {
                        Result.failure(e)
                    }
                }
            }
            if (_servers.value.none { it.id == id }) return@launch
            val snapshot = result.getOrNull()
            viewModelScope.launch(Dispatchers.IO) {
                for (c in detector.detect(id, server.title, snapshot != null, snapshot?.sessions, _contacts.value))
                    if (notificationPrefs.wants(c)) Notifier.post(getApplication(), c)
                if (snapshot != null && (withEvents || checkSecurity)) {
                    detector.markSecurityChecked(id)
                    for (c in detector.detectSecurityChanges(getApplication(), id, server.title, snapshot.events))
                        if (notificationPrefs.wants(c)) Notifier.post(getApplication(), c)
                }
            }
            if (snapshot != null) {
                failureStreak.remove(id)
                if ((withEvents || checkSecurity) && snapshot.events.isNotEmpty()) {
                    recordEventsAsync(id, server.title, snapshot.events)
                }
                var missingConnectTime = false
                setState(id) {
                    val sessions = snapshot.sessions.map { s ->
                        val before = it.sessions?.firstOrNull { o ->
                            o.id == s.id && o.user.equals(s.user, true) && o.connected
                        }
                        if (s.connected && s.connectedUtcMs == 0L && before != null && before.connectedUtcMs > 0L) {
                            s.copy(connectedUtcMs = before.connectedUtcMs)
                        } else {
                            if (s.connected && s.connectedUtcMs == 0L) missingConnectTime = true
                            s
                        }
                    }
                    ServerState(
                        sessions = sessions,
                        events = if (withEvents) snapshot.events else it.events,
                        checkedAt = System.currentTimeMillis(),
                        serverNowUtcMs = snapshot.serverNowUtcMs,
                        latencyMs = if (snapshot.latencyMs >= 0) snapshot.latencyMs else it.latencyMs,
                    )
                }
                // A brand-new connected session appeared while we skipped wevtutil; next poll (or detail open) fills its time.
                if (missingConnectTime && !needTimes && !withEvents) {
                    // Leave connectedUtcMs = 0L so the very next poll fetches it once.
                }
            } else {
                val e = result.exceptionOrNull()
                val kind = when (e) {
                    is WinRmException.Unreachable -> if (e.rdpOpen) ServerState.ErrorKind.NoWinRm else ServerState.ErrorKind.Unreachable
                    is WinRmException.AuthFailed -> ServerState.ErrorKind.AuthFailed
                    else -> ServerState.ErrorKind.Other
                }
                failureStreak[id] = (failureStreak[id] ?: 0) + 1
                setState(id) {
                    it.copy(
                        checking = false,
                        error = e?.message ?: "",
                        errorKind = kind,
                        checkedAt = System.currentTimeMillis(),
                    )
                }
            }
            if (pendingEvents.remove(id) && _selected.value == id && !withEvents) {
                refresh(id, force = true, forceEvents = true)
            }
        }
    }

    private fun recordEventsAsync(serverId: String, serverTitle: String, events: List<AccessEvent>) {
        if (events.isEmpty()) return
        viewModelScope.launch(Dispatchers.Default) {
            eventMergeMutex.withLock {
                val merged = EventStore.merge(_logs.value, serverId, serverTitle, events)
                _logs.value = merged
            }
            schedulePersistLogs()
        }
    }

    /** Remembers the computer name of each connected session and puts known names on the history. */
    private fun withNames(serverId: String, s: ServerSnapshot): ServerSnapshot {
        for (session in s.sessions) {
            if (!session.connected || session.client.isEmpty()) continue
            val connect = s.events.firstOrNull {
                it.sessionId == session.id && it.timeUtcMs == session.connectedUtcMs && it.address.isNotEmpty() && it.address != "LOCAL"
            } ?: continue
            names.put(serverId, connect.address, session.client)
        }
        if (s.events.isEmpty()) return s
        return s.copy(events = s.events.map {
            if (it.client.isNotEmpty() || it.address.isEmpty()) it else it.copy(client = names.get(serverId, it.address))
        })
    }

    private fun setState(id: String, change: (ServerState) -> ServerState) {
        _states.update { it + (id to change(it[id] ?: ServerState())) }
    }

    companion object {
        const val MAX_PARALLEL = 8
        const val STALE_MS = 10_000L
        private const val MAX_BACKOFF_MS = 120_000L
    }
}
