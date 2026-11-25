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
import com.rconnect.android.data.ServerEntry
import com.rconnect.android.data.ServerFile
import com.rconnect.android.data.ServerImport
import com.rconnect.android.data.ServerStore
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

    val health: Health
        get() = when {
            error != null -> Health.Problem
            sessions == null -> Health.Pending
            sessions.any { it.connected } -> Health.InUse
            else -> Health.Free
        }

    val connected: List<RemoteSession> get() = sessions.orEmpty().filter { it.connected }
}

enum class Tab { Servers, People, Contacts }

class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val store = ServerStore(app)
    private val names = ClientNames(app)
    private val contactStore = ContactStore(app)
    private val detector = ChangeDetector(app)
    private val uiPrefs = app.getSharedPreferences("ui", Context.MODE_PRIVATE)
    val notificationPrefs = NotificationPrefs(app)

    private val parallel = Semaphore(MAX_PARALLEL)
    private val saveMutex = Mutex()
    private val pendingEvents = java.util.Collections.synchronizedSet(HashSet<String>())

    private val _servers = MutableStateFlow(store.load())
    val servers: StateFlow<List<ServerEntry>> = _servers

    private val _contacts = MutableStateFlow(contactStore.load())
    val contacts: StateFlow<List<Contact>> = _contacts

    private val _states = MutableStateFlow<Map<String, ServerState>>(emptyMap())
    val states: StateFlow<Map<String, ServerState>> = _states

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
        if (id != null) refresh(id, force = true)
    }

    private fun persistServers(snapshot: List<ServerEntry>) {
        viewModelScope.launch(Dispatchers.IO) {
            saveMutex.withLock { store.save(snapshot) }
        }
    }

    fun save(entry: ServerEntry) {
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

    fun delete(id: String) {
        val updated = _servers.value.filter { it.id != id }
        _servers.value = updated
        _states.update { it - id }
        pendingEvents.remove(id)
        detector.forget(id)
        persistServers(updated)
        if (_selected.value == id) _selected.value = null
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
        contactStore.save(_contacts.value)
    }

    fun deleteContact(id: String) {
        _contacts.update { list -> list.filter { it.id != id } }
        contactStore.save(_contacts.value)
    }

    /** Adds the entries of an address book file; returns how many were added or changed, or null if the text is not an address book. */
    fun importContacts(text: String): Int? {
        val incoming = ContactJson.read(text) ?: return null
        var changed = 0
        _contacts.update { current ->
            val list = current.toMutableList()
            for (c in incoming) {
                val same = list.indexOfFirst { ContactBook.sameMatch(it, c) }
                when {
                    same < 0 -> { list += c; changed++ }
                    list[same].name != c.name -> { list[same] = c.copy(id = list[same].id); changed++ }
                }
            }
            list
        }
        contactStore.save(_contacts.value)
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

    /** Turns the background check on or off together with the notifications. */
    fun setNotificationsEnabled(on: Boolean) {
        notificationPrefs.enabled = on
        if (on) CheckWorker.schedule(getApplication()) else CheckWorker.cancel(getApplication())
        WatchService.sync(getApplication())
    }

    fun setWatching(on: Boolean) {
        notificationPrefs.watch = on
        WatchService.sync(getApplication())
    }

    fun groups(): List<String> = _servers.value.map { it.group }.filter { it.isNotBlank() }.distinct().sorted()

    /** Checks every server whose last answer is older than [STALE_MS], or all of them when [force]. */
    fun refreshAll(force: Boolean = false) {
        for (s in _servers.value) refresh(s.id, force)
    }

    /** Asks one server who is connected; the open server page also reads the sign-in history. */
    fun refresh(id: String, force: Boolean = false) {
        val server = _servers.value.firstOrNull { it.id == id } ?: return
        val current = _states.value[id]
        val withEvents = _selected.value == id
        if (current?.checking == true) {
            if (withEvents) pendingEvents.add(id)
            return
        }
        if (!force && !withEvents && current != null && System.currentTimeMillis() - current.checkedAt < STALE_MS) return
        if (withEvents) pendingEvents.remove(id)
        setState(id) { it.copy(checking = true) }
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                parallel.withPermit {
                    try {
                        val client = WinRmClient(
                            host = server.host.trim(),
                            user = server.user.trim(),
                            password = server.password,
                            timeoutMs = if (withEvents) 30000 else 15000,
                            rdpPort = server.port,
                        )
                        Result.success(client.snapshot(withEvents))
                    } catch (e: Exception) {
                        Result.failure(e)
                    }
                }
            }
            if (_servers.value.none { it.id == id }) return@launch
            val snapshot = result.getOrNull()?.let { withNames(id, it) }
            viewModelScope.launch(Dispatchers.Default) {
                for (c in detector.detect(id, server.title, snapshot != null, snapshot?.sessions, _contacts.value))
                    if (notificationPrefs.wants(c)) Notifier.post(getApplication(), c)
            }
            if (snapshot != null) {
                setState(id) {
                    // Without the event log the connect time is not known: keep the one learned earlier for the same session.
                    val sessions = snapshot.sessions.map { s ->
                        val before = it.sessions?.firstOrNull { o ->
                            o.id == s.id && o.user.equals(s.user, true) &&
                                (o.logonUtcMs == s.logonUtcMs || o.logonUtcMs == 0L || s.logonUtcMs == 0L)
                        }
                        if (s.connectedUtcMs == 0L && before != null) s.copy(connectedUtcMs = before.connectedUtcMs) else s
                    }
                    ServerState(
                        sessions = sessions,
                        events = if (withEvents) snapshot.events else it.events,
                        checkedAt = System.currentTimeMillis(),
                        serverNowUtcMs = snapshot.serverNowUtcMs,
                        latencyMs = if (snapshot.latencyMs >= 0) snapshot.latencyMs else it.latencyMs,
                    )
                }
            } else {
                val e = result.exceptionOrNull()
                val kind = when (e) {
                    is WinRmException.Unreachable -> if (e.rdpOpen) ServerState.ErrorKind.NoWinRm else ServerState.ErrorKind.Unreachable
                    is WinRmException.AuthFailed -> ServerState.ErrorKind.AuthFailed
                    else -> ServerState.ErrorKind.Other
                }
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
                refresh(id, force = true)
            }
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
    }
}
