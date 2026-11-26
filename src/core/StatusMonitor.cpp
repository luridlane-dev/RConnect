#include "StatusMonitor.h"
#include "AccessLog.h"
#include "Contacts.h"
#include "GeoIp.h"
#include "Log.h"
#include "Threats.h"
#include "UsageEngine.h"
#include "UsageLog.h"
#include "WtsQuery.h"
#include "model/ServerStore.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace {

// The port is checked this often. Who is connected is asked about every 6 to 45 seconds (see sessionAgeMs()),
// the servers' logs once a minute.
constexpr int kPollIntervalMs = 6000;
constexpr int kConnectTimeoutMs = 2000;
// Checks are spread over this window instead of firing at once.
constexpr int kSpreadMs = 3000;
constexpr int kSessionTimeoutMs = 8000;
// The periodic check also reads the access events, which can be a few hundred kB after a long pause.
constexpr int kPollSessionTimeoutMs = 20000;
// At most this many checks run at once, so a long list never opens hundreds of sockets.
constexpr int kMaxParallelChecks = 8;
constexpr int kMaxInteractiveChecks = 3;
constexpr int kMaxEventPolls = 4;
constexpr qint64 kRecentSessionMs = 15000;
constexpr qint64 kSessionTimesEveryMs = 55000;
constexpr qint64 kMissingSessionTimesEveryMs = 15000;
// The quick session check (quser, no PowerShell) is cheap, so a short list is asked on every round (6 s) and a
// long one up to every 45 s. Busy/free shows up within seconds instead of within a minute.
qint64 sessionAgeMs(qsizetype servers)
{
    return qBound<qint64>(5000, qint64(servers) * 300, qint64(45000));
}
// Checks somebody is waiting for (a click, the server list opened) jump ahead of the background round.
constexpr int kForcedPriority = 10;
// The logs are read once a minute; idle samples are written more frequently from session checks.
constexpr qint64 kEventsEveryMs = 55000;
constexpr qint64 kUsageSampleEveryMs = 25000;
// A single failed port check is retried before a server is shown as offline.
constexpr int kRetryDelayMs = 4000;

qint64 sessionRetryMs(const ServerStatus &status, qsizetype servers)
{
    if (status.occupancy != ServerStatus::OccupancyError)
        return sessionAgeMs(servers);
    return status.occupancyResult == SessionQuery::AuthFailed ? 60000 : 15000;
}

bool missingSessionTimes(const ServerStatus &status)
{
    return std::any_of(status.all.cbegin(), status.all.cend(), [&status](const RemoteSession &session) {
        return session.connected && connectionMinutes(status, session) < 0;
    });
}

