#include "StatusMonitor.h"
#include "GeoIp.h"
#include "model/ServerStore.h"

#include <QRandomGenerator>
#include <QThreadPool>

#include <mutex>
#include <string>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace {

constexpr int kPollIntervalMs = 60000;
constexpr int kConnectTimeoutMs = 2000;
// Checks are spread over this window instead of firing at once.
constexpr int kSpreadMs = 20000;

// Connect and close; nothing is sent.
bool probe(const std::wstring &host, quint16 port)
{
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW *results = nullptr;
    if (GetAddrInfoW(host.c_str(), std::to_wstring(port).c_str(), &hints, &results) != 0)
        return false;

    bool online = false;
    for (ADDRINFOW *a = results; a && !online; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
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
        closesocket(s);
    }
    FreeAddrInfoW(results);
    return online;
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
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    m_channel->target = this;
    m_timer.setInterval(kPollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &StatusMonitor::refreshAll);
}

StatusMonitor::~StatusMonitor()
{
    std::lock_guard lock(m_channel->mutex);
    m_channel->target = nullptr;
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
    for (const ServerProfile &p : m_store->all()) {
        if (!p.monitor) {
            refresh(p.id); // marks it as disabled
            continue;
        }
        const QString id = p.id;
        const int delay = int(QRandomGenerator::global()->bounded(kSpreadMs));
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
        m_store->setStatus(id, {ServerStatus::Disabled});
        return;
    }
    if (m_store->status(id).state == ServerStatus::Unknown || m_store->status(id).state == ServerStatus::Disabled)
        m_store->setStatus(id, {ServerStatus::Checking});

    m_inFlight.insert(id);
    const std::wstring host = p.host.toStdWString();
    const quint16 port = quint16(p.port);
    QThreadPool::globalInstance()->start([channel = m_channel, id, host, port] {
        const bool online = probe(host, port);
        std::lock_guard lock(channel->mutex);
        if (StatusMonitor *target = channel->target)
            QMetaObject::invokeMethod(target, [target, id, online] { target->finished(id, online); },
                                      Qt::QueuedConnection);
    });
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

void StatusMonitor::finished(const QString &id, bool online)
{
    m_inFlight.remove(id);
    m_store->setStatus(id, {online ? ServerStatus::Online : ServerStatus::Offline});
}
