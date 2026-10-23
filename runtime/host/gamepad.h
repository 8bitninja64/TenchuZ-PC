#pragma once
// Game controllers through SDL3: Xbox, PlayStation, Switch, 8BitDo and other
// pads, mapped by position onto the Xbox 360 layout. Controller N of the title
// is the Nth connected pad, so the first pad is always player 1 whatever its slot.
#include "host/input.h"

namespace tz::host {

// State of the title's controller `user` (0-3); false when no pad is there.
bool gamepadState(uint32_t user, VirtualPad& out);
// Motor speeds 0-65535 (XINPUT_VIBRATION); false when no pad is there.
bool gamepadRumble(uint32_t user, uint16_t left, uint16_t right);

}  // namespace tz::host
