#include "GeoIp.h"
#include "Language.h"

#include <QByteArray>
#include <QFile>
#include <QLocale>

#include <algorithm>
#include <cstring>
#include <mutex>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace {

std::mutex g_mutex;
QByteArray g_data;

struct Table {
    const quint32 *starts = nullptr;
    const char *codes = nullptr;
    quint32 count = 0;

    QString find(quint32 key) const
    {
        const quint32 *end = starts + count;
        const quint32 *it = std::upper_bound(starts, end, key);
        if (it == starts)
            return {};
        const char *cc = codes + 2 * (it - starts - 1);
        if (cc[0] == '-')
            return {};
        return QString::fromLatin1(cc, 2).toLower();
    }
};

bool tables(Table &v4, Table &v6)
{
    if (g_data.isEmpty()) {
        QFile f(QStringLiteral(":/geoip.bin"));
        if (!f.open(QIODevice::ReadOnly))
            return false;
        g_data = f.readAll();
    }
    const char *p = g_data.constData();
    if (g_data.size() < 16 || std::memcmp(p, "RCGEO2", 6) != 0)
        return false;
    quint32 n4, n6;
    std::memcpy(&n4, p + 8, 4);
    std::memcpy(&n6, p + 12, 4);
    if (qsizetype(16 + 6ull * n4 + 6ull * n6) > g_data.size())
        return false;
    const auto *starts = reinterpret_cast<const quint32 *>(p + 16); // QByteArray data is 8-byte aligned
    const char *codes = p + 16 + 4 * (n4 + n6);
    v4 = {starts, codes, n4};
    v6 = {starts + n4, codes + 2 * n4, n6};
    return true;
}

bool isPrivate(quint32 ip)
{
    const quint8 a = quint8(ip >> 24), b = quint8(ip >> 16);
    return a == 10 || a == 127 || a == 0 || (a == 169 && b == 254) || (a == 172 && (b & 0xf0) == 16)
        || (a == 192 && b == 168) || (a == 100 && (b & 0xc0) == 64);
}

} // namespace

namespace GeoIp {

QString countryOf(const QString &host)
{
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    ADDRINFOW *results = nullptr;
    if (GetAddrInfoW(reinterpret_cast<const wchar_t *>(host.utf16()), nullptr, &hints, &results) != 0)
        return {};

    bool have4 = false, have6 = false;
    quint32 ip4 = 0, ip6 = 0;
    for (ADDRINFOW *a = results; a; a = a->ai_next) {
        if (a->ai_family == AF_INET && !have4) {
            ip4 = ntohl(reinterpret_cast<sockaddr_in *>(a->ai_addr)->sin_addr.s_addr);
            have4 = true;
        } else if (a->ai_family == AF_INET6 && !have6) {
            const quint8 *b = reinterpret_cast<sockaddr_in6 *>(a->ai_addr)->sin6_addr.s6_addr;
            ip6 = quint32(b[0]) << 24 | quint32(b[1]) << 16 | quint32(b[2]) << 8 | b[3];
            have6 = true;
        }
    }
    FreeAddrInfoW(results);

    std::lock_guard lock(g_mutex);
    Table v4, v6;
    if (!tables(v4, v6))
        return {};
    if (have4)
        return isPrivate(ip4) ? QString() : v4.find(ip4);
    if (have6)
        return v6.find(ip6);
    return {};
}

void release()
{
    std::lock_guard lock(g_mutex);
    g_data = QByteArray();
}

QString countryName(const QString &code)
{
    const QLocale::Territory territory = QLocale::codeToTerritory(code);
    if (territory == QLocale::AnyTerritory)
        return code.toUpper();
    if (Language::current() == QLatin1String("tr")) {
        const QLocale local(QLocale::Turkish, territory);
        if (local.territory() == territory && local.language() == QLocale::Turkish)
            return local.nativeTerritoryName();
    }
    return QLocale::territoryToString(territory);
}

} // namespace GeoIp