// Connect and close; nothing is sent. Returns the connect time in ms, or -1.
int probe(const std::wstring &host, quint16 port)
{
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW *results = nullptr;
    if (GetAddrInfoW(host.c_str(), std::to_wstring(port).c_str(), &hints, &results) != 0)
        return -1;

    int latency = -1;
    for (ADDRINFOW *a = results; a && latency < 0; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        const auto start = std::chrono::steady_clock::now();
        bool online = false;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        if (connect(s, a->ai_addr, int(a->ai_addrlen)) == 0) {
            online = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval tv{kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
            online = select(0, nullptr, &writable, &failed, &tv) > 0 && FD_ISSET(s, &writable);
        }
        if (online) {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            latency = int(qMax<qint64>(1, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
        }
        closesocket(s);
    }
    FreeAddrInfoW(results);
    return latency;
}

const QString &localComputerName()
{
    static const QString name = [] {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        return GetComputerNameW(buf, &size) ? QString::fromWCharArray(buf, int(size)) : QString();
    }();
    return name;
}

// When WinRM does not answer, Windows' own session query is tried: the server then still shows who is in. A refused
// sign-in is not retried that way (the same account would be refused again).
SessionQuery orWts(const ServerProfile &p, SessionQuery q)
{
    if (q.result == SessionQuery::Ok || q.result == SessionQuery::AuthFailed)
        return q;
    SessionQuery fallback = WtsQuery::sessions(p.host, p.user, p.domain, p.password, kSessionTimeoutMs);
    if (fallback.result != SessionQuery::Ok) {
        q.fallbackError = fallback.error;
        return q;
    }
    fallback.viaWts = true;
    return fallback;
}

// Interactive check right before connecting: no events, so it stays quick.
SessionQuery querySessions(const ServerProfile &p, bool includeTimes)
{
    return orWts(p, WinRm::querySessions(p.host, p.user, p.domain, p.password, kSessionTimeoutMs, nullptr, includeTimes));
}

} // namespace

struct StatusMonitor::Channel {
    std::mutex mutex;
    StatusMonitor *target = nullptr;
};

struct StatusMonitor::SessionCheck {
    ServerProfile profile;
    QList<std::function<void(const ServerStatus &)>> callbacks;
    std::atomic_bool started = false;
    bool interactive = false;
    bool includeTimes = false;
    bool refreshAfterLocalChange = false;
};

StatusMonitor::StatusMonitor(ServerStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_channel(std::make_shared<Channel>())
{
    m_probePool.setMaxThreadCount(kMaxParallelChecks);
    m_sessionPool.setMaxThreadCount(kMaxParallelChecks);
    m_interactivePool.setMaxThreadCount(kMaxInteractiveChecks);
    m_eventPool.setMaxThreadCount(kMaxEventPolls);
    // Background parsing must yield to the RDP control and interactive checks.
    m_probePool.setThreadPriority(QThread::LowPriority);
    m_sessionPool.setThreadPriority(QThread::LowPriority);
    m_eventPool.setThreadPriority(QThread::LowPriority);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    m_channel->target = this;
    m_timer.setInterval(kPollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &StatusMonitor::refreshAll);
}

StatusMonitor::~StatusMonitor()
{
    {
        std::lock_guard lock(m_channel->mutex);
        m_channel->target = nullptr;
    }
    m_probePool.clear();
    m_sessionPool.clear();
    m_interactivePool.clear();
    m_eventPool.clear();
}

void StatusMonitor::setActive(bool active)
{
    if (active) {
        if (!m_timer.isActive()) {
            m_timer.start();
            for (const ServerProfile &profile : m_store->all())
                refresh(profile.id);
        }
    } else {
        m_timer.stop();
    }
}

void StatusMonitor::refreshAll()
{
    const QList<ServerProfile> profiles = m_store->all();
    // Spread periodic checks so every round does not hit all the servers at once.
    const int spread = qBound(200, int(profiles.size()) * 150, kSpreadMs);
    for (const ServerProfile &p : profiles) {
        if (!p.monitor) {
            refresh(p.id); // marks it as disabled
            continue;
        }
        const QString id = p.id;
        const int delay = int(QRandomGenerator::global()->bounded(spread));
        QTimer::singleShot(delay, this, [this, id] { if (m_timer.isActive()) refresh(id); });
    }
}

void StatusMonitor::refreshAllNow()
{
    for (const ServerProfile &p : m_store->all())
        if (p.monitor && canCheckSessions(p))
            refresh(p.id, true);
}

void StatusMonitor::refresh(const QString &id, bool force)
{
    if (force)
        m_forcedProbes.insert(id);
    if (m_probes.contains(id))
        return;
    const ServerProfile p = m_store->profileById(id);
    if (!p.isValid()) {
        m_forcedProbes.remove(id);
        return;
    }

    if (!p.monitor && !force) {
        ServerStatus off;
        off.state = ServerStatus::Disabled;
        m_store->setStatus(id, off);
        return;
    }
    ServerStatus current = m_store->status(id);
    if (current.state == ServerStatus::Unknown || current.state == ServerStatus::Disabled) {
        current.state = ServerStatus::Checking;
        m_store->setStatus(id, current);
    }

    m_probes.insert(id);
    m_probePool.start([channel = m_channel, p] {
        const int latency = probe(p.host.toStdWString(), quint16(p.port));
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, p, latency] {
                target->finishProbe(p, latency);
            }, Qt::QueuedConnection);
    }, force ? kForcedPriority : 0);
}

bool StatusMonitor::canCheckSessions(const ServerProfile &p)
{
    return p.checkSessions && !p.user.isEmpty() && !p.password.isEmpty();
}

std::optional<ServerStatus> StatusMonitor::recentSessionStatus(const ServerProfile &p) const
{
    const ServerProfile current = m_store->profileById(p.id);
    if (!current.isValid() || !canCheckSessions(current) || !current.sameConnection(p))
        return std::nullopt;
    const ServerStatus status = m_store->status(p.id);
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - status.checkedAt;
    const qint64 maxAge = status.occupancy == ServerStatus::OccupancyError
        ? sessionRetryMs(status, m_store->rowCount()) : kRecentSessionMs;
    if (status.occupancy == ServerStatus::OccupancyUnknown || status.checkedAt <= 0 || age < 0 || age > maxAge)
        return std::nullopt;
    return status;
}

void StatusMonitor::checkSessions(const ServerProfile &p, std::function<void(const ServerStatus &)> done)
{
    requestSessions(p, false, std::move(done));
}

void StatusMonitor::localConnectionChanged(const QString &id)
{
    const ServerProfile p = m_store->profileById(id);
    const ServerStatus status = m_store->status(id);
    if (!p.isValid() || !canCheckSessions(p)
        || (status.occupancyResult == SessionQuery::AuthFailed
            && QDateTime::currentMSecsSinceEpoch() - status.checkedAt < sessionRetryMs(status, m_store->rowCount())))
        return;
    const auto check = m_sessionChecks.value(id);
    if (check && check->profile.sameConnection(p) && check->started.load()) {
        check->refreshAfterLocalChange = true;
        return;
    }
    requestSessions(p, false, [](const ServerStatus &) {});
}

void StatusMonitor::requestSessions(const ServerProfile &p, bool includeTimes,
                                    std::function<void(const ServerStatus &)> done)
{
    auto check = m_sessionChecks.value(p.id);
    if (!check || !check->profile.sameConnection(p)) {
        check = std::make_shared<SessionCheck>();
        check->profile = p;
        check->includeTimes = includeTimes;
        m_sessionChecks.insert(p.id, check);
        if (!done) {
            startSessionCheck(check, m_sessionPool);
            return;
        }
    }
    if (!done)
        return;
    check->callbacks.append(std::move(done));
    if (!check->interactive) {
        check->interactive = true;
        // Take over queued work without starting a second network request if the first worker already began.
        startSessionCheck(check, m_interactivePool);
    }
}

void StatusMonitor::startSessionCheck(const std::shared_ptr<SessionCheck> &check, QThreadPool &pool)
{
    pool.start([channel = m_channel, check] {
        if (check->started.exchange(true))
            return;
        const SessionQuery q = querySessions(check->profile, check->includeTimes);
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, check, q] {
                target->finishSessions(check, q);
            }, Qt::QueuedConnection);
    });
}

