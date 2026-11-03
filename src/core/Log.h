#pragma once
#include <QString>

// Appends timestamped lines to rconnect.log next to servers.json. Never pass secrets.
namespace Log {

void write(const QString &line);
// Only written while detailed logging is on (Settings > Detailed logging): connection polling,
// COM events and the like.
void detail(const QString &line);
bool detailed();
void setDetailed(bool on);
QString path();

} // namespace Log
