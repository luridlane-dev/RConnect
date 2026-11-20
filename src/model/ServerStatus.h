#pragma once
#include "core/Contacts.h"
#include "core/WinRm.h"
#include "ui/Theme.h"

#include <QColor>
#include <QCoreApplication>
#include <QString>
#include <QStringList>

struct ServerStatus {
    enum State { Unknown, Checking, Offline, Online, Disabled };
    // Whether someone else is connected, from the WinRM session check.
    enum Occupancy { OccupancyUnknown, Free, Busy, OccupancyError };

    State state = Unknown;
    Occupancy occupancy = OccupancyUnknown;
    QList<RemoteSession> others; // connected sessions from other computers
    QList<RemoteSession> all;    // every session on the server, for the live session list
    bool mine = false;           // this PC has a connected session (a tab here, or mstsc)
    QString occupancyError;
    QString occupancyFallbackError;
    SessionQuery::Result occupancyResult = SessionQuery::Ok; // why occupancy is OccupancyError
    qint64 checkedAt = 0;       // ms since epoch of the last session check
    qint64 serverNowUtcMs = 0;  // remote clock at the last successful session check
    int latencyMs = -1;          // last TCP connect time to the RDP port
    int avgLatencyMs = -1;       // smoothed over the last few checks

    bool operator==(const ServerStatus &) const = default;
};

inline QColor statusColor(const ServerStatus &s)
{
    switch (s.state) {
    case ServerStatus::Online:
        if (s.occupancy == ServerStatus::Busy)
            return Theme::kBusy;
        if (s.occupancy == ServerStatus::OccupancyError)
            return Theme::kOrange; // we are blind here: never look like "free"
        return s.mine ? Theme::kBlue : Theme::kGreen;
    case ServerStatus::Offline:  return Theme::kLabelTertiary;
    default:                     return Theme::kLabelSecondary;
    }
}

// 0 (no signal) to 4, from the smoothed connect time.
inline int signalBars(const ServerStatus &s)
{
    if (s.state != ServerStatus::Online || s.avgLatencyMs < 0)
        return 0;
    return s.avgLatencyMs < 60 ? 4 : (s.avgLatencyMs < 130 ? 3 : (s.avgLatencyMs < 250 ? 2 : 1));
}

inline QString qualityText(int bars)
{
    switch (bars) {
    case 4:  return QCoreApplication::translate("ServerStatus", "Excellent");
    case 3:  return QCoreApplication::translate("ServerStatus", "Good");
    case 2:  return QCoreApplication::translate("ServerStatus", "Fair");
    case 1:  return QCoreApplication::translate("ServerStatus", "Poor");
    default: return {};
    }
}

inline QString idleText(int minutes)
{
    if (minutes < 0)
        return QCoreApplication::translate("ServerStatus", "Activity not measured");
    if (minutes < 1)
        return QCoreApplication::translate("ServerStatus", "using it right now");
    if (minutes < 60)
        return QCoreApplication::translate("ServerStatus", "not used for %1 min").arg(minutes);
    return QCoreApplication::translate("ServerStatus", "not used for %1 h").arg(minutes / 60);
}

// The nickname from the contacts, else the computer name, or the user name when the computer is unknown.
inline QString sessionOwner(const RemoteSession &s)
{
    const QString nickname = Contacts::instance().nameFor(s.client, s.user);
    if (!nickname.isEmpty())
        return nickname;
    return s.client.isEmpty() ? s.user : s.client;
}

// Short text for the status badge on a card. When someone is connected it is their computer.
inline QString statusBadge(const ServerStatus &s)
{
    // Nothing is written while nothing is wrong: a free server, one that is being checked, one that is not
    // watched. Only an offline server, one nobody can be seen on, and the person using it are worth words.
    switch (s.state) {
    case ServerStatus::Unknown:
    case ServerStatus::Checking:
    case ServerStatus::Disabled: return {};
    case ServerStatus::Offline:  return QCoreApplication::translate("ServerStatus", "Offline");
    case ServerStatus::Online:   break;
    }
    if (s.occupancy == ServerStatus::Busy) {
        QString who = sessionOwner(s.others.first());
        if (s.others.size() > 1)
            who += QStringLiteral(" +%1").arg(s.others.size() - 1);
        return who;
    }
    if (s.occupancy == ServerStatus::OccupancyError) {
        if (s.occupancyResult == SessionQuery::Unreachable)
            return QCoreApplication::translate("ServerStatus", "WinRM connection failed");
        if (s.occupancyResult == SessionQuery::AuthFailed)
            return QCoreApplication::translate("ServerStatus", "WinRM authentication rejected");
        return QCoreApplication::translate("ServerStatus", "Session query failed");
    }
    return {};
}

