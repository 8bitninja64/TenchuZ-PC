#pragma once
// Keyboard and mouse as controller 1, bound in TenchuZ.ini [keyboard].
#include <windows.h>
#include <cstdint>
#include <string>

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

// For the F1 menu: rereads the [keyboard] bindings from the settings, frees the
// cursor (window thread), names a virtual key or mouse button as the settings file
// spells it ("" when it has no name), and lists the bindable controls.
void reloadKeyBindings();
void releaseMouse();
std::string keyName(int virtualKey);
const char* const* keyboardControls(int& count);

}  // namespace tz::host
