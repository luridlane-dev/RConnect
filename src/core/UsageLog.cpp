#include "UsageLog.h"

#include "model/ServerProfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>

#include <algorithm>

namespace {

using UsageEngine::Event;
using UsageEngine::Sample;

QMutex g_mutex;
bool g_loaded = false;
QList<Event> g_events;
QSet<QString> g_eventKeys;  // server|record|time|kind
QList<Sample> g_samples;

struct Last {
    Sample::State state = Sample::Gone;
    qint64 wroteMs = 0;
};
QHash<QString, Last> g_last; // "server id|session id" -> last line written
QHash<QString, qint64> g_beat; // server id -> last line written
QHash<QString, qint64> g_clockOffset; // server id -> serverNowUtcMs - localNowMs

// Unchanged disconnected sessions, and servers with no lines, are written again after this long,
// so the file shows when RConnect was watching.
constexpr qint64 kDisconnectedHeartbeatMs = 30 * 60 * 1000;
constexpr qint64 kHeartbeatMs = 5 * 60 * 1000;

const char *const kEventsHeader = "time_utc,server_id,server,event,user,address,session_id,record_id,client\r\n";
const char *const kSamplesHeader = "time_utc,server_id,server,session_id,user,state,idle_min,client\r\n";

QString dir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString quoted(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    if (!text.isEmpty() && QStringLiteral("=+-@\t").contains(text.at(0)))
        text.prepend(QLatin1Char('\''));
    return QLatin1Char('"') + text + QLatin1Char('"');
}

QStringList splitLine(QStringView text)
{
    QStringList fields;
    fields.reserve(9);
    QString field;
    bool inQuotes = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                field += c;
                ++i;
            } else if (c == QLatin1Char('"')) {
                inQuotes = false;
            } else {
                field += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            fields << field;
            field.clear();
        } else {
            field += c;
        }
    }
    fields << field;
    return fields;
}

// Fast zero-allocation ISO-8601 UTC parser ("2026-11-01T17:34:22.4134708Z" -> ms since epoch UTC; 0 if invalid).
qint64 parseUtcMs(QStringView s)
{
    if (s.size() < 19)
        return 0;
    const auto dig2 = [&](qsizetype i, int &out) {
        const ushort c0 = s.at(i).unicode() - '0';
        const ushort c1 = s.at(i + 1).unicode() - '0';
        if (c0 > 9 || c1 > 9)
            return false;
        out = c0 * 10 + c1;
        return true;
    };
    if (s.at(4) != QLatin1Char('-') || s.at(7) != QLatin1Char('-')
        || (s.at(10) != QLatin1Char('T') && s.at(10) != QLatin1Char(' '))
        || s.at(13) != QLatin1Char(':') || s.at(16) != QLatin1Char(':'))
        return 0;
    int yHi = 0, yLo = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!dig2(0, yHi) || !dig2(2, yLo) || !dig2(5, mo) || !dig2(8, d)
        || !dig2(11, h) || !dig2(14, mi) || !dig2(17, se))
        return 0;
    const int y = yHi * 100 + yLo;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59)
        return 0;
    int ms = 0;
    if (s.size() > 19 && s.at(19) == QLatin1Char('.')) {
        int mul = 100;
        for (qsizetype i = 20; i < s.size() && mul >= 1; ++i) {
            const ushort c = s.at(i).unicode() - '0';
            if (c > 9)
                break;
            ms += int(c) * mul;
            mul /= 10;
        }
    }
    const qint64 yr = y - (mo <= 2 ? 1 : 0);
    const qint64 era = (yr >= 0 ? yr : yr - 399) / 400;
    const qint64 yoe = yr - era * 400;
    const qint64 doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const qint64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const qint64 days = era * 146097 + doe - 719468;
    return ((days * 86400 + qint64(h) * 3600 + qint64(mi) * 60 + se) * 1000) + ms;
}

QString isoUtc(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) + QLatin1Char('Z');
}

bool kindOf(const QString &name, Event::Kind &kind)
{
    if (name == QLatin1String("Logon"))
        kind = Event::Logon;
    else if (name == QLatin1String("Logoff"))
        kind = Event::Logoff;
    else if (name == QLatin1String("Disconnect"))
        kind = Event::Disconnect;
    else if (name == QLatin1String("Reconnect"))
        kind = Event::Reconnect;
    else
        return false;
    return true;
}

