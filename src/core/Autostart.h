#pragma once

// Start with Windows (per-user Run key). The entry passes --autostart so the app opens minimized.
namespace Autostart {
bool isEnabled();
void setEnabled(bool on);
// Moves the entry along when the exe was moved or updated.
void refresh();
}
