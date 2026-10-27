#pragma once
#include <QList>
#include <QString>

// A Windows session on a server, from `quser`.
struct RemoteSession {
    QString user;
    QString client;        // computer the session is connected from, empty if unknown
    int id = 0;
    bool connected = false; // false: logged on but disconnected
    int idleMinutes = 0;

    bool operator==(const RemoteSession &) const = default;
};

struct SessionQuery {
    enum Result { Ok, Unreachable, AuthFailed, Failed };

    Result result = Failed;
    QList<RemoteSession> sessions;
    QString error;
};

namespace WinRm {

// Lists the sessions on `host` over WinRM (HTTP 5985, NTLM with message encryption).
// Blocking; needs an administrator account on the server.
SessionQuery querySessions(const QString &host, const QString &user, const QString &domain,
                           const QString &password, int timeoutMs);

} // namespace WinRm
