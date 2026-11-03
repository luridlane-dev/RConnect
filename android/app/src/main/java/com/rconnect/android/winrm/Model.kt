package com.rconnect.android.winrm

/** A Windows session on a server, from `quser`. */
data class RemoteSession(
    val user: String,
    val client: String,          // computer the session is connected from, empty if unknown
    val id: Int,
    val connected: Boolean,      // false: signed in but disconnected
    val idleMinutes: Int,
    val logonUtcMs: Long,        // 0 if unknown
    val connectedUtcMs: Long = 0, // last (re)connect from the session log, 0 if unknown
)

/** Something that happened on a server (session log, failed sign-ins). */
data class AccessEvent(
    val kind: Kind,
    val timeUtcMs: Long,
    val user: String,
    val address: String,
    val client: String,
    val detail: String,
    val sessionId: Int,
    val recordId: Long,
) {
    enum class Kind { SignIn, SignOut, Disconnect, Reconnect, FailedSignIn, Logon }
}

data class ServerSnapshot(
    val sessions: List<RemoteSession>,
    val events: List<AccessEvent>,   // newest first
    val serverNowUtcMs: Long,        // the server's own clock, 0 if unknown
)

sealed class WinRmException(message: String) : Exception(message) {
    /** [rdpOpen]: the server still answers on the Remote Desktop port, so it is up and only WinRM is not. */
    class Unreachable(message: String, val rdpOpen: Boolean = false) : WinRmException(message)
    class AuthFailed(message: String) : WinRmException(message)
    class Failed(message: String) : WinRmException(message)
}