void StatusMonitor::finishSessions(const std::shared_ptr<SessionCheck> &check, const SessionQuery &q)
{
    const ServerProfile &p = check->profile;
    const ServerProfile current = m_store->profileById(p.id);
    const bool latest = m_sessionChecks.value(p.id) == check;
    if (latest)
        m_sessionChecks.remove(p.id);
    const bool publish = latest && canCheckSessions(current) && current.sameConnection(p);
    if (publish && check->refreshAfterLocalChange && q.result == SessionQuery::Ok) {
        // This sample may have been taken before the local login or disconnect.
        requestSessions(current, false, [callbacks = std::move(check->callbacks)](const ServerStatus &s) {
            for (const auto &callback : callbacks)
                callback(s);
        });
        return;
    }
    ServerStatus status = publish ? m_store->status(p.id) : ServerStatus{};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool optionalTimesFailed = check->includeTimes && q.result != SessionQuery::Ok
        && q.result != SessionQuery::AuthFailed && (status.occupancy == ServerStatus::Free || status.occupancy == ServerStatus::Busy);
    if (!optionalTimesFailed)
        applySessions(status, q);
    if (q.result == SessionQuery::Ok) {
        if (q.serverNowUtcMs > 0)
            m_serverClockOffsetMs.insert(p.id, q.serverNowUtcMs - now);
        else if (status.serverNowUtcMs <= 0 && m_serverClockOffsetMs.contains(p.id))
            status.serverNowUtcMs = now + m_serverClockOffsetMs.value(p.id);
    }
    if (publish && status.state != ServerStatus::Offline) {
        if (check->includeTimes)
            m_sessionTimesAt.insert(p.id, now);
        m_store->setStatus(p.id, status);
        if (current.monitor && status.state == ServerStatus::Online && q.result == SessionQuery::Ok) {
            warnAboutStrangers(p.id, status);
            const qint64 sampleEvery = q.sessions.isEmpty() ? kEventsEveryMs : kUsageSampleEveryMs;
            if (q.sessionsRead && now - m_usageAt.value(p.id) >= sampleEvery) {
                m_usageAt.insert(p.id, now);
                ServerProfile sampleProfile;
                sampleProfile.id = current.id;
                sampleProfile.name = current.name;
                const qint64 sampledAt = status.serverNowUtcMs > 0 ? status.serverNowUtcMs : 0;
                m_eventPool.start([sampleProfile, sessions = q.sessions, sampledAt] {
                    UsageLog::sampled(sampleProfile, sessions, sampledAt);
                }, kForcedPriority);
            }
            if (!q.viaWts)
                pollEvents(current);
            if (!check->interactive && !check->includeTimes && missingSessionTimes(status)
                && now - m_sessionTimesAt.value(p.id) >= kMissingSessionTimesEveryMs)
                requestSessions(current, true);
        }
    }
    for (const auto &callback : std::as_const(check->callbacks))
        callback(status);
}

