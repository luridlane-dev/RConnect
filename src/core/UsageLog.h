#pragma once
#include "AccessLog.h"
#include "UsageEngine.h"
#include "WinRm.h"

#include <QList>
#include <QString>

struct ServerProfile;

// Stores the facts behind "time on servers"; UsageEngine turns them into visits.
//
//  - usage-events.csv: session events (sign-in, sign-out, disconnect, reconnect), UTC, copied from
//    access-log.csv. Can be rebuilt from it at any time.
//  - usage-samples.csv: what quser said at each check, with idle minutes. Connected sessions on every
//    check, others when their state changes. Server clock, UTC.
//
// Only what was read is written, nothing is estimated. Both files only grow.
namespace UsageLog {

QString eventsPath();
QString samplesPath();

// A check succeeded. `live` are the current sessions, `serverNowUtcMs` the server's clock (0: use this
// PC's) and `written` the access log lines the check added.
void sampled(const ServerProfile &server, const QList<RemoteSession> &live, qint64 serverNowUtcMs,
             const QList<AccessRecord> &written);

// Rebuilds usage-events.csv from access-log.csv (old file kept as .bak). Returns the event count.
int rebuildEvents();

// Visits and days from everything stored, cut at this PC's midnight.
UsageEngine::Report report();

} // namespace UsageLog
