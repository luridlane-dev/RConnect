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

QStringList splitLine(const QString &text)
{
    QStringList fields;
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

// "2026-11-01T17:34:22.4134708Z" -> ms since epoch (UTC); 0 if it is not a time.
qint64 parseUtcMs(const QString &text)
{
    const QDateTime t = QDateTime::fromString(text.left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
    return t.isValid() ? t.toMSecsSinceEpoch() : 0;
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
    QString serverId, address, account, client;
};

// Collects real client computer names recorded by Windows Security log (4778/4779/4624).
QList<ClientHint> clientHintsFrom(const QList<AccessRecord> &records)
{
    QList<ClientHint> hints;
    for (const AccessRecord &r : records) {
        if (r.source != QLatin1String("SEC") || r.client.isEmpty())
            continue;
        if (r.event != QLatin1String("SecurityReconnect") && r.event != QLatin1String("SecurityDisconnect")
            && r.event != QLatin1String("SecurityLogon"))
            continue;
        // 4624 LogonType 10 sometimes writes the server's own hostname as WorkstationName.
        const qsizetype slash = r.user.lastIndexOf(QLatin1Char('\\'));
        if (slash > 0 && r.client.compare(r.user.left(slash), Qt::CaseInsensitive) == 0)
            continue;
        const qint64 ms = parseUtcMs(r.timeUtc);
        if (ms == 0)
            continue;
        hints << ClientHint{ms, r.serverId, r.address, UsageEngine::accountOf(r.user), r.client};
    }
    return hints;
}

QString matchClientHint(const Event &e, const QList<ClientHint> &hints)
{
    if (hints.isEmpty())
        return {};
    const QString account = UsageEngine::accountOf(e.user);
    qint64 bestDiff = 15001;
    QString best;
    for (const ClientHint &h : hints) {
        if (h.serverId != e.serverId)
            continue;
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
bool toEvent(const AccessRecord &r, const QList<ClientHint> &hints, Event &e, QByteArray &line, QString &key)
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
    const QList<ClientHint> hints = clientHintsFrom(records);
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
    const QList<ClientHint> hints = clientHintsFrom(written);
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
    const qint64 now = serverNowUtcMs > 0 ? serverNowUtcMs : QDateTime::currentMSecsSinceEpoch();
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
    {
        QMutexLocker lock(&g_mutex);
        load();
        events = g_events;
        samples = g_samples;
    }
    return UsageEngine::compute(events, samples, QDateTime::currentMSecsSinceEpoch(), QTimeZone::systemTimeZone());
}
