#pragma once
// Trainer cheats (F1 > Cheats). Toggles act on the title's thread: applyFrame() runs once
// per frame from VdSwap and keeps poked values in place, and the overrides and mid-asm
// hooks of the cheats that change game code (kernel/cheats.cpp) check them. Actions
// (buttons) run at the next frame. Nothing is saved: every start begins with all cheats off.
// The control file's "cheat <id> on|off" command sets them in automated runs.
#include <string>

namespace tz::cheats {

struct Info {
    const char* id;    // short name for the control file
    const char* name;
    const char* help;
    bool action;  // a one-time action (button) rather than a toggle
};

int count();
const Info& info(int cheat);
int find(const std::string& id);  // -1 when unknown
bool enabled(int cheat);
// Toggles a cheat; for an action, asks for it to run at the next frame.
void set(int cheat, bool on);
// The result of the last action (shown under the buttons).
std::string message();
// The title's thread, once per frame.
void applyFrame();

}  // namespace tz::cheats
