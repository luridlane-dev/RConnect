package com.rconnect.android.winrm

/** A Windows session on a server, from `quser`. */
data class RemoteSession(
    val user: String,
    val client: String,          // computer the session is connected from, empty if unknown
    val id: Int,
    val connected: Boolean,      // false: signed in but disconnected
    val idleMinutes: Int,        // -1 if unknown
    val logonUtcMs: Long,        // 0 if unknown
    val connectedUtcMs: Long = 0, // last (re)connect from the session log, 0 if unknown
)

/** Something that happened on a server (session log, security log, system log). */
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
    enum class Kind {
        SignIn,
        SignOut,
        Disconnect,
        Reconnect,
        FailedSignIn,
        Logon,
        UserCreated,
        UserEnabled,
        PasswordChangeAttempt,
        PasswordResetAttempt,
        UserDisabled,
        UserDeleted,
        GroupMemberAdded,
        GroupMemberRemoved,
        TaskCreated,
        TaskRemoved,
        TaskEnabled,
        TaskDisabled,
        TaskUpdated,
        ServiceInstalled,
        AuditPolicyChanged,
        AccountLocked,
        LogCleared,
    }

    val isSecurity: Boolean
        get() = when (kind) {
            Kind.UserCreated,
            Kind.UserEnabled,
            Kind.PasswordChangeAttempt,
            Kind.PasswordResetAttempt,
            Kind.UserDisabled,
            Kind.UserDeleted,
            Kind.GroupMemberAdded,
            Kind.GroupMemberRemoved,
            Kind.TaskCreated,
            Kind.TaskRemoved,
            Kind.TaskEnabled,
            Kind.TaskDisabled,
            Kind.TaskUpdated,
            Kind.ServiceInstalled,
            Kind.AuditPolicyChanged,
            Kind.AccountLocked,
            Kind.LogCleared,
            -> true
            else -> false
        }
}

data class ServerSnapshot(
    val sessions: List<RemoteSession>,
    val events: List<AccessEvent>,   // newest first
    val serverNowUtcMs: Long,        // the server's own clock, 0 if unknown
    val latencyMs: Int = -1,         // TCP connect time in ms, -1 if unknown
)

sealed class WinRmException(message: String) : Exception(message) {
    /** [rdpOpen]: the server still answers on the Remote Desktop port, so it is up and only WinRM is not. */
    class Unreachable(message: String, val rdpOpen: Boolean = false) : WinRmException(message)
    class AuthFailed(message: String) : WinRmException(message)
    class Failed(message: String) : WinRmException(message)
}
