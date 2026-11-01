#include "Threats.h"

#include "GeoIp.h"
#include "model/ServerProfile.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

constexpr int kBruteForceAttempts = 5;
constexpr qint64 kBruteForceWindowMs = 10 * 60 * 1000;
constexpr int kBreakInAttempts = 3;
constexpr qint64 kBreakInWindowMs = 60 * 60 * 1000;
// The same address is not reported again for this long.
constexpr qint64 kRepeatMs = 30 * 60 * 1000;
// Older events are history, not news.
constexpr qint64 kRecentMs = 24 * 3600 * 1000;
constexpr int kMaxUsers = 20;

QMutex g_mutex;
bool g_loaded = false;

struct Failures {
    QList<qint64> times;  // ms since epoch of each failed attempt in the last hour
    QStringList users;    // accounts tried
    qint64 lastAlert = 0; // time of the event that raised the last alarm
};
QHash<QString, Failures> g_failures;           // server id|address
QHash<QString, qint64> g_breakInAt;            // server id|address|user -> time of the last alarm
QSet<QString> g_known;                         // server id|account|country that signed in before
QSet<QString> g_serversWithHistory;            // servers with at least one sign-in on record
QHash<QString, QString> g_country;             // address -> country, "" if none

qint64 msOf(const QString &timeUtc)
{
    const QDateTime utc = QDateTime::fromString(timeUtc.left(23) + QLatin1Char('Z'), Qt::ISODateWithMs);
    return utc.isValid() ? utc.toMSecsSinceEpoch() : 0;
}

QString shortName(const QString &user)
{
    return user.mid(user.lastIndexOf(QLatin1Char('\\')) + 1);
}

bool isSignIn(const QString &event)
{
    return event == QLatin1String("Authenticated") || event == QLatin1String("SecurityLogon")
        || event == QLatin1String("Logon") || event == QLatin1String("Reconnect")
        || event == QLatin1String("SecurityReconnect");
}

// Caller holds the mutex.
QString countryOf(const QString &address)
{
    // Only addresses: the events also say "LOCAL", which would be looked up as a host name.
    static const QRegularExpression ip(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}$|^[0-9a-fA-F:.]*:[0-9a-fA-F:.]*$"));
    if (!ip.match(address).hasMatch())
        return {};
    const auto it = g_country.constFind(address);
    if (it != g_country.cend())
        return it.value();
    return g_country.insert(address, GeoIp::countryOf(address)).value();
}

QString knownKey(const QString &serverId, const QString &user, const QString &country)
{
    return serverId + QLatin1Char('|') + shortName(user).toLower() + QLatin1Char('|') + country;
}

// Programs that attackers use to keep a foothold, and places they hide in. A hit is not proof, only
// worth a look.
bool suspiciousCommand(const QString &text)
{
    static const QStringList markers = {
        QStringLiteral("powershell"), QStringLiteral("pwsh"), QStringLiteral("cmd.exe /c"), QStringLiteral("cmd /c"),
        QStringLiteral("mshta"), QStringLiteral("rundll32"), QStringLiteral("regsvr32"), QStringLiteral("wscript"),
        QStringLiteral("cscript"), QStringLiteral("certutil"), QStringLiteral("bitsadmin"), QStringLiteral("psexe"),
        QStringLiteral("frombase64"), QStringLiteral(" -enc"), QStringLiteral("http://"), QStringLiteral("https://"),
        QStringLiteral("\\temp\\"), QStringLiteral("\\appdata\\"), QStringLiteral("\\users\\public\\"),
        QStringLiteral("\\perflogs\\"),
    };
    const QString lower = text.toLower();
    return std::any_of(markers.cbegin(), markers.cend(), [&lower](const QString &m) { return lower.contains(m); });
}

AccessRecord alertRecord(const AccessRecord &cause, const QString &event, const QString &user, const QString &detail)
{
    AccessRecord a = cause;
    a.source = QStringLiteral("RCONNECT");
    a.event = event;
    a.user = user;
    a.detail = detail;
    a.client.clear();
    a.operatorName.clear();
    a.sessionId = 0;
    a.recordId = 0;
    return a;
}

// Caller holds the mutex.
void loadHistory()
{
    if (g_loaded)
        return;
    g_loaded = true;
    for (const AccessRecord &r : AccessLog::readAll()) {
        if (r.source == QLatin1String("RCONNECT") || !isSignIn(r.event))
            continue;
        g_serversWithHistory.insert(r.serverId);
        const QString country = countryOf(r.address);
        if (!country.isEmpty())
            g_known.insert(knownKey(r.serverId, r.user, country));
    }
    GeoIp::release();
}

} // namespace

