#pragma once
#include "AccessLog.h"

#include <QList>

struct ServerProfile;

// Looks at new access events for signs of an attack and writes a warning line for each (see
// AccessLog::alert):
//  - AlertBruteForce:  many failed sign-ins from one address in a short time
//  - AlertBreakIn:     a sign-in from an address that had just failed several times
//  - AlertNewCountry:  an account signs in from a country it was never seen in on that server
//  - AlertSuspicious:  a new service or scheduled task that runs a script or a program from a
//                      temporary folder
// Tampering events (a new account, a new administrator, a cleared log) are always worth a warning
// but are already lines of their own, so they are only returned.
namespace Threats {

// Reads the history the "new country" check compares with; call once at start, off the GUI thread.
void warmUp();

// `events` are the lines just written for `server`. Returns what deserves the user's attention
// right now: events older than a day are only history. The records of the warnings have the
// warning's event name; tampering events come back as they are.
QList<AccessRecord> evaluate(const ServerProfile &server, const QList<AccessRecord> &events);

// Whether an event of the access log is a warning in itself.
bool isTampering(const QString &event);

} // namespace Threats