// A short activity label for a busy card.
inline QString idleShort(int minutes)
{
    if (minutes < 1)
        return QCoreApplication::translate("ServerStatus", "Active");
    return minutes < 60 ? QCoreApplication::translate("ServerStatus", "Idle %1 min").arg(minutes)
                        : QCoreApplication::translate("ServerStatus", "Idle %1 h").arg(minutes / 60);
}

inline qint64 connectionMinutes(const ServerStatus &status, const RemoteSession &session)
{
    if (!session.connected || session.connectedSinceUtcMs <= 0 || status.serverNowUtcMs < session.connectedSinceUtcMs)
        return -1;
    return (status.serverNowUtcMs - session.connectedSinceUtcMs) / 60000;
}

inline QString connectionDurationText(qint64 minutes)
{
    if (minutes < 0)
        return {};
    QString duration;
    if (minutes == 0)
        duration = QCoreApplication::translate("ServerStatus", "<1 min");
    else if (minutes < 60)
        duration = QCoreApplication::translate("ServerStatus", "%1 min").arg(minutes);
    else if (minutes < 24 * 60)
        duration = QCoreApplication::translate("ServerStatus", "%1 h %2 min").arg(minutes / 60).arg(minutes % 60);
    else
        duration = QCoreApplication::translate("ServerStatus", "%1 d %2 h").arg(minutes / (24 * 60)).arg(minutes / 60 % 24);
    return QCoreApplication::translate("ServerStatus", "Connected %1").arg(duration);
}

inline QStringList sessionErrorDetails(const ServerStatus &s)
{
    QStringList details;
    if (s.occupancy != ServerStatus::OccupancyError)
        return details;
    if (!s.occupancyError.isEmpty())
        details << QStringLiteral("WinRM / TCP 5985 · ") + s.occupancyError;
    if (!s.occupancyFallbackError.isEmpty())
        details << QStringLiteral("WTS / SMB TCP 445 · ") + s.occupancyFallbackError;
    else if (s.occupancyResult == SessionQuery::AuthFailed)
        details << QCoreApplication::translate("ServerStatus", "WTS · Skipped after authentication rejection.");
    return details;
}

inline QString statusToolTip(const ServerStatus &s)
{
    if (s.state != ServerStatus::Online)
        return {};
    QStringList parts;
    if (s.latencyMs >= 0)
        parts << QCoreApplication::translate("ServerStatus", "Connection: %1 · %2 ms (average %3 ms)")
                     .arg(qualityText(signalBars(s))).arg(s.latencyMs).arg(s.avgLatencyMs);
    if (s.occupancy == ServerStatus::OccupancyUnknown && !s.mine)
        parts << QCoreApplication::translate("ServerStatus", "The server answers, but who is connected could not be read.");
    if (s.occupancy == ServerStatus::OccupancyError) {
        parts << sessionErrorDetails(s).join(QLatin1Char('\n'));
    } else if (s.occupancy == ServerStatus::Busy) {
        QStringList lines;
        for (const RemoteSession &r : s.others) {
            QString line = QCoreApplication::translate("ServerStatus", "%1 (user %2), %3")
                               .arg(r.client.isEmpty() ? QStringLiteral("?") : r.client, r.user, idleText(r.idleMinutes));
            const QString age = connectionDurationText(connectionMinutes(s, r));
            if (!age.isEmpty())
                line += QStringLiteral(" · ") + age;
            lines << line;
        }
        parts << QCoreApplication::translate("ServerStatus", "Connected now:") + QLatin1Char('\n')
                     + lines.join(QLatin1Char('\n'));
    }
    return parts.join(QStringLiteral("\n\n"));
}
