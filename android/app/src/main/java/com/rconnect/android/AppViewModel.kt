package com.rconnect.android

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.rconnect.android.data.ClientNames
import com.rconnect.android.data.ServerEntry
import com.rconnect.android.data.ServerImport
import com.rconnect.android.data.ServerStore
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
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext

enum class Health { Pending, Free, InUse, Problem }

/** What the last check of one server found. */
data class ServerState(
    val sessions: List<RemoteSession>? = null,   // null: never answered
    val events: List<AccessEvent>? = null,
    val checkedAt: Long = 0,                     // phone clock, ms
    val checking: Boolean = false,
    val error: String? = null,
    val errorKind: ErrorKind = ErrorKind.None,
) {
    enum class ErrorKind { None, Unreachable, AuthFailed, Other }

    val health: Health
        get() = when {
            error != null -> Health.Problem
            sessions == null -> Health.Pending
            sessions.any { it.connected } -> Health.InUse
            else -> Health.Free
        }

    val connected: List<RemoteSession> get() = sessions.orEmpty().filter { it.connected }
}

enum class Tab { Servers, People }

class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val store = ServerStore(app)
    private val names = ClientNames(app)
    private val parallel = Semaphore(MAX_PARALLEL)

    private val _servers = MutableStateFlow(store.load())
    val servers: StateFlow<List<ServerEntry>> = _servers

    private val _states = MutableStateFlow<Map<String, ServerState>>(emptyMap())
    val states: StateFlow<Map<String, ServerState>> = _states

    /** The server whose page is open, or null for the lists. */
    private val _selected = MutableStateFlow<String?>(null)
    val selected: StateFlow<String?> = _selected

    val tab = MutableStateFlow(Tab.Servers)
    /** Null: all servers. */
    val filter = MutableStateFlow<Health?>(null)
    val query = MutableStateFlow("")

    fun select(id: String?) {
        _selected.value = id
        if (id != null) refresh(id, force = true)
    }

    fun save(entry: ServerEntry) {
        _servers.update { list -> if (list.any { it.id == entry.id }) list.map { if (it.id == entry.id) entry else it } else list + entry }
        store.save(_servers.value)
        refresh(entry.id, force = true)
    }

    fun delete(id: String) {
        _servers.update { list -> list.filter { it.id != id } }
        _states.update { it - id }
        store.save(_servers.value)
        if (_selected.value == id) _selected.value = null
    }

    /** Adds the servers of a pasted list; returns how many were added and how many lines were skipped. */
    fun import(text: String): Pair<Int, Int> {
        val result = ServerImport.parse(text, _servers.value)
        if (result.servers.isNotEmpty()) {
            _servers.update { it + result.servers }
            store.save(_servers.value)
            for (s in result.servers) refresh(s.id)
        }
        return result.servers.size to result.skipped
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
        if (current?.checking == true) return
        val withEvents = _selected.value == id
        if (!force && !withEvents && current != null && System.currentTimeMillis() - current.checkedAt < STALE_MS) return
        setState(id) { it.copy(checking = true) }
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                parallel.withPermit {
                    try {
                        val client = WinRmClient(server.host.trim(), server.user.trim(), server.password,
                            timeoutMs = if (withEvents) 30000 else 15000)
                        Result.success(client.snapshot(withEvents))
                    } catch (e: Exception) {
                        Result.failure(e)
                    }
                }
            }
            val snapshot = result.getOrNull()?.let { withNames(id, it) }
            if (snapshot != null) {
                setState(id) {
                    // Without the event log the connect time is not known: keep the one learned earlier for the same session.
                    val sessions = snapshot.sessions.map { s ->
                        val before = it.sessions?.firstOrNull { o -> o.id == s.id && o.logonUtcMs == s.logonUtcMs }
                        if (s.connectedUtcMs == 0L && before != null) s.copy(connectedUtcMs = before.connectedUtcMs) else s
                    }
                    ServerState(sessions, if (withEvents) snapshot.events else it.events, System.currentTimeMillis())
                }
            } else {
                val e = result.exceptionOrNull()
                val kind = when (e) {
                    is WinRmException.Unreachable -> ServerState.ErrorKind.Unreachable
                    is WinRmException.AuthFailed -> ServerState.ErrorKind.AuthFailed
                    else -> ServerState.ErrorKind.Other
                }
                setState(id) { it.copy(checking = false, error = e?.message ?: "", errorKind = kind, checkedAt = System.currentTimeMillis()) }
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
        const val STALE_MS = 30_000L
    }
}
