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
#include <QTimeZone>

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
// Failed sign-ins within this window from one address are folded into a single summary line.
constexpr qint64 kRunGapSecs = 3600;
constexpr int kKeepFailedDays = 14;
constexpr int kKeepOtherDays = 400;

// The summary line of a folded run has "|more=N" after its detail: N attempts besides the line itself.
const char *const kMoreMarker = "|more=";

const char kHeader[] =
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
    case AccessEvent::UserEnabled: return QStringLiteral("UserEnabled");
    case AccessEvent::UserDisabled: return QStringLiteral("UserDisabled");
    case AccessEvent::UserDeleted: return QStringLiteral("UserDeleted");
    case AccessEvent::PasswordChangeAttempt: return QStringLiteral("PasswordChangeAttempt");
    case AccessEvent::PasswordResetAttempt: return QStringLiteral("PasswordResetAttempt");
    case AccessEvent::AccountLocked: return QStringLiteral("AccountLocked");
    case AccessEvent::GroupMemberRemoved: return QStringLiteral("GroupMemberRemoved");
    case AccessEvent::TaskRemoved: return QStringLiteral("TaskRemoved");
    case AccessEvent::TaskEnabled: return QStringLiteral("TaskEnabled");
    case AccessEvent::TaskDisabled: return QStringLiteral("TaskDisabled");
    case AccessEvent::TaskUpdated: return QStringLiteral("TaskUpdated");
    case AccessEvent::AuditPolicyChanged: return QStringLiteral("AuditPolicyChanged");
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

// Fast zero-allocation ISO-8601 UTC parser ("YYYY-MM-DDTHH:MM:SS(.mmm...)Z" -> ms since epoch, or 0).
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

// Within the last ten minutes: new enough for a session number to still mean the same session.
bool recent(const QString &timeUtc)
{
    const qint64 ms = parseUtcMs(timeUtc);
    return ms != 0 && qAbs(QDateTime::currentMSecsSinceEpoch() - ms) <= 600000;
}

QString localTime(const QString &timeUtc)
{
    const qint64 ms = parseUtcMs(timeUtc);
    return ms != 0 ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::LocalTime).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                   : QString();
}

// One CSV line without its line break; fields may be quoted.
QStringList splitLine(QStringView line)
{
    QStringList fields;
    fields.reserve(14);
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
        // Cheap test on the bytes first: keep LSM session events and Security events that carry client computer names.
        if (mode == Scan::SessionEvents && !header && !oldFormat
            && !raw.contains("\"LSM\"") && !raw.contains("\"SecurityReconnect\"")
            && !raw.contains("\"SecurityDisconnect\"") && !raw.contains("\"SecurityLogon\""))
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
    if (isNew && f.write(kHeader) != qint64(sizeof(kHeader) - 1))
        return false;
    return f.write(lines) == lines.size() && f.flush();
}

} // namespace

// Everyone who signs in to or out of a server is recorded. There is no switch for it.
bool AccessLog::serverEventsEnabled()
{
    return true;
}

void AccessLog::setServerEventsEnabled(bool)
{
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
        QStringLiteral("UserEnabled"), QStringLiteral("UserDisabled"), QStringLiteral("UserDeleted"),
        QStringLiteral("PasswordChangeAttempt"), QStringLiteral("PasswordResetAttempt"), QStringLiteral("AccountLocked"),
        QStringLiteral("GroupMemberRemoved"), QStringLiteral("TaskRemoved"), QStringLiteral("TaskEnabled"),
        QStringLiteral("TaskDisabled"), QStringLiteral("TaskUpdated"), QStringLiteral("AuditPolicyChanged"),
        QStringLiteral("Connect"),       QStringLiteral("Monitor"),         QStringLiteral("AuditEnable"),
        QStringLiteral("Message"),       QStringLiteral("DisplayTuned"),
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
        const QString channelKey = key(serverId, source);
        c.afterUtc = g_lastEvent.value(channelKey);
        for (qint64 id : g_atLastEvent.value(channelKey))
            c.afterRecordId = qMax(c.afterRecordId, id);
        const qint64 checked = g_checkedAt.value(channelKey);
        if (c.afterUtc.isEmpty() && checked > 0)
            c.lookbackMs = QDateTime::currentMSecsSinceEpoch() - checked + kLookbackMarginMs;
        result.insert(source, c);
    }
    return result;
}