bool Threats::isTampering(const QString &event)
{
    return event == QLatin1String("UserCreated") || event == QLatin1String("GroupMemberAdded")
        || event == QLatin1String("LogCleared");
}

void Threats::warmUp()
{
    QMutexLocker lock(&g_mutex);
    loadHistory();
}

QList<AccessRecord> Threats::evaluate(const ServerProfile &server, const QList<AccessRecord> &events)
{
    QMutexLocker lock(&g_mutex);
    loadHistory();

    QList<AccessRecord> sorted = events;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const AccessRecord &a, const AccessRecord &b) { return a.timeUtc < b.timeUtc; });

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<AccessRecord> news;
    const auto raise = [&](const AccessRecord &cause, const QString &event, const QString &user, const QString &detail) {
        const AccessRecord a = alertRecord(cause, event, user, detail);
        AccessLog::alert(server, a.timeUtc, event, user, a.address, detail);
        news << a;
    };

    for (const AccessRecord &r : std::as_const(sorted)) {
        const qint64 t = msOf(r.timeUtc);
        const bool recent = t > 0 && now - t <= kRecentMs;

        if (Threats::isTampering(r.event)) {
            if (recent)
                news << r;
            continue;
        }
        if (r.event == QLatin1String("ServiceInstalled") || r.event == QLatin1String("TaskCreated")) {
            if (recent && suspiciousCommand(r.detail))
                raise(r, QStringLiteral("AlertSuspicious"), r.user.isEmpty() ? r.event : r.user,
                      r.event + QLatin1Char('|') + r.detail);
            continue;
        }

        if (r.event == QLatin1String("FailedLogon") && !r.address.isEmpty() && t > 0) {
            Failures &f = g_failures[server.id + QLatin1Char('|') + r.address];
            f.times.append(t);
            f.times.erase(std::remove_if(f.times.begin(), f.times.end(),
                                         [t](qint64 x) { return x < t - kBreakInWindowMs; }),
                          f.times.end());
            const QString who = shortName(r.user);
            if (!who.isEmpty() && !f.users.contains(who, Qt::CaseInsensitive)) {
                f.users.append(who);
                while (f.users.size() > kMaxUsers)
                    f.users.removeFirst();
            }
            const int inWindow = int(std::count_if(f.times.cbegin(), f.times.cend(),
                                                   [t](qint64 x) { return x >= t - kBruteForceWindowMs; }));
            if (recent && inWindow >= kBruteForceAttempts && (f.lastAlert == 0 || t - f.lastAlert >= kRepeatMs)) {
                f.lastAlert = t;
                raise(r, QStringLiteral("AlertBruteForce"), f.users.mid(0, 5).join(QStringLiteral(", ")),
                      QStringLiteral("%1|%2").arg(inWindow).arg(f.users.size()));
            }
            continue;
        }

        if (!isSignIn(r.event) || r.source == QLatin1String("RCONNECT"))
            continue;

        // A sign-in that follows failed attempts from the same address.
        if (!r.address.isEmpty() && t > 0 && r.event != QLatin1String("Reconnect")
            && r.event != QLatin1String("SecurityReconnect")) {
            const auto it = g_failures.constFind(server.id + QLatin1Char('|') + r.address);
            if (it != g_failures.cend()) {
                const int before = int(std::count_if(it->times.cbegin(), it->times.cend(), [t](qint64 x) {
                    return x >= t - kBreakInWindowMs && x <= t;
                }));
                const QString key = server.id + QLatin1Char('|') + r.address + QLatin1Char('|') + shortName(r.user).toLower();
                if (recent && before >= kBreakInAttempts && t - g_breakInAt.value(key) >= kRepeatMs) {
                    g_breakInAt.insert(key, t);
                    raise(r, QStringLiteral("AlertBreakIn"), r.user, QString::number(before));
                }
            }
        }

        // An account signing in from a country it was never seen in. The first sign-in a server
        // is seen with only sets the baseline.
        const bool hadHistory = g_serversWithHistory.contains(server.id);
        g_serversWithHistory.insert(server.id);
        const QString country = countryOf(r.address);
        if (country.isEmpty() || r.user.isEmpty())
            continue;
        const QString key = knownKey(server.id, r.user, country);
        if (g_known.contains(key))
            continue;
        g_known.insert(key);
        if (recent && hadHistory)
            raise(r, QStringLiteral("AlertNewCountry"), r.user, country);
    }
    GeoIp::release();
    return news;
}