QString nameOf(Event::Kind kind)
{
    switch (kind) {
    case Event::Logon:      return QStringLiteral("Logon");
    case Event::Logoff:     return QStringLiteral("Logoff");
    case Event::Disconnect: return QStringLiteral("Disconnect");
    case Event::Reconnect:  return QStringLiteral("Reconnect");
    }
    return {};
}

QString eventKey(const QString &serverId, qint64 recordId, const QString &timeUtc, const QString &name)
{
    return serverId + QLatin1Char('|') + QString::number(recordId) + QLatin1Char('|') + timeUtc + QLatin1Char('|') + name;
}

struct ClientHint {
    qint64 ms = 0;
    QString address, account, client;
};
using ClientHintIndex = QHash<QString, QList<ClientHint>>;

// Collects real client computer names recorded by Windows Security log (4778/4779/4624),
// indexed by serverId and sorted by timestamp for O(log M) binary search lookup.
ClientHintIndex clientHintsFrom(const QList<AccessRecord> &records)
{
    ClientHintIndex hints;
    for (const AccessRecord &r : records) {
        if (r.source != QLatin1String("SEC") || r.client.isEmpty())
            continue;
        if (r.event != QLatin1String("SecurityReconnect") && r.event != QLatin1String("SecurityDisconnect")
            && r.event != QLatin1String("SecurityLogon"))
            continue;
        // 4624 LogonType 10 sometimes writes the server's own hostname as WorkstationName.
        const qsizetype slash = r.user.lastIndexOf(QLatin1Char('\\'));
        if (slash > 0 && r.client.compare(QStringView(r.user).left(slash), Qt::CaseInsensitive) == 0)
            continue;
        const qint64 ms = parseUtcMs(r.timeUtc);
        if (ms == 0)
            continue;
        hints[r.serverId] << ClientHint{ms, r.address, UsageEngine::accountOf(r.user), r.client};
    }
    for (auto it = hints.begin(); it != hints.end(); ++it) {
        std::sort(it.value().begin(), it.value().end(),
                  [](const ClientHint &a, const ClientHint &b) { return a.ms < b.ms; });
    }
    return hints;
}

QString matchClientHint(const Event &e, const ClientHintIndex &hintsByServer)
{
    const auto serverIt = hintsByServer.constFind(e.serverId);
    if (serverIt == hintsByServer.cend() || serverIt.value().isEmpty())
        return {};
    const QList<ClientHint> &hints = serverIt.value();
    const QString account = UsageEngine::accountOf(e.user);
    constexpr qint64 kWindowMs = 15000;
    qint64 bestDiff = kWindowMs + 1;
    QString best;
    auto it = std::lower_bound(hints.cbegin(), hints.cend(), e.ms - kWindowMs,
                               [](const ClientHint &h, qint64 t) { return h.ms < t; });
    for (; it != hints.cend() && it->ms <= e.ms + kWindowMs; ++it) {
        const ClientHint &h = *it;
        if (!e.address.isEmpty() && !h.address.isEmpty() && h.address != e.address)
            continue;
        if (!account.isEmpty() && !h.account.isEmpty() && h.account.compare(account, Qt::CaseInsensitive) != 0)
            continue;
        const qint64 diff = qAbs(h.ms - e.ms);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = h.client;
        }
    }
    return best;
}

// Converts an access log record; false if it is not a session event.
bool toEvent(const AccessRecord &r, const ClientHintIndex &hints, Event &e, QByteArray &line, QString &key)
{
    Event::Kind kind;
    if (r.source != QLatin1String("LSM") || !kindOf(r.event, kind))
        return false;
    e.ms = parseUtcMs(r.timeUtc);
    if (e.ms == 0)
        return false;
    e.kind = kind;
    e.serverId = r.serverId;
    e.server = r.server;
    e.sessionId = r.sessionId;
    e.user = r.user;
    e.address = r.address;
    e.recordId = r.recordId;
    e.client = matchClientHint(e, hints);
    key = eventKey(r.serverId, r.recordId, r.timeUtc, r.event);
    const QStringList fields = {quoted(r.timeUtc), quoted(r.serverId), quoted(r.server), quoted(r.event), quoted(r.user),
                                quoted(r.address), QString::number(r.sessionId), QString::number(r.recordId),
                                quoted(e.client)};
    line = fields.join(QLatin1Char(',')).toUtf8() + "\r\n";
    return true;
}

