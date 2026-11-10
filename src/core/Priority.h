#pragma once

// How hard Windows may throttle RConnect.
//
// While a Remote Desktop session is connected the process is never slowed down, whichever window has the focus:
// the picture and above all the sound of a session in the background would stutter or drop out. Only a program
// with no connected session gives way to the window in front (a game, say).
namespace Priority {

void setApplicationActive(bool active);
void setLiveSessions(bool live);

} // namespace Priority