int AccessLog::polled(const ServerProfile &server, const QList<AccessEvent> &events, const QList<RemoteSession> &live,
                      QList<AccessRecord> *written, const QSet<QString> &failedSources)
{
    QMutexLocker lock(&g_mutex);
    loadLastEvents();

    QHash<QString, QString> newest;
    QHash<QString, QMultiHash<QString, qint64>> writtenIds; // by key: time -> record ids
    QList<AccessRecord> records;
    QByteArray lines;
    struct Folded { qint64 count = 0; QString timeUtc, user, detail, client, address; qint64 recordId = 0; };
    QHash<QString, Folded> folded;       // address (or client) -> folded failed sign-ins in this check
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
        // Password guessing from the internet produces thousands of failed sign-ins a day: fold all failed
        // attempts from the same address within one check into a single line (Threats still sees every event).
        if (e.kind == AccessEvent::FailedLogon) {
            const QString foldKey = !e.address.isEmpty() ? e.address : (QStringLiteral("C:") + client);
            Folded &f = folded[foldKey];
            ++f.count;
            if (f.count == 1 || e.timeUtc >= f.timeUtc) {
                f.timeUtc = e.timeUtc;
                f.user = e.user;
                f.detail = e.detail;
                f.client = client;
                f.address = e.address;
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
        const Folded &f = it.value();
        const QString detail = f.count > 1 ? f.detail + QLatin1String(kMoreMarker) + QString::number(f.count - 1) : f.detail;
        lines += line(server, f.timeUtc, QStringLiteral("SEC"), QStringLiteral("FailedLogon"), f.user, f.address, f.client,
                      detail, {}, 0, f.recordId);
    }
    if (added > 0 && !write(lines))
        return -1;
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
        if (!failedSources.contains(source))
            g_checkedAt.insert(key(server.id, source), now);
    return added;
}

void AccessLog::note(const ServerProfile &server, const QString &event, const QString &user, const QString &detail,
                     const QString &address, const QDateTime &when)
{
    QMutexLocker lock(&g_mutex);
    const QString timeUtc = when.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")) + QStringLiteral("0000Z");
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
    constexpr qint64 kLookAtLeast = 256 * 1024; // compact once the log grows past 256 KB
    if (in.size() < kLookAtLeast || !in.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QSaveFile out(path());
    if (!out.open(QIODevice::WriteOnly))
        return;

    struct Run {
        qint64 lastMs = 0;
        qint64 total = 0;   // total attempts represented
        qint64 lines = 0;   // source CSV lines merged into this run
        QStringList newest; // fields of the newest line, with |more= stripped from detail
    };
    QHash<QString, Run> runs; // server id | address
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kDayMs = 86400LL * 1000;
    constexpr qint64 kRunGapMs = kRunGapSecs * 1000;
    qint64 dropped = 0;

    const auto flush = [&](Run &run) {
        if (run.total <= 0 || run.newest.isEmpty())
            return;
        QStringList c = run.newest;
        if (run.total > 1)
            c[10] = c.at(10) + QLatin1String(kMoreMarker) + QString::number(run.total - 1);
        QStringList quotedFields;
        quotedFields.reserve(c.size());
        for (int i = 0; i < c.size(); ++i)
            quotedFields << (i >= 12 ? c.at(i) : quoted(c.at(i)));
        out.write(quotedFields.join(QLatin1Char(',')).toUtf8() + "\r\n");
        if (run.lines > 1)
            dropped += run.lines - 1;
        run = {};
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
        const QString lineText = QString::fromUtf8(raw);
        QStringList c = splitLine(QStringView(lineText).trimmed());
        if (c.size() < 14) {
            out.write(raw);
            continue;
        }
        const qint64 timeMs = parseUtcMs(c.at(0));
        const qint64 ageMs = timeMs != 0 ? (nowMs - timeMs) : 0;
        if (ageMs > qint64(failed ? kKeepFailedDays : kKeepOtherDays) * kDayMs) {
            ++dropped;
            continue;
        }
        if (!failed || c.at(5) != QLatin1String("SEC") || timeMs == 0) {
            out.write(raw);
            continue;
        }
        qint64 standsFor = 1;
        if (const qsizetype marker = c.at(10).indexOf(QLatin1String(kMoreMarker)); marker >= 0) {
            standsFor += qMax<qint64>(0, QStringView(c.at(10)).mid(marker + 6).toLongLong());
            c[10].truncate(marker);
        }
        const QString addrKey = !c.at(8).isEmpty() ? c.at(8) : (QStringLiteral("C:") + c.at(9));
        const QString key = c.at(2) + QLatin1Char('|') + addrKey;
        Run &run = runs[key];
        if (run.lastMs != 0 && qAbs(run.lastMs - timeMs) > kRunGapMs)
            flush(run);
        run.lastMs = timeMs;
        run.total += standsFor;
        ++run.lines;
        if (run.newest.isEmpty() || c.at(0) >= run.newest.at(0))
            run.newest = std::move(c);
        // Runs that ended long ago are written out so the table stays small.
        if (runs.size() > 50000) {
            for (auto it = runs.begin(); it != runs.end();) {
                if (qAbs(it->lastMs - timeMs) > kRunGapMs) {
                    flush(it.value());
                    it = runs.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    for (auto it = runs.begin(); it != runs.end(); ++it)
        flush(it.value());

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
    for (const AccessRecord &r : readFile(nullptr, 0, true)) {
        const bool lsmSession = r.source == QLatin1String("LSM")
            && (r.event == QLatin1String("Logon") || r.event == QLatin1String("Logoff")
                || r.event == QLatin1String("Disconnect") || r.event == QLatin1String("Reconnect"));
        const bool secClientHint = r.source == QLatin1String("SEC") && !r.client.isEmpty()
            && (r.event == QLatin1String("SecurityReconnect") || r.event == QLatin1String("SecurityDisconnect")
                || r.event == QLatin1String("SecurityLogon"));
        if (lsmSession || secClientHint)
            events << r;
    }
    return events;
}
