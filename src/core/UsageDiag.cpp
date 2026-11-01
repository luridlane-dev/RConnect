#include "UsageDiag.h"

#include "AccessLog.h"
#include "WinRm.h"
#include "model/ServerProfile.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMap>

namespace {

constexpr int kTimeoutMs = 60000;

QString events(const QString &log, const QString &ids, int count)
{
    QString filter;
    for (const QString &id : ids.split(QLatin1Char(',')))
        filter += (filter.isEmpty() ? QString() : QStringLiteral(" or ")) + QStringLiteral("EventID=") + id;
    return QStringLiteral("wevtutil qe %1 /c:%2 /rd:true /f:xml /q:\"*[System[(%3)]]\" 2>&1 & ")
        .arg(log).arg(count).arg(filter);
}

} // namespace

QString UsageDiag::report(const ServerProfile &p)
{
    const QString lsm = QStringLiteral("Microsoft-Windows-TerminalServices-LocalSessionManager/Operational");
    const QString rcm = QStringLiteral("Microsoft-Windows-TerminalServices-RemoteConnectionManager/Operational");
    const QString command =
        QStringLiteral("echo ##CLOCK## & powershell -NoProfile -NonInteractive -Command \"(Get-Date).ToUniversalTime().ToString('o'); "
                       "[TimeZoneInfo]::Local.Id\" 2>&1 & ")
        + QStringLiteral("echo ##QUSER## & quser 2>&1 & echo ##QWINSTA## & qwinsta 2>&1 & ")
        + QStringLiteral("echo ##LSM_INFO## & wevtutil gli %1 2>&1 & ").arg(lsm)
        + QStringLiteral("echo ##LSM## & ") + events(lsm, QStringLiteral("21,22,23,24,25,39,40"), 40)
        + QStringLiteral("echo ##RCM## & ") + events(rcm, QStringLiteral("1149"), 10)
        + QStringLiteral("echo ##CLIENTNAME## & for /f \"delims=\" %k in ('reg query HKU') do "
                         "@reg query \"%k\\Volatile Environment\" /s /v CLIENTNAME 2>nul");

    QString out = QStringLiteral("=== %1 (%2) ===\nasked at %3Z\n").arg(
        p.name, p.host, QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")));
    const SessionQuery q = WinRm::runCommand(p.host, p.user, p.domain, p.password, kTimeoutMs, command);
    if (q.result != SessionQuery::Ok)
        return out + QStringLiteral("FAILED: ") + q.error + QLatin1Char('\n');
    out += QString::fromUtf8(q.output) + QLatin1Char('\n');

    // The periodic check's own query, from the same cursors.
    const QHash<QString, EventCursor> cursors = AccessLog::cursors(p.id);
    out += QStringLiteral("##POLL##\n");
    for (auto it = cursors.cbegin(); it != cursors.cend(); ++it)
        out += QStringLiteral("cursor %1 after=%2 lookback=%3\n").arg(it.key(), it.value().afterUtc).arg(it.value().lookbackMs);
    QElapsedTimer timer;
    timer.start();
    const SessionQuery poll = WinRm::querySessions(p.host, p.user, p.domain, p.password, 20000, &cursors);
    out += QStringLiteral("result=%1 took=%2ms error=%3\n").arg(int(poll.result)).arg(timer.elapsed()).arg(poll.error);
    QMap<QString, int> bySource;
    for (const AccessEvent &e : poll.events)
        ++bySource[e.source];
    for (auto it = bySource.cbegin(); it != bySource.cend(); ++it)
        out += QStringLiteral("events %1: %2\n").arg(it.key()).arg(it.value());
    for (const AccessEvent &e : poll.events)
        if (e.source == QLatin1String("LSM"))
            out += QStringLiteral("  LSM %1 id=%2 rec=%3 session=%4 %5 %6\n").arg(e.timeUtc).arg(int(e.kind)).arg(e.recordId)
                       .arg(e.sessionId).arg(e.user, e.address);
    for (const RemoteSession &s : poll.sessions)
        out += QStringLiteral("  session %1 user=%2 connected=%3 idle=%4 client=%5 logonUtcMs=%6\n").arg(s.id).arg(s.user)
                   .arg(s.connected).arg(s.idleMinutes).arg(s.client).arg(s.logonUtcMs);
    return out;
}
