#pragma once
#include <QDate>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimeZone>

// Turns session events and quser samples into visits and per-day totals. Pure code: no files, no
// network, same input gives the same output.
//
// Events come from the server's session log (21 sign-in, 23 sign-out, 24 disconnect, 25 reconnect)
// and are exact. Samples are what quser said at a check: connected or not, and idle whole minutes.
// All times are UTC ms.
//
// Rules:
//  - A segment is one connection, from a sign-in/reconnect to the next disconnect/sign-out.
//    Disconnected time is never counted.
//  - Segments of one session that follow each other within kMergeGapSec, from the same IP (and the
//    same computer name when both are known), form one visit. The gap is not counted.
//  - A visit without a computer name takes the one name that every named visit from the same IP address
//    on that server agrees on; if the names differ, it stays unnamed.
//  - Away (AFK): idle for kAfkMinutes or more. A sample at time t with idle m minutes proves the
//    stretch [t - m min - 30 s, t] (quser rounds down, so the middle of the minute is used). The
//    whole stretch counts as away, clipped to the segment start.
//  - Unknown: parts of a segment with no sample within kSampleGapSec, unless a later idle reading
//    proves they were away. Never guessed.
//  - No end event: the segment ends at its last sample that saw it connected. No start event: it
//    starts at its first such sample (a lower bound). A connected quser session with no event at
//    all becomes its own "inferred" segment.
//
// Error margin: idle has whole-minute precision and checks are ~2 minutes apart, so an away stretch
// can be off by about 2 minutes at each end (usually too short). Event times are exact.
namespace UsageEngine {

constexpr int kAfkMinutes = 5;     // idle this long or more is away
constexpr int kMergeGapSec = 300;  // reconnect within this is the same visit
constexpr int kSampleGapSec = 330; // samples further apart leave the time between unknown
constexpr int kIdleMiddleSec = 30; // middle of the minute quser rounds idle down to

struct Event {
    enum Kind { Logon, Logoff, Disconnect, Reconnect };
    qint64 ms = 0;      // ms since epoch, UTC
    Kind kind = Logon;
    QString serverId;
    QString server;
    int sessionId = 0;
    QString user;
    QString address;    // client IP address
    qint64 recordId = 0;
};

struct Sample {
    // Heartbeat: a check worked, no session meant. Shows RConnect was watching.
    enum State { Connected, Disconnected, Gone, Heartbeat };
    qint64 ms = 0;      // server clock when known
    QString serverId;
    QString server;
    int sessionId = 0;
    QString user;
    State state = Connected;
    int idleMin = 0;
    QString client;     // computer the session was connected from, at that moment
};

enum class Kind { Active, Afk, Unknown };

// A stretch of a segment with one kind.
struct Part {
    qint64 start = 0, end = 0;
    Kind kind = Kind::Active;
};

inline qint64 duration(const QList<Part> &parts, Kind kind)
{
    qint64 sum = 0;
    for (const Part &p : parts)
        if (p.kind == kind)
            sum += p.end - p.start;
    return sum;
}

struct Segment {
    qint64 start = 0, end = 0;
    bool startKnown = true;   // false: no start event, start is the first sample that saw it
    bool endKnown = true;     // false: no end event, end is the last sample that saw it
    bool open = false;        // still connected, end is "now"
    bool inferred = false;    // not in the event log, only quser shows it
    QString client;
    QList<Part> parts;        // cover start..end without gaps
};

struct Visit {
    QString serverId, server, user, address, client;
    int sessionId = 0;
    QList<Segment> segments;
    qint64 start() const { return segments.isEmpty() ? 0 : segments.first().start; }
    qint64 end() const { return segments.isEmpty() ? 0 : segments.last().end; }
    bool open() const { return !segments.isEmpty() && segments.last().open; }
    bool startKnown() const { return !segments.isEmpty() && segments.first().startKnown; }
    bool endKnown() const { return !segments.isEmpty() && segments.last().endKnown; }
    bool inferred() const;
    int joined() const { return int(segments.size()) - 1; } // short drops merged in
};

// The part of a visit in one local day.
struct DayVisit {
    int visit = 0;            // index into Report::visits
    qint64 start = 0, end = 0;
    QList<Part> parts;        // only this day's parts of the connected stretches
    qint64 activeMs = 0, afkMs = 0, unknownMs = 0;
    bool fromPreviousDay = false, intoNextDay = false;
    qint64 totalMs() const { return activeMs + afkMs + unknownMs; }
};

// One computer on one server on one day.
struct DayGroup {
    QDate day;
    QString serverId, server, client, address;
    QStringList users;
    int entries = 0;          // visits that began this day
    qint64 firstIn = 0, lastOut = 0;
    bool inside = false;      // still connected
    bool uncertain = false;   // some visit has no exact start or end
    qint64 activeMs = 0, afkMs = 0, unknownMs = 0;
    QList<DayVisit> visits;
    qint64 totalMs() const { return activeMs + afkMs + unknownMs; }
};

// quser and the event log disagree about a session.
struct Mismatch {
    QString serverId, server;
    int sessionId = 0;
    bool serverSaysConnected = false;
    qint64 sampleMs = 0;
};

struct Report {
    QList<Visit> visits;
    QList<DayGroup> days;     // newest day first
    QList<Mismatch> mismatches;
};

Report compute(const QList<Event> &events, const QList<Sample> &samples, qint64 nowMs, const QTimeZone &zone);

// Local day of a moment, and the start of the next one.
QDate dayOf(qint64 ms, const QTimeZone &zone);
qint64 nextDayStart(qint64 ms, const QTimeZone &zone);

// "SERVER\Administrator" -> "Administrator".
QString accountOf(const QString &user);

} // namespace UsageEngine
