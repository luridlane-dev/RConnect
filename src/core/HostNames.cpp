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

constexpr qsizetype kMaxCachedNames = 4096;

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
    const wchar_t *whost = reinterpret_cast<const wchar_t *>(address.utf16());
    sockaddr_storage storage{};
    int length = 0;
    sockaddr_in *v4 = reinterpret_cast<sockaddr_in *>(&storage);
    sockaddr_in6 *v6 = reinterpret_cast<sockaddr_in6 *>(&storage);
    if (InetPtonW(AF_INET, whost, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        length = sizeof(sockaddr_in);
    } else if (InetPtonW(AF_INET6, whost, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        length = sizeof(sockaddr_in6);
    } else {
        return {};
    }
    wchar_t host[NI_MAXHOST] = {};
    if (GetNameInfoW(reinterpret_cast<sockaddr *>(&storage), length, host, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) != 0)
        return {};
    return QString::fromWCharArray(host);
}

void startLookup(const QString &address)
{
    HostNames::Notifier *signaller = HostNames::notifier(); // made here, on the calling (GUI) thread
    pool().start([address, signaller] {
        const QString name = lookup(address);
        {
            QMutexLocker lock(&g_mutex);
            if (g_names.size() >= kMaxCachedNames) {
                g_names.removeIf([](QHash<QString, QString>::iterator it) { return it.value().isEmpty(); });
                if (g_names.size() >= kMaxCachedNames)
                    g_names.clear();
            }
            g_names.insert(address, name);
            g_pending.remove(address);
        }
        if (!name.isEmpty())
            QMetaObject::invokeMethod(signaller, &HostNames::Notifier::resolved, Qt::QueuedConnection);
    });
}

} // namespace

namespace HostNames {

bool isLocalAddress(const QString &address)
{
    const QStringView a = QStringView(address).trimmed();
    if (a.size() < 4)
        return false;
    if (a.contains(QLatin1Char(':'))) {
        return a.startsWith(u"fe80:", Qt::CaseInsensitive)
            || a.startsWith(u"fd", Qt::CaseInsensitive)
            || a.startsWith(u"fc", Qt::CaseInsensitive);
    }
    int octets[4] = {};
    int part = 0;
    int val = 0;
    int digits = 0;
    for (QChar ch : a) {
        const ushort u = ch.unicode();
        if (u >= '0' && u <= '9') {
            val = val * 10 + (u - '0');
            if (++digits > 3 || val > 255)
                return false;
        } else if (u == '.') {
            if (digits == 0 || part >= 3)
                return false;
            octets[part++] = val;
            val = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    if (part != 3 || digits == 0)
        return false;
    octets[3] = val;
    const int first = octets[0], second = octets[1];
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
    startLookup(address);
}

QString cachedOrRequest(const QString &address)
{
    if (address.isEmpty())
        return {};
    {
        QMutexLocker lock(&g_mutex);
        const auto it = g_names.constFind(address);
        if (it != g_names.cend())
            return it.value();
        if (g_pending.contains(address) || g_pending.size() > 400)
            return {};
        g_pending.insert(address);
    }
    startLookup(address);
    return {};
}

Notifier *notifier()
{
    static Notifier *n = new Notifier; // created on first use from the GUI thread, and kept
    return n;
}

} // namespace HostNames
