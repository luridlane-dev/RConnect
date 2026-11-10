#pragma once
#include "WinRm.h"

// Second way to see who is signed in on a server, for when WinRM is not available (not switched on, blocked by
// the firewall, no answer): Windows' own Remote Desktop Services management calls (the ones "quser /server:"
// uses), over the file-sharing port (445). It signs in to the server with the saved administrator account first.
//
// It lists the sessions only. The server's event logs are not read this way, so the history does not grow from it.
namespace WtsQuery {

// Blocking. result is Ok with the sessions, or Failed with the reason in `error`.
// A server that failed is not tried again for a few minutes, so a server that cannot be reached this way does not
// keep a thread busy on every check.
SessionQuery sessions(const QString &host, const QString &user, const QString &domain, const QString &password,
                      int timeoutMs);

} // namespace WtsQuery
