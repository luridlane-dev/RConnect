#pragma once
#include <QString>

// Offline IP-to-country lookup (DB-IP Lite data embedded as :/geoip.bin).
namespace GeoIp {
// Lower-case ISO code ("us"), or empty for private, unknown or unresolvable hosts.
// Resolves host names, so it blocks; call it off the GUI thread.
QString countryOf(const QString &host);
// Frees the table until the next lookup.
void release();
// Country name in the current UI language, falling back to English.
QString countryName(const QString &code);
}
