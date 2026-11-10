#include "HostNames.h"

#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QThreadPool>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace {

QMutex g_mutex;
QHash<QString, QString> g_names; // address -> name, empty = looked up, none found
QSet<QString> g_pending;

QThreadPool &pool()
{
    static QThreadPool *p = [] {
        auto *pp = new QThreadPool;
        pp->setMaxThreadCount(3);
        return pp;
    }();
    return *p;
}

// The name the address resolves to, or empty.
QString lookup(const QString &address)
{
    const std::string ip = address.toStdString();
    sockaddr_storage storage{};
    int length = 0;
    sockaddr_in *v4 = reinterpret_cast<sockaddr_in *>(&storage);
    sockaddr_in6 *v6 = reinterpret_cast<sockaddr_in6 *>(&storage);
    if (InetPtonA(AF_INET, ip.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        length = sizeof(sockaddr_in);
    } else if (InetPtonA(AF_INET6, ip.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        length = sizeof(sockaddr_in6);
    } else {
        return {};
    }
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<sockaddr *>(&storage), length, host, sizeof(host), nullptr, 0, NI_NAMEREQD) != 0)
        return {};
    return QString::fromLocal8Bit(host);
}

} // namespace

namespace HostNames {

bool isLocalAddress(const QString &address)
{
    const QString a = address.trimmed().toLower();
    if (a.startsWith(QLatin1String("fe80:")) || a.startsWith(QLatin1String("fd")) || a.startsWith(QLatin1String("fc")))
        return a.contains(QLatin1Char(':'));
    const QStringList parts = a.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return false;
    const int first = parts.at(0).toInt(), second = parts.at(1).toInt();
    return first == 10 || (first == 172 && second >= 16 && second <= 31) || (first == 192 && second == 168)
        || (first == 169 && second == 254) || (first == 100 && second >= 64 && second <= 127);
}

QString cached(const QString &address)
{
    QMutexLocker lock(&g_mutex);
    return g_names.value(address);
}

void request(const QString &address)
{
    if (address.isEmpty())
        return;
    {
        QMutexLocker lock(&g_mutex);
        if (g_names.contains(address) || g_pending.contains(address) || g_pending.size() > 400)
            return;
        g_pending.insert(address);
    }
    Notifier *signaller = notifier(); // made here, on the calling (GUI) thread
    pool().start([address, signaller] {
        const QString name = lookup(address);
        {
            QMutexLocker lock(&g_mutex);
            g_names.insert(address, name);
            g_pending.remove(address);
        }
        if (!name.isEmpty())
            QMetaObject::invokeMethod(signaller, &Notifier::resolved, Qt::QueuedConnection);
    });
}

Notifier *notifier()
{
    static Notifier *n = new Notifier; // created on first use from the GUI thread, and kept
    return n;
}

} // namespace HostNames
