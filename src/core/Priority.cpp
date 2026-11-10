#include "Priority.h"

#include <windows.h>

namespace {

bool g_active = true;
bool g_live = false;
bool g_yielding = false;
bool g_applied = false;

constexpr ULONG kIgnoreTimerResolution = 0x4; // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION (Windows 11)

void apply()
{
    const bool yield = !g_active && !g_live;
    if (g_applied && yield == g_yielding)
        return;
    g_applied = true;
    g_yielding = yield;

    const HANDLE self = GetCurrentProcess();
    SetPriorityClass(self, yield ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    // Windows 11 also slows a program on its own once its window is covered or minimised (efficiency mode, coarse
    // timers). Not yielding means opting out of that explicitly; yielding leaves the choice to Windows.
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | kIgnoreTimerResolution;
    state.StateMask = yield ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(self, ProcessPowerThrottling, &state, sizeof(state));
}

} // namespace

namespace Priority {

void setApplicationActive(bool active)
{
    g_active = active;
    apply();
}

void setLiveSessions(bool live)
{
    g_live = live;
    apply();
}

} // namespace Priority
