#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// A Windows session on a server, from `quser`.
struct RemoteSession {
    QString user;
    QString client;        // computer the session is connected from, empty if unknown
    int id = 0;
    bool connected = false; // false: logged on but disconnected
    int idleMinutes = -1;   // -1 if the input measurement was unavailable
    qint64 logonUtcMs = 0;  // when the session signed in (ms since epoch, UTC); 0 if unknown
    qint64 connectedSinceUtcMs = 0; // start of the current connection, including a reconnect

    bool operator==(const RemoteSession &) const = default;
};

// Something that happened on a server, from its event logs: Remote Desktop session changes
// (TerminalServices-LocalSessionManager), authentications (RemoteConnectionManager) and, if the
// server audits them, logons, failed logons and disconnects (Security). Also changes that point
// to tampering: new accounts, administrators, services and scheduled tasks, cleared logs.
struct AccessEvent {
    enum Kind {
        Logon = 21, Logoff = 23, Disconnect = 24, Reconnect = 25, // LocalSessionManager
        Authenticated = 1149,                                     // RemoteConnectionManager
        SecurityLogon = 4624, FailedLogon = 4625,                 // Security
        SecurityReconnect = 4778, SecurityDisconnect = 4779,
        UserCreated = 4720, GroupMemberAdded = 4732,              // Security (need auditing)
        TaskCreated = 4698, TaskRemoved = 4699, TaskEnabled = 4700, TaskDisabled = 4701, TaskUpdated = 4702,
        AuditPolicyChanged = 4719, UserEnabled = 4722, PasswordChangeAttempt = 4723, PasswordResetAttempt = 4724,
        UserDisabled = 4725, UserDeleted = 4726, GroupMemberRemoved = 4733, AccountLocked = 4740,
        LogCleared = 1102,                                        // Security
        SystemLogCleared = 104,                                   // System
        ServiceInstalled = 7045,                                  // System
    };

    Kind kind = Logon;
    QString source;     // LSM, RCM, SEC, AUD or SYS: each query has its own read position
    QString timeUtc;    // as the server wrote it, 7 fraction digits: 2026-10-31T08:15:30.1234567Z
    QString user;       // DOMAIN and user name
    QString address;    // client IP, empty if the event has none
    QString client;     // client computer name, empty if the event has none
    QString detail;     // logon type, failure reason, session name
    int sessionId = 0;
    qint64 recordId = 0;
};

// Where to continue reading one event log.
struct EventCursor {
    QString afterUtc;      // from this event on (inclusive); wins when set
    qint64 lookbackMs = 0; // otherwise this far back from the server's clock; 0 = 7 days
    qint64 afterRecordId = 0; // tie-breaker at afterUtc
};

struct SessionQuery {
    enum Result { Ok, Unreachable, AuthFailed, Failed };

    Result result = Failed;
    QList<RemoteSession> sessions;
    bool sessionsRead = false; // an actual session listing was returned
    QList<AccessEvent> events; // per log oldest first; only when events were asked for
    int eventsSkipped = 0;     // events of the logs that could not be read
    QSet<QString> failedEventSources; // channels that did not confirm a successful read
    bool moreEvents = false; // at least one channel filled the page
    qint64 serverNowUtcMs = 0; // the server's own clock at the check (ms since epoch, UTC); 0 if unknown
    QByteArray output;         // raw standard output of runCommand()
    bool viaWts = false;       // the sessions come from Windows' own query (WtsQuery), so there are no events
    QString error;
    QString fallbackError; // WTS result when both routes failed
};

namespace WinRm {

// The logs read for the access log: the values of AccessEvent::source.
const QStringList &eventSources();

// Lists the sessions on `host` over WinRM (HTTP 5985, NTLM with message encryption).
// Blocking; needs an administrator account on the server.
//
// `cursors` (by source): also reads the access events after each cursor, at most 300 per log and
// call. Without it no events are read. `includeTimes` still requests session start times and the server clock.
SessionQuery querySessions(const QString &host, const QString &user, const QString &domain,
                           const QString &password, int timeoutMs,
                           const QHash<QString, EventCursor> *cursors = nullptr, bool includeTimes = false);

// Runs one command line (cmd.exe) on the server; the output is in `output`.
SessionQuery runCommand(const QString &host, const QString &user, const QString &domain,
                        const QString &password, int timeoutMs, const QString &command);

SessionQuery runPowerShell(const QString &host, const QString &user, const QString &domain,
                           const QString &password, int timeoutMs, const QString &script);

} // namespace WinRm
