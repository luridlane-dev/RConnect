#include "StatusMonitor.h"
#include "GeoIp.h"
#include "model/ServerStore.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QThreadPool>

#include <chrono>
#include <mutex>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace {

constexpr int kPollIntervalMs = 60000;
constexpr int kConnectTimeoutMs = 2000;
// Checks are spread over this window instead of firing at once.
constexpr int kSpreadMs = 20000;
constexpr int kSessionTimeoutMs = 8000;
// At most this many checks run at once, so a long list never opens hundreds of sockets.
constexpr int kMaxParallelChecks = 8;
// WinRM costs a whole sign-in, so it is asked less often than the port.
constexpr qint64 kSessionMaxAgeMs = 100000;
// A single failed port check is retried before a server is shown as offline.
constexpr int kRetryDelayMs = 4000;

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

QString localComputerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &size) ? QString::fromWCharArray(name, int(size)) : QString();
}

SessionQuery querySessions(const ServerProfile &p)
{
    return WinRm::querySessions(p.host, p.user, p.domain, p.password, kSessionTimeoutMs);
}

} // namespace

struct StatusMonitor::Channel {
    std::mutex mutex;
    StatusMonitor *target = nullptr;
};

StatusMonitor::StatusMonitor(ServerStore *store, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_channel(std::make_shared<Channel>())
{
    m_pool.setMaxThreadCount(kMaxParallelChecks);
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
    m_pool.clear();
}

void StatusMonitor::setActive(bool active)
{
    if (active) {
        if (!m_timer.isActive()) {
            m_timer.start();
            refreshAll();
        }
    } else {
        m_timer.stop();
    }
}

void StatusMonitor::refreshAll()
{
    const QList<ServerProfile> profiles = m_store->all();
    // A short list is checked at once; a long one is spread out.
    const int spread = qBound(200, int(profiles.size()) * 150, kSpreadMs);
    for (const ServerProfile &p : profiles) {
        if (!p.monitor) {
            refresh(p.id); // marks it as disabled
            continue;
        }
        const QString id = p.id;
        const int delay = int(QRandomGenerator::global()->bounded(spread));
        QTimer::singleShot(delay, this, [this, id] { refresh(id); });
    }
}

void StatusMonitor::refresh(const QString &id, bool force)
{
    if (m_inFlight.contains(id))
        return;
    const ServerProfile p = m_store->profileById(id);
    if (!p.isValid())
        return;

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

    m_inFlight.insert(id);
    const bool check = canCheckSessions(p)
        && (force || QDateTime::currentMSecsSinceEpoch() - current.checkedAt >= kSessionMaxAgeMs);
    m_pool.start([channel = m_channel, p, check] {
        const int latency = probe(p.host.toStdWString(), quint16(p.port));
        const SessionQuery q = latency >= 0 && check ? querySessions(p) : SessionQuery{};
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, id = p.id, latency, check, q] {
                target->finished(id, latency, latency >= 0 && check, q);
            }, Qt::QueuedConnection);
    });
}

bool StatusMonitor::canCheckSessions(const ServerProfile &p)
{
    return p.checkSessions && !p.user.isEmpty() && !p.password.isEmpty();
}

void StatusMonitor::checkSessions(const ServerProfile &p, std::function<void(const ServerStatus &)> done)
{
    QThreadPool::globalInstance()->start([channel = m_channel, p, done] {
        const SessionQuery q = querySessions(p);
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, id = p.id, q, done] {
                ServerStatus s = target->m_store->status(id);
                applySessions(s, q);
                if (q.result == SessionQuery::Ok)
                    s.state = ServerStatus::Online;
                target->m_store->setStatus(id, s);
                done(s);
            }, Qt::QueuedConnection);
    });
}

void StatusMonitor::applySessions(ServerStatus &s, const SessionQuery &q)
{
    s.checkedAt = QDateTime::currentMSecsSinceEpoch();
    s.others.clear();
    s.occupancyError.clear();
    if (q.result != SessionQuery::Ok) {
        s.occupancy = ServerStatus::OccupancyError;
        s.occupancyError = q.error;
        return;
    }
    // Sessions from this PC are ours (a tab here, or mstsc).
    const QString self = localComputerName();
    for (const RemoteSession &r : q.sessions)
        if (r.connected && r.client.compare(self, Qt::CaseInsensitive) != 0)
            s.others << r;
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

void StatusMonitor::finished(const QString &id, int latency, bool checked, const SessionQuery &q)
{
    m_inFlight.remove(id);
    if (!m_store->profileById(id).isValid())
        return;
    ServerStatus s = m_store->status(id);
    const bool online = latency >= 0;

    if (!online && s.state == ServerStatus::Online && !m_retried.contains(id)) {
        // One lost check is not an outage; look again shortly.
        m_retried.insert(id);
        QTimer::singleShot(kRetryDelayMs, this, [this, id] { refresh(id); });
        return;
    }
    m_retried.remove(id);

    s.state = online ? ServerStatus::Online : ServerStatus::Offline;
    s.latencyMs = latency;
    if (online)
        s.avgLatencyMs = s.avgLatencyMs < 0 ? latency : (s.avgLatencyMs * 3 + latency + 2) / 4;
    else
        s.avgLatencyMs = -1;

    if (!online || !canCheckSessions(m_store->profileById(id))) {
        s.occupancy = ServerStatus::OccupancyUnknown;
        s.others.clear();
        s.occupancyError.clear();
        s.checkedAt = 0;
    } else if (checked) {
        applySessions(s, q);
    } // otherwise the last answer is still fresh
    m_store->setStatus(id, s);
}