// Caller holds the mutex.
void append(const QString &path, const char *header, const QByteArray &lines)
{
    if (lines.isEmpty())
        return;
    QDir().mkpath(dir());
    QFile f(path);
    const bool isNew = !f.exists() || f.size() == 0;
    if (!f.open(QIODevice::Append))
        return;
    if (isNew)
        f.write(header);
    f.write(lines);
}

// Caller holds the mutex. Returns the event count.
int rebuildLocked()
{
    const QList<AccessRecord> records = AccessLog::readSessionEvents();
    QDir().mkpath(dir());
    if (QFile::exists(UsageLog::eventsPath())) {
        QFile::remove(UsageLog::eventsPath() + QStringLiteral(".bak"));
        QFile::copy(UsageLog::eventsPath(), UsageLog::eventsPath() + QStringLiteral(".bak"));
    }
    const ClientHintIndex hints = clientHintsFrom(records);
    QList<Event> events;
    QSet<QString> keys;
    QByteArray lines;
    for (const AccessRecord &r : records) {
        Event e;
        QByteArray line;
        QString key;
        if (!toEvent(r, hints, e, line, key) || keys.contains(key))
            continue;
        keys.insert(key);
        events << e;
        lines += line;
    }
    QSaveFile f(UsageLog::eventsPath());
    const QByteArray header(kEventsHeader);
    if (!f.open(QIODevice::WriteOnly) || f.write(header) != header.size() || f.write(lines) != lines.size() || !f.commit())
        return -1;
    g_events = std::move(events);
    g_eventKeys = std::move(keys);
    return int(g_events.size());
}

// Caller holds the mutex.
void load()
{
    if (g_loaded)
        return;
    g_loaded = true;

    if (!QFile::exists(UsageLog::eventsPath()))
        rebuildLocked(); // first run: take the events from the access log
    else {
        QFile f(UsageLog::eventsPath());
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            bool header = true;
            while (!f.atEnd()) {
                const QString text = QString::fromUtf8(f.readLine()).trimmed();
                if (header) {
                    header = false;
                    continue;
                }
                const QStringList c = splitLine(text);
                Event e;
                if (c.size() < 8 || !kindOf(c.at(3), e.kind))
                    continue;
                e.ms = parseUtcMs(c.at(0));
                if (e.ms == 0)
                    continue;
                e.serverId = c.at(1);
                e.server = c.at(2);
                e.user = c.at(4);
                e.address = c.at(5);
                e.sessionId = c.at(6).toInt();
                e.recordId = c.at(7).toLongLong();
                if (c.size() >= 9)
                    e.client = c.at(8);
                const QString key = eventKey(e.serverId, e.recordId, c.at(0), c.at(3));
                if (g_eventKeys.contains(key))
                    continue;
                g_eventKeys.insert(key);
                g_events << e;
            }
        }
    }

    QFile f(UsageLog::samplesPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool header = true;
        while (!f.atEnd()) {
            const QString text = QString::fromUtf8(f.readLine()).trimmed();
            if (header) {
                header = false;
                continue;
            }
            const QStringList c = splitLine(text);
            if (c.size() < 8)
                continue;
            Sample s;
            s.ms = parseUtcMs(c.at(0));
            if (s.ms == 0)
                continue;
            s.serverId = c.at(1);
            s.server = c.at(2);
            s.sessionId = c.at(3).toInt();
            s.user = c.at(4);
            const QString state = c.at(5);
            s.state = state == QLatin1String("C") ? Sample::Connected
                : state == QLatin1String("D")     ? Sample::Disconnected
                : state == QLatin1String("H")     ? Sample::Heartbeat
                                                  : Sample::Gone;
            bool idleKnown = false;
            s.idleMin = c.at(6).toInt(&idleKnown);
            if (!idleKnown)
                s.idleMin = -1;
            s.client = c.at(7);
            g_samples << s;
            if (s.ms > g_beat.value(s.serverId))
                g_beat[s.serverId] = s.ms;
            if (s.state != Sample::Heartbeat) {
                const QString key = s.serverId + QLatin1Char('|') + QString::number(s.sessionId);
                Last &last = g_last[key];
                if (s.ms >= last.wroteMs) {
                    last.state = s.state;
                    last.wroteMs = s.ms;
                }
            }
        }
    }
}

} // namespace

QString UsageLog::eventsPath()
{
    return dir() + QStringLiteral("/usage-events.csv");
}

