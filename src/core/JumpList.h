#pragma once
#include <QList>
#include <QString>

#include <cstdint>

namespace JumpList {

struct Item {
    QString id;
    QString name;
    QString host;
};

// WM_COPYDATA identifier used to forward a Jump List "--connect <serverId>" click to the running instance.
inline constexpr uintptr_t kCopyDataConnect = 0x52434F4Eu;

// Sets the explicit Windows AppUserModelID so the taskbar button and Jump List stay tied together.
void initAppId(bool testMode);

// Publishes the "Recent servers" custom category on the Windows taskbar right-click menu.
void update(const QString &categoryTitle, const QList<Item> &items);

} // namespace JumpList