void StatusMonitor::applySessions(ServerStatus &s, const SessionQuery &q)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 previousClock = s.serverNowUtcMs;
    const qint64 previousCheck = s.checkedAt;
    const QList<RemoteSession> previous = std::move(s.all);
    s.checkedAt = now;
    s.serverNowUtcMs = 0;
    s.others.clear();
    s.all.clear();
    s.mine = false;
    s.occupancyError.clear();
    s.occupancyFallbackError.clear();
    s.occupancyResult = SessionQuery::Ok;
    if (q.result != SessionQuery::Ok) {
        s.occupancy = ServerStatus::OccupancyError;
        s.occupancyError = q.error;
        s.occupancyFallbackError = q.fallbackError;
        s.occupancyResult = q.result;
        return;
    }
    s.serverNowUtcMs = q.serverNowUtcMs;
    if (s.serverNowUtcMs <= 0 && previousClock > 0 && now >= previousCheck)
        s.serverNowUtcMs = previousClock + (now - previousCheck);
    s.all = q.sessions;
    // Sessions from this PC are ours (a tab here, or mstsc).
    const QString &self = localComputerName();
    for (RemoteSession &r : s.all) {
        if (r.connected && !q.viaWts && q.serverNowUtcMs <= 0) {
            const auto it = std::find_if(previous.cbegin(), previous.cend(), [&r](const RemoteSession &old) {
                return old.connected && old.id == r.id && old.user.compare(r.user, Qt::CaseInsensitive) == 0
                    && old.client.compare(r.client, Qt::CaseInsensitive) == 0;
            });
            if (it != previous.cend())
                r.connectedSinceUtcMs = it->connectedSinceUtcMs;
        }
        if (!r.connected)
            continue;
        if (r.client.compare(self, Qt::CaseInsensitive) != 0)
            s.others << r;
        else
            s.mine = true;
    }
    s.occupancy = s.others.isEmpty() ? ServerStatus::Free : ServerStatus::Busy;
}

