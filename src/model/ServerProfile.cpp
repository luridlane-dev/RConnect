#include "ServerProfile.h"
#include "core/Secrets.h"

#include <QUuid>

QString ServerProfile::displayUser() const
{
    if (user.isEmpty())
        return {};
    return domain.isEmpty() ? user : domain + QLatin1Char('\\') + user;
}

QSize ServerProfile::fixedResolution() const
{
    const int x = resolution.indexOf(QLatin1Char('x'));
    if (x <= 0)
        return {};
    const QSize s(resolution.left(x).toInt(), resolution.mid(x + 1).toInt());
    return s.width() >= 640 && s.height() >= 480 ? s : QSize();
}

QJsonObject ServerProfile::toJson() const
{
    QJsonObject o;
    o[QLatin1String("id")] = id;
    o[QLatin1String("name")] = name;
    o[QLatin1String("host")] = host;
    o[QLatin1String("port")] = port;
    o[QLatin1String("user")] = user;
    o[QLatin1String("domain")] = domain;
    o[QLatin1String("savePassword")] = savePassword;
    if (savePassword)
        o[QLatin1String("password")] = Secrets::protect(password);
    o[QLatin1String("color")] = color;
    if (!notes.isEmpty())
        o[QLatin1String("notes")] = Secrets::protect(notes);
    o[QLatin1String("country")] = country;
    o[QLatin1String("countryHost")] = countryHost;
    o[QLatin1String("colorDepth")] = colorDepth;
    o[QLatin1String("resolution")] = resolution;
    o[QLatin1String("clipboard")] = clipboard;
    o[QLatin1String("drives")] = drives;
    o[QLatin1String("audio")] = audio;
    o[QLatin1String("ignoreCertWarnings")] = ignoreCertWarnings;
    o[QLatin1String("monitor")] = monitor;
    return o;
}

ServerProfile ServerProfile::fromJson(const QJsonObject &o)
{
    ServerProfile p;
    p.id = o.value(QLatin1String("id")).toString();
    if (p.id.isEmpty())
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = o.value(QLatin1String("name")).toString();
    p.host = o.value(QLatin1String("host")).toString();
    p.port = o.value(QLatin1String("port")).toInt(3389);
    p.user = o.value(QLatin1String("user")).toString();
    p.domain = o.value(QLatin1String("domain")).toString();
    p.savePassword = o.value(QLatin1String("savePassword")).toBool(true);
    if (p.savePassword)
        p.password = Secrets::unprotect(o.value(QLatin1String("password")).toString());
    p.color = o.value(QLatin1String("color")).toString(QStringLiteral("#3a96dd"));
    p.notes = Secrets::unprotect(o.value(QLatin1String("notes")).toString());
    p.country = o.value(QLatin1String("country")).toString();
    p.countryHost = o.value(QLatin1String("countryHost")).toString();
    p.colorDepth = o.value(QLatin1String("colorDepth")).toInt(32);
    p.resolution = o.value(QLatin1String("resolution")).toString();
    p.clipboard = o.value(QLatin1String("clipboard")).toBool(true);
    p.drives = o.value(QLatin1String("drives")).toBool(false);
    p.audio = o.value(QLatin1String("audio")).toBool(true);
    p.ignoreCertWarnings = o.value(QLatin1String("ignoreCertWarnings")).toBool(true);
    p.monitor = o.value(QLatin1String("monitor")).toBool(true);
    if (p.name.isEmpty())
        p.name = p.host;
    return p;
}
