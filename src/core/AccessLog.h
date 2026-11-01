#pragma once
#include "WinRm.h"

#include <QHash>
#include <QList>
#include <QString>

struct ServerProfile;

// One line of access-log.csv.
struct AccessRecord {
    QString timeUtc;   // 2026-10-31T08:15:30.1234567Z
    QString serverId;
    QString server;    // name when the line was written
    QString host;
    QString source;    // LSM, RCM, SEC (server event logs) or RCONNECT (done through this program)
    QString event;     // see AccessLog::events()
    QString user;      // account that logged on (or, for RCONNECT lines, the one used on the server)
    QString address;   // client IP address
    QString client;    // client computer name
    QString detail;    // logon type, failure reason, session name, ...
    QString operatorName; // RCONNECT lines: the Windows user who ran this program
    int sessionId = 0;
    qint64 recordId = 0;
};

// Who logged on to, failed to log on to, left or watched a server, kept next to servers.json in
// access-log.csv. Most lines come from the servers' own event logs (see AccessEvent), so they also
// cover people who did not use RConnect. Lines with source RCONNECT record what was done through
// this program and by which Windows user. The file only grows; it is never rotated.
namespace AccessLog {

QString path();

// Event names as written to the file, in the order the viewer lists them.
const QStringList &events();

// Where the next check of the server should continue reading, by log (WinRm::eventSources()).
QHash<QString, EventCursor> cursors(const QString &serverId);

// A check of the server succeeded: writes the events that are new and moves the cursors.
// Returns how many lines were added.
// `live` are the sessions the server has right now: they give the computer name of people who
// signed in without RConnect, which the event logs rarely carry.
// `written` (optional) receives the new lines as records, oldest first per log.
int polled(const ServerProfile &server, const QList<AccessEvent> &events, const QList<RemoteSession> &live,
           QList<AccessRecord> *written = nullptr);

// Records something done through RConnect, e.g. "Connect" or "Monitor", for the Windows user running it.
void note(const ServerProfile &server, const QString &event, const QString &user, const QString &detail = {},
          const QString &address = {});

// Records a warning RConnect worked out from the events (see Threats), e.g. "AlertBruteForce".
// `timeUtc` is the time of the event that caused it.
void alert(const ServerProfile &server, const QString &timeUtc, const QString &event, const QString &user,
           const QString &address, const QString &detail);

QList<AccessRecord> readAll();

// Only the end of the file, as much as fits in `maxBytes`: enough for the viewer however long the
// log has grown.
QList<AccessRecord> readRecent(qint64 maxBytes);

// The session events of the servers' own session log (sign-in, sign-out, disconnect, reconnect) of the
// whole file, oldest first. Lines of other kinds are skipped before they are parsed, so this is quick even
// when the file holds hundreds of thousands of failed sign-ins.
QList<AccessRecord> readSessionEvents();

} // namespace AccessLog