void StatusMonitor::resolveCountries()
{
    QList<QPair<QString, QString>> pending; // id, host
    for (const ServerProfile &p : m_store->all())
        if (p.countryHost != p.host)
            pending.append({p.id, p.host});
    if (pending.isEmpty())
        return;
    QThreadPool::globalInstance()->start([channel = m_channel, pending] {
        QList<QStringList> results;
        for (const auto &[id, host] : pending)
            results.append({id, host, GeoIp::countryOf(host)});
        GeoIp::release();
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, results] {
                for (const QStringList &r : results)
                    target->m_store->setCountry(r[0], r[1], r[2]);
            }, Qt::QueuedConnection);
    });
}

// With an address book in use, a person connecting from a computer that is not in it is reported once.
void StatusMonitor::warnAboutStrangers(const QString &id, const ServerStatus &s)
{
    QSet<QString> present;
    const bool book = !Contacts::instance().all().isEmpty();
    for (const RemoteSession &r : s.others) {
        if (!r.connected || r.client.isEmpty())
            continue;
        const QString key = id + QLatin1Char('|') + r.client.toLower();
        present.insert(key);
        if (!book || Contacts::instance().match(r.client, UsageEngine::accountOf(r.user)) || m_strangers.contains(key))
            continue;
        m_strangers.insert(key);
        emit unknownPerson(m_store->profileById(id).name, r.client, UsageEngine::accountOf(r.user));
    }
    // Once they have left, the next time they connect counts again.
    for (auto it = m_strangers.begin(); it != m_strangers.end();) {
        if (it->startsWith(id + QLatin1Char('|')) && !present.contains(*it))
            it = m_strangers.erase(it);
        else
            ++it;
    }
}

void StatusMonitor::finishProbe(const ServerProfile &p, int latency)
{
    const QString &id = p.id;
    m_probes.remove(id);
    const bool force = m_forcedProbes.remove(id);
    const ServerProfile current = m_store->profileById(id);
    if (!current.isValid())
        return;
    if (!current.sameConnection(p) || current.monitor != p.monitor || current.checkSessions != p.checkSessions) {
        refresh(id, force);
        return;
    }

    ServerStatus s = m_store->status(id);
    const bool online = latency >= 0;
    if (!online && s.state == ServerStatus::Online && !m_retried.contains(id)) {
        // One lost check is not an outage; look again shortly.
        m_retried.insert(id);
        QTimer::singleShot(kRetryDelayMs, this, [this, id, force] { refresh(id, force); });
        return;
    }
    m_retried.remove(id);

    s.state = online ? ServerStatus::Online : ServerStatus::Offline;
    const auto steady = [](int shown, int now) {
        return shown >= 0 && qAbs(now - shown) <= qMax(6, shown / 3) ? shown : now;
    };
    s.latencyMs = online ? steady(s.latencyMs, latency) : latency;
    if (online) {
        const int average = s.avgLatencyMs < 0 ? latency : (s.avgLatencyMs * 3 + latency + 2) / 4;
        s.avgLatencyMs = steady(s.avgLatencyMs, average);
    } else {
        s.avgLatencyMs = -1;
    }

    const bool checkSessions = online && canCheckSessions(current);
    if (!checkSessions) {
        s.occupancy = ServerStatus::OccupancyUnknown;
        s.others.clear();
        s.all.clear();
        s.mine = false;
        s.occupancyError.clear();
        s.occupancyFallbackError.clear();
        s.occupancyResult = SessionQuery::Ok;
        s.checkedAt = 0;
        s.serverNowUtcMs = 0;
        m_sessionTimesAt.remove(id);
    }
    m_store->setStatus(id, s);

    if (!checkSessions)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - s.checkedAt < sessionRetryMs(s, m_store->rowCount()))
        return;
    const qint64 interval = missingSessionTimes(s) ? kMissingSessionTimesEveryMs : kSessionTimesEveryMs;
    const bool includeTimes = !s.all.isEmpty() && now - m_sessionTimesAt.value(id) >= interval;
    requestSessions(current, includeTimes);
}

