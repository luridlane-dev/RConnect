#pragma once
#include <QString>

// Appends timestamped lines to rconnect.log next to servers.json. Never pass secrets.
namespace Log {

void write(const QString &line);
QString path();

} // namespace Log
