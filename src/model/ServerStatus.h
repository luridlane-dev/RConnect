#pragma once
#include <QColor>
#include <QCoreApplication>
#include <QString>

struct ServerStatus {
    enum State { Unknown, Checking, Offline, Online, Disabled };

    State state = Unknown;

    bool operator==(const ServerStatus &o) const { return state == o.state; }
};

inline QColor statusColor(ServerStatus::State s)
{
    switch (s) {
    case ServerStatus::Online:   return QColor(0x3f, 0xa8, 0x74);
    case ServerStatus::Offline:  return QColor(0xc4, 0x50, 0x4c);
    default:                     return QColor(0x80, 0x80, 0x80);
    }
}

inline QString statusText(const ServerStatus &s)
{
    switch (s.state) {
    case ServerStatus::Unknown:
    case ServerStatus::Checking: return QCoreApplication::translate("ServerStatus", "Checking…");
    case ServerStatus::Disabled: return QCoreApplication::translate("ServerStatus", "Status check off");
    case ServerStatus::Offline:  return QCoreApplication::translate("ServerStatus", "Offline");
    case ServerStatus::Online:   return QCoreApplication::translate("ServerStatus", "Online");
    }
    return {};
}