void StatusMonitor::refreshEvents(const QString &serverId)
{
    for (const ServerProfile &p : m_store->all()) {
        if (!serverId.isEmpty() && p.id != serverId)
            continue;
        if (p.monitor && canCheckSessions(p))
            pollEvents(p, true);
        else
            emit eventSyncChanged(p.id, p.name, false, {},
                                  tr("Server monitoring and a saved account are required."));
    }
}

void StatusMonitor::pollEvents(const ServerProfile &p, bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_eventPolls.contains(p.id) || (!force && now - m_eventsAt.value(p.id) < kEventsEveryMs)
        || !AccessLog::serverEventsEnabled())
        return;

    m_eventPolls.insert(p.id);
    emit eventSyncChanged(p.id, p.name, true, {}, {});
    m_eventPool.start([channel = m_channel, p] {
        const QHash<QString, EventCursor> cursors = AccessLog::cursors(p.id);
        const SessionQuery q = WinRm::querySessions(p.host, p.user, p.domain, p.password, kPollSessionTimeoutMs, &cursors);
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, p, q] { target->finishEventPoll(p, q); }, Qt::QueuedConnection);
    });
}

void StatusMonitor::finishEventPoll(const ServerProfile &p, const SessionQuery &q)
{
    const ServerProfile current = m_store->profileById(p.id);
    if (!current.isValid() || !current.sameConnection(p)) {
        m_eventPolls.remove(p.id);
        m_eventsAt.remove(p.id);
        emit eventSyncChanged(p.id, p.name, false, {}, tr("Server settings changed during the read."));
        return;
    }
    if (q.result != SessionQuery::Ok) {
        m_eventPolls.remove(p.id);
        m_eventsAt.insert(p.id, QDateTime::currentMSecsSinceEpoch());
        emit eventSyncChanged(p.id, p.name, false, {}, q.error.isEmpty() ? tr("The server logs could not be read.") : q.error);
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (q.serverNowUtcMs > 0)
        m_serverClockOffsetMs.insert(p.id, q.serverNowUtcMs - now);
    const qint64 sampledAt = q.serverNowUtcMs > 0
        ? q.serverNowUtcMs
        : (m_serverClockOffsetMs.contains(p.id) ? now + m_serverClockOffsetMs.value(p.id) : 0);
    if (q.sessionsRead)
        m_usageAt.insert(p.id, now);

    m_eventPool.start([channel = m_channel, p, q, sampledAt] {
        if (q.eventsSkipped > 0)
            Log::write(QStringLiteral("[events] %1: %2 event(s) of its logs could not be read and were skipped")
                           .arg(p.name).arg(q.eventsSkipped));
        QList<AccessRecord> written;
        const int saved = AccessLog::polled(p, q.events, q.sessions, &written, q.failedEventSources);
        UsageLog::recordEvents(written);
        if (q.sessionsRead)
            UsageLog::sampled(p, q.sessions, sampledAt);
        const QList<AccessRecord> alerts = Threats::evaluate(p, written);
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, p, q, saved, alerts] {
                target->m_eventPolls.remove(p.id);
                target->m_eventsAt.insert(p.id, QDateTime::currentMSecsSinceEpoch());
                const bool more = q.moreEvents && saved > 0;
                const QString error = saved < 0 ? tr("Could not save the activity history.")
                    : q.eventsSkipped > 0 ? tr("%1 event records could not be read.").arg(q.eventsSkipped) : QString();
                emit target->eventSyncChanged(p.id, p.name, more, q.failedEventSources.values(), error);
                for (const AccessRecord &alert : alerts)
                    emit target->alertRaised(alert);
                if (more) {
                    QTimer::singleShot(750, target, [target, p] {
                        const ServerProfile current = target->m_store->profileById(p.id);
                        if (current.isValid() && current.monitor && canCheckSessions(current) && current.sameConnection(p))
                            target->pollEvents(current, true);
                        else
                            emit target->eventSyncChanged(p.id, p.name, false, {}, {});
                    });
                }
            }, Qt::QueuedConnection);
    }, kForcedPriority);
}
