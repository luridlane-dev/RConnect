#include "AccessLog.h"

#include "model/ServerProfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMultiHash>
#include <QMutex>
#include <QMutexLocker>
#include <QAtomicInt>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>

#include <functional>

namespace {

QMutex g_mutex;
QHash<QString, QString> g_lastEvent; // "server id|source" -> time of the newest line written
QHash<QString, QSet<qint64>> g_atLastEvent; // ... and the record ids written with exactly that time
QHash<QString, QString> g_clientByAddress; // client IP -> computer name last seen with it
QHash<QString, qint64> g_checkedAt;  // "server id|source" -> ms since epoch of the last successful check
bool g_loaded = false;

// A source with nothing written yet is read this far back from the previous check, so it does not
// ask for 7 days every minute. Covers the time a check takes.
constexpr qint64 kLookbackMarginMs = 120000;

// Computer names remembered by address; a flood of addresses from the internet must not grow it forever.
constexpr qsizetype kMaxKnownClients = 20000;
// Failed sign-ins from one address in one check: this many lines are written, the rest become one summary line.
constexpr int kMaxFailedLinesPerAddress = 3;
// Failed sign-ins this close together from one address are one run.
constexpr qint64 kRunGapSecs = 600;
constexpr int kKeepFailedDays = 30;
constexpr int kKeepOtherDays = 400;

// The summary line of a folded run has "|more=N" after its detail: N attempts besides the line itself.
const char *const kMoreMarker = "|more=";

const char *const kHeader =
    "time_utc,time_local,server_id,server,host,source,event,user,address,client,detail,operator,session_id,record_id\r\n";

QString key(const QString &serverId, const QString &source)
{
    return serverId + QLatin1Char('|') + source;
}

QString eventName(AccessEvent::Kind kind)
{
    switch (kind) {
    case AccessEvent::Logon:              return QStringLiteral("Logon");
    case AccessEvent::Logoff:             return QStringLiteral("Logoff");
    case AccessEvent::Disconnect:         return QStringLiteral("Disconnect");
    case AccessEvent::Reconnect:          return QStringLiteral("Reconnect");
    case AccessEvent::Authenticated:      return QStringLiteral("Authenticated");
    case AccessEvent::SecurityLogon:      return QStringLiteral("SecurityLogon");
    case AccessEvent::FailedLogon:        return QStringLiteral("FailedLogon");
    case AccessEvent::SecurityReconnect:  return QStringLiteral("SecurityReconnect");
    case AccessEvent::SecurityDisconnect: return QStringLiteral("SecurityDisconnect");
    case AccessEvent::UserCreated:        return QStringLiteral("UserCreated");
    case AccessEvent::GroupMemberAdded:   return QStringLiteral("GroupMemberAdded");
    case AccessEvent::TaskCreated:        return QStringLiteral("TaskCreated");
    case AccessEvent::LogCleared:
    case AccessEvent::SystemLogCleared:   return QStringLiteral("LogCleared");
    case AccessEvent::ServiceInstalled:   return QStringLiteral("ServiceInstalled");
    }
    return {};
}

QString quoted(QString text)
{
    text.replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    // Names come from the network (failed logons): keep a spreadsheet from running them as a formula.
    if (!text.isEmpty() && QStringLiteral("=+-@\t").contains(text.at(0)))
        text.prepend(QLatin1Char('\''));
    return QLatin1Char('"') + text + QLatin1Char('"');
}

// Within the last ten minutes: new enough for a session number to still mean the same session.
bool recent(const QString &timeUtc)
{
    const QDateTime utc = QDateTime::fromString(timeUtc.left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
    return utc.isValid() && qAbs(utc.secsTo(QDateTime::currentDateTimeUtc())) <= 600;
}

QString localTime(const QString &timeUtc)
{
    const QDateTime utc = QDateTime::fromString(timeUtc.left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
    return utc.isValid() ? utc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString();
}

// One CSV line without its line break; fields may be quoted.
QStringList splitLine(const QString &line)
{
    QStringList fields;
    QString field;
    bool inQuotes = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"') && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
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

enum class Scan { All, SessionEvents, WithoutFailed };

// Passes every line of the file to `each`, one at a time. `legacy` is set when the file has the first version's
// ten columns (all from the LocalSessionManager log).
void scanFile(const std::function<void(AccessRecord &&)> &each, bool *legacy = nullptr, qint64 maxBytes = 0,
              Scan mode = Scan::All)
{
    QFile f(AccessLog::path());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    bool header = true;
    bool oldFormat = false;
    if (maxBytes > 0 && f.size() > maxBytes) {
        f.seek(f.size() - maxBytes);
        f.readLine(); // the line cut in half
        header = false;
    }
    while (!f.atEnd()) {
        const QByteArray raw = f.readLine();
        // Cheap test on the bytes first: the other sources are not session events.
        if (mode == Scan::SessionEvents && !header && !oldFormat && !raw.contains("\"LSM\""))
            continue;
        if (mode == Scan::WithoutFailed && !header && !oldFormat && raw.contains("\"FailedLogon\""))
            continue;
        const QString line = QString::fromUtf8(raw).trimmed();
        if (header) {
            header = false;
            oldFormat = line.startsWith(QLatin1String("time_utc,time_local,server_id,server,host,event,"));
            if (legacy)
                *legacy = oldFormat;
            continue;
        }
        const QStringList c = splitLine(line);
        // The first version's file may also hold lines of the current format below its header.
        if (oldFormat && c.size() >= 10 && c.size() < 14) {
            AccessRecord r;
            r.timeUtc = c.at(0);
            r.serverId = c.at(2);
            r.server = c.at(3);
            r.host = c.at(4);
            r.event = c.at(5);
            // RConnect's own notes are not events of the server's log.
            r.source = r.event == QLatin1String("Connect") || r.event == QLatin1String("Monitor")
                               || r.event == QLatin1String("AuditEnable")
                           ? QStringLiteral("RCONNECT")
                           : QStringLiteral("LSM");
            r.user = c.at(6);
            r.address = c.at(7);
            r.sessionId = c.at(8).toInt();
            r.recordId = c.at(9).toLongLong();
            each(std::move(r));
            continue;
        }
        if (c.size() < 14)
            continue;
        AccessRecord r;
        r.timeUtc = c.at(0);
        r.serverId = c.at(2);
        r.server = c.at(3);
        r.host = c.at(4);
        r.source = c.at(5);
        r.event = c.at(6);
        r.user = c.at(7);
        r.address = c.at(8);
        r.client = c.at(9);
        r.detail = c.at(10);
        r.operatorName = c.at(11);
        r.sessionId = c.at(12).toInt();
        r.recordId = c.at(13).toLongLong();
        each(std::move(r));
    }
}

QList<AccessRecord> readFile(bool *legacy = nullptr, qint64 maxBytes = 0, bool sessionEventsOnly = false)
{
    QList<AccessRecord> records;
    scanFile([&records](AccessRecord &&r) { records << std::move(r); }, legacy, maxBytes,
             sessionEventsOnly ? Scan::SessionEvents : Scan::All);
    return records;
}

// The first line of the file says whether it is from the first version.
bool fileIsLegacy()
{
    QFile f(AccessLog::path());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    return QString::fromUtf8(f.readLine()).startsWith(QLatin1String("time_utc,time_local,server_id,server,host,event,"));
}

QByteArray line(const ServerProfile &server, const QString &timeUtc, const QString &source, const QString &event,
                const QString &user, const QString &address, const QString &client, const QString &detail,
                const QString &operatorName, int sessionId, qint64 recordId);

// A file from the first version is converted once, so the new lines below it fit the header.
void migrateLegacyFile(const QList<AccessRecord> &records)
{
    QFile::copy(AccessLog::path(), AccessLog::path() + QStringLiteral(".v1.bak")); // kept, in case
    QSaveFile f(AccessLog::path());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(kHeader);
    for (const AccessRecord &r : records) {
        ServerProfile p;
        p.id = r.serverId;
        p.name = r.server;
        p.host = r.host;
        f.write(line(p, r.timeUtc, r.source, r.event, r.user, r.address, r.client, r.detail, r.operatorName,
                     r.sessionId, r.recordId));
    }
    f.commit();
}

void loadLastEvents()
{
    if (g_loaded)
        return;
    g_loaded = true;
    const bool legacy = fileIsLegacy();
    QList<AccessRecord> records;
    if (legacy) {
        records = readFile();
        // The first version could read the same events again once the format changed.
        QSet<QString> seen;
        QList<AccessRecord> unique;
        for (const AccessRecord &r : std::as_const(records)) {
            const QString id = key(r.serverId, r.source) + QLatin1Char('|') + r.timeUtc + QLatin1Char('|') + r.event
                               + QLatin1Char('|') + QString::number(r.recordId);
            if (r.source != QLatin1String("RCONNECT") && seen.contains(id))
                continue;
            seen.insert(id);
            unique << r;
        }
        records = unique;
        migrateLegacyFile(records);
    }
    // Streamed: the file can be far larger than is sensible to hold in memory.
    const auto learn = [](const AccessRecord &r) {
        if (r.source == QLatin1String("RCONNECT"))
            return;
        if (!r.client.isEmpty() && !r.address.isEmpty() && g_clientByAddress.size() < kMaxKnownClients)
            g_clientByAddress.insert(r.address, r.client);
        const QString k = key(r.serverId, r.source);
        QString &last = g_lastEvent[k];
        if (r.timeUtc > last) {
            last = r.timeUtc;
            g_atLastEvent[k].clear();
        }
        if (r.timeUtc == last)
            g_atLastEvent[k].insert(r.recordId);
    };
    if (legacy) {
        for (const AccessRecord &r : std::as_const(records))
            learn(r);
    } else {
        scanFile([&learn](AccessRecord &&r) { learn(r); });
    }
}

QByteArray line(const ServerProfile &server, const QString &timeUtc, const QString &source, const QString &event,
                const QString &user, const QString &address, const QString &client, const QString &detail,
                const QString &operatorName, int sessionId, qint64 recordId)
{
    const QStringList fields = {quoted(timeUtc), quoted(localTime(timeUtc)), quoted(server.id), quoted(server.name),
                                quoted(server.host), quoted(source), quoted(event), quoted(user), quoted(address),
                                quoted(client), quoted(detail), quoted(operatorName), QString::number(sessionId),
                                QString::number(recordId)};
    return fields.join(QLatin1Char(',')).toUtf8() + "\r\n";
}

// Caller holds the mutex.
bool write(const QByteArray &lines)
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    QFile f(AccessLog::path());
    const bool isNew = !f.exists() || f.size() == 0;
    if (!f.open(QIODevice::Append))
        return false;
    if (isNew)
        f.write(kHeader);
    f.write(lines);
    return true;
}

} // namespace

namespace {
QAtomicInt g_serverEvents(-1);
}

bool AccessLog::serverEventsEnabled()
{
    if (g_serverEvents.loadRelaxed() < 0)
        g_serverEvents.storeRelaxed(QSettings().value(QStringLiteral("log/serverEvents"), false).toBool() ? 1 : 0);
    return g_serverEvents.loadRelaxed() == 1;
}

void AccessLog::setServerEventsEnabled(bool on)
{
    g_serverEvents.storeRelaxed(on ? 1 : 0);
    QSettings().setValue(QStringLiteral("log/serverEvents"), on);
}

QString AccessLog::path()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/access-log.csv");
}

const QStringList &AccessLog::events()
{
    static const QStringList names = {
        QStringLiteral("Logon"),         QStringLiteral("Logoff"),          QStringLiteral("Disconnect"),
        QStringLiteral("Reconnect"),     QStringLiteral("Authenticated"),   QStringLiteral("SecurityLogon"),
        QStringLiteral("FailedLogon"),   QStringLiteral("SecurityReconnect"), QStringLiteral("SecurityDisconnect"),
        QStringLiteral("UserCreated"),   QStringLiteral("GroupMemberAdded"), QStringLiteral("TaskCreated"),
        QStringLiteral("LogCleared"),    QStringLiteral("ServiceInstalled"),
        QStringLiteral("Connect"),       QStringLiteral("Monitor"),         QStringLiteral("AuditEnable"),
        QStringLiteral("Message"),
        QStringLiteral("Closed"),        QStringLiteral("ServerAdded"),     QStringLiteral("ServerEdited"),
        QStringLiteral("ServerRemoved"), QStringLiteral("Import"),          QStringLiteral("Update"),
        QStringLiteral("AlertBruteForce"), QStringLiteral("AlertBreakIn"),  QStringLiteral("AlertNewCountry"),
        QStringLiteral("AlertSuspicious"),
    };
    return names;
}

QHash<QString, EventCursor> AccessLog::cursors(const QString &serverId)
{
    QMutexLocker lock(&g_mutex);
    loadLastEvents();
    QHash<QString, EventCursor> result;
    for (const QString &source : WinRm::eventSources()) {
        EventCursor c;
        c.afterUtc = g_lastEvent.value(key(serverId, source));
        const qint64 checked = g_checkedAt.value(key(serverId, source));
        if (c.afterUtc.isEmpty() && checked > 0)
            c.lookbackMs = QDateTime::currentMSecsSinceEpoch() - checked + kLookbackMarginMs;
        result.insert(source, c);
    }
    return result;
}

int AccessLog::polled(const ServerProfile &server, const QList<AccessEvent> &events, const QList<RemoteSession> &live,
                      QList<AccessRecord> *written)
{
    QMutexLocker lock(&g_mutex);
    loadLastEvents();

    QHash<QString, QString> newest;
    QHash<QString, QMultiHash<QString, qint64>> writtenIds; // by key: time -> record ids
    QList<AccessRecord> records;
    QByteArray lines;
    struct Folded { qint64 more = 0; QString timeUtc, user, detail, client; qint64 recordId = 0; };
    QHash<QString, int> failedKept;      // address -> failed sign-in lines written in this check
    QHash<QString, Folded> folded;       // address -> the ones left out
    int added = 0;
    for (const AccessEvent &e : events) {
        const QString k = key(server.id, e.source);
        // Already written, e.g. by an overlapping check. The query is inclusive, so the newest
        // event comes back and events sharing its time are told apart by record id.
        const QString last = g_lastEvent.value(k);
        if (e.timeUtc < last || (e.timeUtc == last && g_atLastEvent.value(k).contains(e.recordId)))
            continue;
        QString client = e.client;
        // A session that is still open tells the computer name of its own sign-in event.
        if (client.isEmpty() && e.sessionId > 0 && recent(e.timeUtc)) {
            const QString account = e.user.mid(e.user.lastIndexOf(QLatin1Char('\\')) + 1);
            for (const RemoteSession &s : live)
                if (s.id == e.sessionId && !s.client.isEmpty() && s.user.compare(account, Qt::CaseInsensitive) == 0) {
                    client = s.client;
                    break;
                }
        }
        // The same address is the same computer: what one event learned fills the others.
        if (client.isEmpty() && !e.address.isEmpty())
            client = g_clientByAddress.value(e.address);
        if (!client.isEmpty() && !e.address.isEmpty() && g_clientByAddress.size() < kMaxKnownClients)
            g_clientByAddress.insert(e.address, client);
        // Password guessing from the internet is thousands of failed sign-ins a day: only the first few of each
        // address are written, the rest are counted in one summary line (the alerts below still see them all).
        if (e.kind == AccessEvent::FailedLogon && failedKept[e.address]++ >= kMaxFailedLinesPerAddress) {
            Folded &f = folded[e.address];
            ++f.more;
            if (e.timeUtc >= f.timeUtc) {
                f.timeUtc = e.timeUtc;
                f.user = e.user;
                f.detail = e.detail;
                f.client = client;
                f.recordId = e.recordId;
            }
        } else {
            lines += line(server, e.timeUtc, e.source, eventName(e.kind), e.user, e.address, client, e.detail, {},
                          e.sessionId, e.recordId);
        }
        if (e.timeUtc > newest.value(k))
            newest.insert(k, e.timeUtc);
        writtenIds[k].insert(e.timeUtc, e.recordId);
        if (written) {
            AccessRecord r;
            r.timeUtc = e.timeUtc;
            r.serverId = server.id;
            r.server = server.name;
            r.host = server.host;
            r.source = e.source;
            r.event = eventName(e.kind);
            r.user = e.user;
            r.address = e.address;
            r.client = client;
            r.detail = e.detail;
            r.sessionId = e.sessionId;
            r.recordId = e.recordId;
            records << r;
        }
        ++added;
    }
    for (auto it = folded.cbegin(); it != folded.cend(); ++it) {
        // The line stands for itself and the others: N events in all, so N - 1 besides it.
        const Folded &f = it.value();
        lines += line(server, f.timeUtc, QStringLiteral("SEC"), QStringLiteral("FailedLogon"), f.user, it.key(), f.client,
                      f.detail + QLatin1String(kMoreMarker) + QString::number(f.more - 1), {}, 0, f.recordId);
    }
    if (added > 0 && !write(lines))
        return 0;
    if (written)
        *written = records;
    for (auto it = newest.cbegin(); it != newest.cend(); ++it) {
        if (it.value() > g_lastEvent.value(it.key()))
            g_atLastEvent[it.key()].clear();
        g_lastEvent.insert(it.key(), it.value());
        for (qint64 record : writtenIds.value(it.key()).values(it.value()))
            g_atLastEvent[it.key()].insert(record);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QString &source : WinRm::eventSources())
        g_checkedAt.insert(key(server.id, source), now);
    return added;
}

void AccessLog::note(const ServerProfile &server, const QString &event, const QString &user, const QString &detail,
                     const QString &address)
{
    QMutexLocker lock(&g_mutex);
    const QString timeUtc = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
                            + QStringLiteral("0000Z");
    const QString operatorName = qEnvironmentVariable("USERDOMAIN") + QLatin1Char('\\') + qEnvironmentVariable("USERNAME");
    write(line(server, timeUtc, QStringLiteral("RCONNECT"), event, user, address, qEnvironmentVariable("COMPUTERNAME"),
               detail, operatorName, 0, 0));
}

void AccessLog::alert(const ServerProfile &server, const QString &timeUtc, const QString &event, const QString &user,
                      const QString &address, const QString &detail)
{
    QMutexLocker lock(&g_mutex);
    write(line(server, timeUtc, QStringLiteral("RCONNECT"), event, user, address, {}, detail, {}, 0, 0));
}

void AccessLog::forEach(const std::function<void(const AccessRecord &)> &each, bool withoutFailedSignIns)
{
    QMutexLocker lock(&g_mutex);
    scanFile([&each](AccessRecord &&r) { each(r); }, nullptr, 0, withoutFailedSignIns ? Scan::WithoutFailed : Scan::All);
}

void AccessLog::maintain()
{
    QMutexLocker lock(&g_mutex);
    QFile in(path());
    constexpr qint64 kLookAtLeast = 2 * 1024 * 1024; // a small file needs nothing
    if (in.size() < kLookAtLeast || !in.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QSaveFile out(path());
    if (!out.open(QIODevice::WriteOnly))
        return;

    struct Run {
        QDateTime last;
        int kept = 0;
        qint64 more = 0;
        QStringList newest; // the fields of the newest folded line
    };
    QHash<QString, Run> runs; // server id | address
    const QDateTime now = QDateTime::currentDateTimeUtc();
    qint64 dropped = 0;

    const auto flush = [&](Run &run, const QString &address) {
        if (run.more <= 0)
            return;
        QStringList c = run.newest;
        c[8] = address;
        c[10] = c.at(10) + QLatin1String(kMoreMarker) + QString::number(run.more - 1);
        QStringList quotedFields;
        for (int i = 0; i < c.size(); ++i)
            quotedFields << (i >= 12 ? c.at(i) : quoted(c.at(i)));
        out.write(quotedFields.join(QLatin1Char(',')).toUtf8() + "\r\n");
        run.more = 0;
    };

    bool header = true;
    while (!in.atEnd()) {
        const QByteArray raw = in.readLine();
        if (header) {
            header = false;
            out.write(raw);
            continue;
        }
        const bool failed = raw.contains("\"FailedLogon\"");
        const QStringList c = splitLine(QString::fromUtf8(raw).trimmed());
        if (c.size() < 14) {
            out.write(raw);
            continue;
        }
        const QDateTime time = QDateTime::fromString(c.at(0).left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
        const qint64 ageDays = time.isValid() ? time.daysTo(now) : 0;
        if (ageDays > (failed ? kKeepFailedDays : kKeepOtherDays)) {
            ++dropped;
            continue;
        }
        if (!failed || c.at(5) != QLatin1String("SEC") || c.at(10).contains(QLatin1String(kMoreMarker)) || !time.isValid()) {
            out.write(raw);
            continue;
        }
        const QString key = c.at(2) + QLatin1Char('|') + c.at(8);
        Run &run = runs[key];
        if (!run.last.isValid() || qAbs(run.last.secsTo(time)) > kRunGapSecs) {
            flush(run, c.at(8));
            run.kept = 0;
        }
        run.last = time;
        if (run.kept < kMaxFailedLinesPerAddress) {
            ++run.kept;
            out.write(raw);
        } else {
            ++run.more;
            ++dropped;
            if (run.newest.isEmpty() || c.at(0) >= run.newest.at(0))
                run.newest = c;
        }
        // Runs that ended long ago are written out so the table stays small.
        if (runs.size() > 50000) {
            for (auto it = runs.begin(); it != runs.end();) {
                if (qAbs(it->last.secsTo(time)) > kRunGapSecs) {
                    flush(it.value(), it.key().section(QLatin1Char('|'), 1));
                    it = runs.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    for (auto it = runs.begin(); it != runs.end(); ++it)
        flush(it.value(), it.key().section(QLatin1Char('|'), 1));

    in.close();
    if (dropped == 0) {
        out.cancelWriting();
        return;
    }
    out.commit();
}

QList<AccessRecord> AccessLog::readAll()
{
    QMutexLocker lock(&g_mutex);
    return readFile();
}

QList<AccessRecord> AccessLog::readRecent(qint64 maxBytes)
{
    QMutexLocker lock(&g_mutex);
    return readFile(nullptr, maxBytes);
}

QList<AccessRecord> AccessLog::readSessionEvents()
{
    QMutexLocker lock(&g_mutex);
    QList<AccessRecord> events;
    for (const AccessRecord &r : readFile(nullptr, 0, true))
        if (r.source == QLatin1String("LSM")
            && (r.event == QLatin1String("Logon") || r.event == QLatin1String("Logoff")
                || r.event == QLatin1String("Disconnect") || r.event == QLatin1String("Reconnect")))
            events << r;
    return events;
}
