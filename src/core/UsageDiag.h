#pragma once
#include <QString>

struct ServerProfile;

// Read-only dump of what a server says about its sessions: clock, quser, qwinsta, the newest raw
// session events and the log state. For checking the usage log by hand.
namespace UsageDiag {

QString report(const ServerProfile &server);

} // namespace UsageDiag
