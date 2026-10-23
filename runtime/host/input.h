#pragma once
// Keyboard and mouse as controller 1, bound in TenchuZ.ini [keyboard].
#include <windows.h>
#include <cstdint>

namespace tz::host {

struct VirtualPad {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t thumbs[4] = {};  // LX, LY, RX, RY
};

// Window thread: feed messages; returns true when the message was consumed.
bool handleInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
// Registers raw mouse input for the window.
void initializeInput(HWND window);
// True once a key or mouse button has been used (the pad then counts as connected).
bool keyboardMouseActive();
// Current state; mouse motion since the previous call becomes right stick deflection.
VirtualPad keyboardMouseState();
// Combines a controller state with the keyboard/mouse one.
void mergePad(VirtualPad& into, const VirtualPad& other);

}  // namespace tz::host