QString UsageLog::samplesPath()
{
    return dir() + QStringLiteral("/usage-samples.csv");
}

void UsageLog::recordEvents(const QList<AccessRecord> &written)
{
    QMutexLocker lock(&g_mutex);
    load();
    const ClientHintIndex hints = clientHintsFrom(written);
    QByteArray eventLines;
    for (const AccessRecord &r : written) {
        Event e;
        QByteArray line;
        QString key;
        if (!toEvent(r, hints, e, line, key) || g_eventKeys.contains(key))
            continue;
        g_eventKeys.insert(key);
        g_events << e;
        eventLines += line;
    }
    append(eventsPath(), kEventsHeader, eventLines);
}

void UsageLog::sampled(const ServerProfile &server, const QList<RemoteSession> &live, qint64 serverNowUtcMs)
{
    QMutexLocker lock(&g_mutex);
    load();
    const qint64 localNow = QDateTime::currentMSecsSinceEpoch();
    if (serverNowUtcMs > 0)
        g_clockOffset.insert(server.id, serverNowUtcMs - localNow);
    const qint64 now = serverNowUtcMs > 0
        ? serverNowUtcMs
        : (g_clockOffset.contains(server.id) ? localNow + g_clockOffset.value(server.id) : localNow);
    if (now <= g_beat.value(server.id))
        return;

    QByteArray sampleLines;
    auto add = [&](int sessionId, const QString &user, Sample::State state, int idle, const QString &client) {
        Sample s;
        s.ms = now;
        s.serverId = server.id;
        s.server = server.name;
        s.sessionId = sessionId;
        s.user = user;
        s.state = state;
        s.idleMin = idle;
        s.client = client;
        g_samples << s;
        const QString code = state == Sample::Connected ? QStringLiteral("C")
            : state == Sample::Disconnected             ? QStringLiteral("D")
            : state == Sample::Heartbeat                ? QStringLiteral("H")
                                                        : QStringLiteral("G");
        const QStringList fields = {quoted(isoUtc(now)), quoted(server.id), quoted(server.name), QString::number(sessionId),
                                    quoted(user), quoted(code), QString::number(idle), quoted(client)};
        sampleLines += fields.join(QLatin1Char(',')).toUtf8() + "\r\n";
    };

    QSet<QString> present;
    for (const RemoteSession &s : live) {
        const QString key = server.id + QLatin1Char('|') + QString::number(s.id);
        present.insert(key);
        const Sample::State state = s.connected ? Sample::Connected : Sample::Disconnected;
        Last &last = g_last[key];
        if (state == Sample::Connected || last.state != state || now - last.wroteMs >= kDisconnectedHeartbeatMs) {
            add(s.id, s.user, state, s.idleMinutes, s.client);
            last.wroteMs = now;
        }
        last.state = state;
    }
    // Sessions seen at the last check but not now.
    const QString prefix = server.id + QLatin1Char('|');
    for (auto it = g_last.begin(); it != g_last.end(); ++it) {
        if (!it.key().startsWith(prefix) || present.contains(it.key()) || it.value().state == Sample::Gone)
            continue;
        add(it.key().mid(prefix.size()).toInt(), {}, Sample::Gone, 0, {});
        it.value().state = Sample::Gone;
        it.value().wroteMs = now;
    }
    if (sampleLines.isEmpty() && now - g_beat.value(server.id) >= kHeartbeatMs)
        add(-1, {}, Sample::Heartbeat, 0, {});
    if (!sampleLines.isEmpty())
        g_beat[server.id] = now;
    append(samplesPath(), kSamplesHeader, sampleLines);
}

int UsageLog::rebuildEvents()
{
    QMutexLocker lock(&g_mutex);
    load();
    return rebuildLocked();
}

UsageEngine::Report UsageLog::report()
{
    QList<Event> events;
    QList<Sample> samples;
    QHash<QString, qint64> offsets;
    {
        QMutexLocker lock(&g_mutex);
        load();
        events = g_events;
        samples = g_samples;
        offsets = g_clockOffset;
    }
    const qint64 localNow = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, qint64> serverNow;
    for (auto it = offsets.cbegin(); it != offsets.cend(); ++it)
        serverNow.insert(it.key(), localNow + it.value());
    return UsageEngine::compute(events, samples, localNow, QTimeZone::systemTimeZone(), serverNow);
}
