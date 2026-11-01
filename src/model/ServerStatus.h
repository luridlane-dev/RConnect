#pragma once
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
    qint64 checkedAt = 0;        // ms since epoch of the last session check
    int latencyMs = -1;          // last TCP connect time to the RDP port
    int avgLatencyMs = -1;       // smoothed over the last few checks

    bool operator==(const ServerStatus &) const = default;
};

inline QColor statusColor(const ServerStatus &s)
{
    switch (s.state) {
    case ServerStatus::Online:
        if (s.occupancy == ServerStatus::Busy)
            return Theme::kOrange;
        return s.mine ? Theme::kBlue : Theme::kGreen;
    case ServerStatus::Offline:  return Theme::kRed;
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
    if (minutes < 1)
        return QCoreApplication::translate("ServerStatus", "using it right now");
    if (minutes < 60)
        return QCoreApplication::translate("ServerStatus", "not used for %1 min").arg(minutes);
    return QCoreApplication::translate("ServerStatus", "not used for %1 h").arg(minutes / 60);
}

// Computer name, or the user name when the computer is unknown.
inline QString sessionOwner(const RemoteSession &s)
{
    return s.client.isEmpty() ? s.user : s.client;
}

// Short text for the status badge on a card. When someone is connected it is their computer.
inline QString statusBadge(const ServerStatus &s)
{
    switch (s.state) {
    case ServerStatus::Unknown:
    case ServerStatus::Checking: return QCoreApplication::translate("ServerStatus", "Checking…");
    case ServerStatus::Disabled: return QCoreApplication::translate("ServerStatus", "Status check off");
    case ServerStatus::Offline:  return QCoreApplication::translate("ServerStatus", "Offline");
    case ServerStatus::Online:   break;
    }
    if (s.occupancy == ServerStatus::Busy) {
        QString who = sessionOwner(s.others.first());
        if (s.others.size() > 1)
            who += QStringLiteral(" +%1").arg(s.others.size() - 1);
        return who; // the second badge on the card says whether they are at the computer
    }
    if (s.mine)
        return QCoreApplication::translate("ServerStatus", "You are connected");
    return s.occupancy == ServerStatus::Free ? QCoreApplication::translate("ServerStatus", "Free")
                                             : QCoreApplication::translate("ServerStatus", "Online");
}

// For the second badge of a busy card: whether the person is at the computer, in plain words.
inline QString idleShort(int minutes)
{
    if (minutes < 1)
        return QCoreApplication::translate("ServerStatus", "Using it now");
    return minutes < 60 ? QCoreApplication::translate("ServerStatus", "Not used for %1 min").arg(minutes)
                        : QCoreApplication::translate("ServerStatus", "Not used for %1 h").arg(minutes / 60);
}

inline QString statusToolTip(const ServerStatus &s)
{
    if (s.state != ServerStatus::Online)
        return {};
    QStringList parts;
    if (s.latencyMs >= 0)
        parts << QCoreApplication::translate("ServerStatus", "Connection: %1 · %2 ms (average %3 ms)")
                     .arg(qualityText(signalBars(s))).arg(s.latencyMs).arg(s.avgLatencyMs);
    if (s.occupancy == ServerStatus::OccupancyError) {
        parts << QCoreApplication::translate("ServerStatus", "Could not check who is connected: %1")
                     .arg(s.occupancyError);
    } else if (s.occupancy == ServerStatus::Busy) {
        QStringList lines;
        for (const RemoteSession &r : s.others)
            lines << QCoreApplication::translate("ServerStatus", "%1 (user %2), %3")
                         .arg(r.client.isEmpty() ? QStringLiteral("?") : r.client, r.user, idleText(r.idleMinutes));
        parts << QCoreApplication::translate("ServerStatus", "Connected now:") + QLatin1Char('\n')
                     + lines.join(QLatin1Char('\n'));
    }
    return parts.join(QStringLiteral("\n\n"));
}
